/*
 * camera_pipeline.c — OV5647 MIPI CSI-2 → ISP → JPEG pipeline
 *
 * Proven hardware init ported from camera_sensor_stack_probe_RGB.
 * Key chip-rev v1.3 quirks preserved (see inline comments).
 *
 * Resolution path:
 *   800×1280 RAW8 (sensor) → ISP RGB565 → 2× CPU downsample → 400×640 RGB565
 *   → JPEG HW encoder (quality CONFIG_CAM_JPEG_QUALITY) → uvc_fb_t
 *
 * With USB OTG HS the 4× downsample from the serial version is no longer
 * needed.  2× keeps full horizontal crop and halves each axis, giving a
 * 400×640 image with better detail than the old 200×320.
 */

#include "camera_pipeline.h"

#include <math.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ldo_regulator.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_sccb_intf.h"
#include "driver/i2c_master.h"
#include "esp_private/esp_cache_private.h"
#include "esp_cache.h"
#include "driver/isp.h"
#include "driver/isp_demosaic.h"
#include "driver/isp_ccm.h"
#include "driver/isp_gamma.h"
#include "driver/jpeg_encode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "example_sensor_init.h"
#include "usb_device_uvc.h"

static const char *TAG = "cam_pipeline";

/* ── Hardware constants ──────────────────────────────────────────────────── */
#define MIPI_PHY_LDO_CHAN_ID      3
#define MIPI_PHY_LDO_VOLTAGE_MV   2500
#define CAM_SDA_IO                7
#define CAM_SCL_IO                8
/*
 * OV5647  RAW10 1280×960 binning 45fps
 * - Sensor window: x=24..2599, y=12..1943  (~99 % of full 2592×1944 array)
 * - 2:1 horizontal + vertical binning → maximum FOV, best sensitivity
 * - Output: 1280×960  LANDSCAPE  (no software rotation needed)
 * - MIPI 2-lane: IDI clock = 88.333 MHz × 5 = 441.67 Mbps / 2 = ~221 Mbps/lane
 */
#define CAM_HRES                  1280
#define CAM_VRES                  960
#define CAM_LANE_BITRATE_MBPS     221

/*
 * Preview: uniform 2:1 nearest-neighbour downscale → 640×480.
 *
 * 1280×960  ──÷2──►  640×480  (every other pixel, every other row)
 *
 * No rotation needed: the 1280×960 mode already outputs landscape.
 * No distortion: both axes scale by exactly 2.
 * Cache-friendly: sequential row reads from source.
 */
#define PREVIEW_HRES              640
#define PREVIEW_VRES              480

/* Derived sizes */
#define RAW_FRAME_BYTES           (CAM_HRES * CAM_VRES)          /* RAW8  */
#define RGB_FRAME_BYTES           (CAM_HRES * CAM_VRES * 2)      /* RGB565 */
#define PREVIEW_FRAME_BYTES       (PREVIEW_HRES * PREVIEW_VRES * 2)

/* JPEG output budget — 640×480 at quality 80 ≈ 30–100 KB */
#define JPEG_OUT_BUF_SIZE         (200 * 1024)
#define JPEG_QUALITY              80

/* Frame timeout passed to cam_pipeline_capture_jpeg */
#define FRAME_TIMEOUT_MS          5000

/* ── Module-level state ──────────────────────────────────────────────────── */
static SemaphoreHandle_t        s_frame_sem;
static esp_cam_ctlr_trans_t     s_finished_trans;
static esp_cam_ctlr_trans_t     s_cam_trans;

static uint8_t                 *s_frame_bufs[2]; /* ping-pong DMA write buffers (PSRAM) */
static volatile int             s_write_idx;     /* which buf DMA is currently writing */
static uint8_t                 *s_preview_buf;   /* 640×480  RGB565 (PSRAM) */
static uint8_t                 *s_jbuf;          /* JPEG output    (PSRAM)  */
static size_t                   s_jbuf_sz;
static jpeg_encoder_handle_t    s_jpeg_enc;
static jpeg_encode_cfg_t        s_enc_cfg;

/* ── ISR callbacks ───────────────────────────────────────────────────────── */
/*
 * on_get_new_trans: re-arms the DMA with the same user buffer every frame.
 * Returning false means the ISR should not yield — the FreeRTOS scheduler
 * will not be invoked from this path.
 */
static bool IRAM_ATTR s_on_get_new_trans(esp_cam_ctlr_handle_t handle,
                                          esp_cam_ctlr_trans_t *trans,
                                          void *user_data)
{
    /* Ping-pong: alternate to the OTHER buffer so the CPU can safely read the
     * just-completed buffer while DMA writes the next frame to the other one. */
    s_write_idx ^= 1;
    trans->buffer = s_frame_bufs[s_write_idx];
    trans->buflen = RGB_FRAME_BYTES;
    return false;
}

/*
 * on_trans_finished: fires from DMA ISR on every completed frame.
 * Saves the transaction descriptor and wakes the capture task via semaphore.
 * Returns true when the semaphore give caused a higher-priority task to be
 * unblocked, so that the port can yield immediately.
 */
static bool IRAM_ATTR s_on_trans_finished(esp_cam_ctlr_handle_t handle,
                                           esp_cam_ctlr_trans_t *trans,
                                           void *user_data)
{
    s_finished_trans = *trans;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &woken);
    return woken == pdTRUE;
}

/* ── Gamma curve (γ = 0.55) ─────────────────────────────────────────────── */
/*
 * γ < 1 brightens shadows.  0.55 is between sRGB (0.45) and linear (1.0).
 * Applied in ISP hardware for zero CPU overhead.
 */
static uint32_t s_gamma_brighten(uint32_t x)
{
    if (x == 0) return 0;
    return (uint32_t)(powf((float)x / 255.0f, 0.55f) * 255.0f + 0.5f);
}

/* ── 2:1 nearest-neighbour downscale  1280×960 → 640×480 ────────────────── */
/*
 * The 1280×960 binning mode already outputs a landscape image — no rotation
 * needed.  A uniform 2× downscale maps every even pixel and every even row
 * from the source to the destination.
 *
 * Access pattern: sequential row reads → cache-friendly, fast on PSRAM.
 */
static void s_scale_half(const uint8_t *src, uint8_t *dst)
{
    const uint16_t *in  = (const uint16_t *)src;
    uint16_t       *out = (uint16_t *)dst;

    for (int dy = 0; dy < PREVIEW_VRES; dy++) {
        const uint16_t *src_row = in + (size_t)(dy * 2) * CAM_HRES;
        uint16_t       *dst_row = out + (size_t)dy * PREVIEW_HRES;
        for (int dx = 0; dx < PREVIEW_HRES; dx++) {
            dst_row[dx] = src_row[dx * 2];
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t cam_pipeline_init(void)
{
    ESP_LOGI(TAG, "Initialising camera pipeline  %dx%d → %dx%d (2:1 downscale, RAW10 binning)  JPEG q%d",
             CAM_HRES, CAM_VRES, PREVIEW_HRES, PREVIEW_VRES, JPEG_QUALITY);

    /* 1. MIPI PHY LDO ---------------------------------------------------- */
    esp_ldo_channel_handle_t ldo_mipi = NULL;
    esp_ldo_channel_config_t ldo_cfg  = {
        .chan_id    = MIPI_PHY_LDO_CHAN_ID,
        .voltage_mv = MIPI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi));
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2. PSRAM buffers --------------------------------------------------- */
    size_t align = 0;
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align);

    for (int i = 0; i < 2; i++) {
        s_frame_bufs[i] = heap_caps_aligned_alloc(align, RGB_FRAME_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_frame_bufs[i]) {
            ESP_LOGE(TAG, "frame_buf[%d] alloc failed (%u B)", i, (unsigned)RGB_FRAME_BYTES);
            return ESP_FAIL;
        }
    }
    s_write_idx = 0;

    s_preview_buf = heap_caps_aligned_alloc(align, PREVIEW_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_preview_buf) {
        ESP_LOGE(TAG, "preview_buf alloc failed (%u B)", (unsigned)PREVIEW_FRAME_BYTES);
        return ESP_FAIL;
    }

    /* 3. JPEG encoder ---------------------------------------------------- */
    jpeg_encode_engine_cfg_t jeng = { .intr_priority = 0, .timeout_ms = 700 };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&jeng, &s_jpeg_enc));

    jpeg_encode_memory_alloc_cfg_t jmem = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER
    };
    s_jbuf = (uint8_t *)jpeg_alloc_encoder_mem(JPEG_OUT_BUF_SIZE, &jmem, &s_jbuf_sz);
    if (!s_jbuf) {
        ESP_LOGE(TAG, "JPEG buf alloc failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "JPEG buf: %u B", (unsigned)s_jbuf_sz);

    s_enc_cfg = (jpeg_encode_cfg_t){
        .height        = PREVIEW_VRES,
        .width         = PREVIEW_HRES,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
        .pixel_reverse = false,
    };

    /* 4. Frame-ready semaphore ------------------------------------------- */
    s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_sem) {
        ESP_LOGE(TAG, "Semaphore create failed");
        return ESP_FAIL;
    }

    /* DMA starts writing into buf[0]; on_get_new_trans will flip to buf[1] */
    s_cam_trans.buffer = s_frame_bufs[0];
    s_cam_trans.buflen = RGB_FRAME_BYTES;

    /* 5. ISP: RAW8 → RGB565 ----------------------------------------------- */
    /*
     * v1.3 quirk: the CSI bridge (MIPI_CSI_BRG_USER_ISP) only routes data to
     * DMA when ISP is active.  set_color_mode_bypass() is a NO-OP.
     * ISP must be initialised BEFORE the CSI controller.
     */
    isp_proc_handle_t isp_proc = NULL;
    esp_isp_processor_cfg_t isp_cfg = {
        .clk_hz                 = 80 * 1000 * 1000,
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW10,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = false,
        .has_line_end_packet    = false,
        .h_res                  = CAM_HRES,
        .v_res                  = CAM_VRES,
        /* OV5647 native Bayer order on ESP32-P4 — confirmed by esp_cam_sensor v0.7.1 */
        .bayer_order            = COLOR_RAW_ELEMENT_ORDER_GBRG,
    };
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_cfg, &isp_proc));
    ESP_ERROR_CHECK(esp_isp_enable(isp_proc));

    /*
     * v1.3 quirk: demosaic is NOT enabled by esp_isp_enable().
     * Must be explicitly configured + enabled or ISP outputs flat/striped data.
     */
    esp_isp_demosaic_config_t demosaic_cfg = {
        .grad_ratio  = { .val = 0 },
        .padding_mode = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA,
    };
    ESP_ERROR_CHECK(esp_isp_demosaic_configure(isp_proc, &demosaic_cfg));
    ESP_ERROR_CHECK(esp_isp_demosaic_enable(isp_proc));

    /* CCM: gentle indoor correction — slight R boost, mild B reduction */
    esp_isp_ccm_config_t ccm_cfg = {
        .matrix = {
            { 1.4f, -0.2f, -0.2f },
            {-0.1f,  1.1f,  0.0f },
            { 0.0f, -0.1f,  0.9f },
        },
        .saturation = true,
        .flags = { .update_once_configured = true },
    };
    ESP_ERROR_CHECK(esp_isp_ccm_configure(isp_proc, &ccm_cfg));
    ESP_ERROR_CHECK(esp_isp_ccm_enable(isp_proc));

    /* Gamma γ = 0.55 — applied in hardware */
    isp_gamma_curve_points_t gamma_pts = {};
    ESP_ERROR_CHECK(esp_isp_gamma_fill_curve_points(s_gamma_brighten, &gamma_pts));
    ESP_ERROR_CHECK(esp_isp_gamma_configure(isp_proc, COLOR_COMPONENT_R, &gamma_pts));
    ESP_ERROR_CHECK(esp_isp_gamma_configure(isp_proc, COLOR_COMPONENT_G, &gamma_pts));
    ESP_ERROR_CHECK(esp_isp_gamma_configure(isp_proc, COLOR_COMPONENT_B, &gamma_pts));
    ESP_ERROR_CHECK(esp_isp_gamma_enable(isp_proc));

    ESP_LOGI(TAG, "ISP enabled: demosaic GBRG + CCM + gamma(0.55)");

    /* 6. CSI controller -------------------------------------------------- */
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .h_res                  = CAM_HRES,
        .v_res                  = CAM_VRES,
        .lane_bit_rate_mbps     = CAM_LANE_BITRATE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW10,
        .output_data_color_type = CAM_CTLR_COLOR_RAW10,
        .data_lane_num          = 2,
        .byte_swap_en           = false,
        .queue_items            = 1,
    };
    esp_cam_ctlr_handle_t cam = NULL;
    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&csi_cfg, &cam));

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = s_on_get_new_trans,
        .on_trans_finished = s_on_trans_finished,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, &s_cam_trans));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam));
    ESP_LOGI(TAG, "CSI started");

    /* 7. OV5647 sensor --------------------------------------------------- */
    example_sensor_handle_t sensor = { .sccb_handle = NULL, .i2c_bus_handle = NULL };
    example_sensor_config_t scfg = {
        .i2c_port_num   = I2C_NUM_0,
        .i2c_sda_io_num = CAM_SDA_IO,
        .i2c_scl_io_num = CAM_SCL_IO,
        .port           = ESP_CAM_SENSOR_MIPI_CSI,
        .format_name    = "MIPI_2lane_24Minput_RAW10_1280x960_binning_45fps",
    };
    example_sensor_init(&scfg, &sensor);
    if (!sensor.sccb_handle) {
        ESP_LOGE(TAG, "OV5647 not found on I2C — halting");
        return ESP_FAIL;
    }

    /* Restart MIPI stream after ISP/CSI are ready (required on this board) */
    esp_sccb_transmit_reg_a16v8(sensor.sccb_handle, 0x0100, 0x00);
    esp_sccb_transmit_reg_a16v8(sensor.sccb_handle, 0x4800, 0x14);  /* continuous clock */
    vTaskDelay(pdMS_TO_TICKS(5));
    esp_sccb_transmit_reg_a16v8(sensor.sccb_handle, 0x0100, 0x01);  /* stream on */
    ESP_LOGI(TAG, "OV5647 streaming — warming up 1500 ms");
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "Camera pipeline ready");
    return ESP_OK;
}

esp_err_t cam_pipeline_capture_jpeg(uvc_fb_t *fb, uint32_t timeout_ms)
{
    /* Loop until we get a valid-sized frame (skip DMA size mismatches) */
    while (1) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            ESP_LOGW(TAG, "Frame timeout (%lu ms)", (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }

        /*
         * v1.3 quirk: received_size is reported in MIPI input bytes.
         * For RAW8:  1 byte/pixel  → W×H
         * For RAW10: 4px in 5B     → W×H×5/4  (packed)
         *
         * Accept both values to be safe; log on first unexpected size so we
         * can see the actual chip-reported count in the serial monitor.
         */
        {
            size_t rx  = s_finished_trans.received_size;
            size_t raw8_sz  = (size_t)CAM_HRES * CAM_VRES;          /* 1 B/px  */
            size_t raw10_sz = (size_t)CAM_HRES * CAM_VRES * 5 / 4;  /* 1.25B/px*/

            static bool first = true;
            if (first) {
                ESP_LOGI(TAG, "DMA received_size=%u (expected %u or %u)",
                         (unsigned)rx, (unsigned)raw8_sz, (unsigned)raw10_sz);
                first = false;
            }
            if (rx != raw8_sz && rx != raw10_sz) {
                ESP_LOGW(TAG, "DMA unexpected size %u — skip", (unsigned)rx);
                continue;
            }
        }

        break;  /* valid frame */
    }

    /* Cache sync: invalidate CPU cache for the just-completed DMA buffer so
     * we read fresh PSRAM data (DMA bypasses the L1 cache). */
    uint8_t *done_buf = s_finished_trans.buffer;
    esp_cache_msync(done_buf, RGB_FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    /* 2:1 downscale 1280×960 → 640×480 */
    s_scale_half(done_buf, s_preview_buf);

    /* JPEG encode */
    uint32_t  jpeg_size = 0;
    esp_err_t enc_err   = jpeg_encoder_process(s_jpeg_enc, &s_enc_cfg,
                                               s_preview_buf, PREVIEW_FRAME_BYTES,
                                               s_jbuf, s_jbuf_sz, &jpeg_size);
    if (enc_err != ESP_OK || jpeg_size == 0) {
        ESP_LOGE(TAG, "JPEG encode failed: 0x%x  size=%lu", enc_err, (unsigned long)jpeg_size);
        return ESP_FAIL;
    }

    /* Append EOI marker if missing (paranoia) */
    bool has_eoi = (jpeg_size >= 2 &&
                    s_jbuf[jpeg_size - 2] == 0xFF &&
                    s_jbuf[jpeg_size - 1] == 0xD9);
    if (!has_eoi && (jpeg_size + 2 <= s_jbuf_sz)) {
        s_jbuf[jpeg_size]     = 0xFF;
        s_jbuf[jpeg_size + 1] = 0xD9;
        jpeg_size += 2;
    }

    /* Fill UVC frame descriptor — including timestamp (required by host) */
    uint64_t us = (uint64_t)esp_timer_get_time();
    fb->buf              = s_jbuf;
    fb->len              = jpeg_size;
    fb->width            = PREVIEW_HRES;
    fb->height           = PREVIEW_VRES;
    fb->format           = UVC_FORMAT_JPEG;
    fb->timestamp.tv_sec  = (long)(us / 1000000UL);
    fb->timestamp.tv_usec = (long)(us % 1000000UL);

    return ESP_OK;
}
