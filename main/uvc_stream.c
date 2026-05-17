/*
 * uvc_stream.c — USB UVC device stream (esp-iot-solution usb_device_uvc)
 *
 * Implements the four callbacks required by usb_device_uvc and delegates
 * frame capture to cam_pipeline_capture_jpeg().
 *
 * Data flow:
 *   [UVC camera task] → fb_get_cb → cam_pipeline_capture_jpeg()
 *                                       → DMA semaphore wait
 *                                       → cache sync
 *                                       → 2× downsample
 *                                       → JPEG HW encode
 *                                    ← uvc_fb_t{buf, len, w, h, JPEG}
 *                    ← USB bulk/ISO transfer → Host (macOS/Win/Linux webcam)
 *
 * fb_return_cb: the buffer is internally owned by camera_pipeline.c and
 * will be overwritten on the next capture.  No pool management is needed.
 *
 * start_cb / stop_cb: the camera pipeline runs continuously once initialised,
 * so start/stop only log the host connection event.
 */

#include "uvc_stream.h"
#include "camera_pipeline.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb_device_uvc.h"

static const char *TAG = "uvc_stream";

/*
 * UVC transfer buffer: TinyUSB uses this as the USB packet staging area.
 * Must be larger than the maximum JPEG frame size (400×640 q80 ≈ 80 KB).
 * 200 KB gives generous headroom.
 */
#define UVC_XFER_BUF_SIZE  (200 * 1024)

/* Frame timeout forwarded to cam_pipeline_capture_jpeg() */
#define FRAME_TIMEOUT_MS   5000

/* ── Callbacks ───────────────────────────────────────────────────────────── */

/*
 * start_cb: called when the USB host opens the UVC device (e.g. app opens
 * the camera).  The camera pipeline is already running, so nothing to do
 * except log the event and the negotiated format.
 */
static esp_err_t s_uvc_start_cb(uvc_format_t format, int width, int height,
                                 int rate, void *ctx)
{
    const char *fmt_str = (format == UVC_FORMAT_JPEG) ? "MJPEG" : "H264";
    ESP_LOGI(TAG, "Host opened camera: %s %dx%d @ %d fps", fmt_str, width, height, rate);
    return ESP_OK;
}

/*
 * fb_get_cb: called by the UVC component's camera task each time the host
 * requests a new frame.  Blocks until a valid JPEG is ready.
 *
 * Returns NULL on timeout or encode error; the UVC component will retry
 * on the next scheduling cycle.
 */
static uvc_fb_t *s_uvc_fb_get_cb(void *ctx)
{
    static uvc_fb_t fb;  /* static: valid until next call */

    esp_err_t err = cam_pipeline_capture_jpeg(&fb, FRAME_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capture_jpeg failed: 0x%x — returning NULL", err);
        return NULL;
    }

    return &fb;
}

/*
 * fb_return_cb: called when the USB host has consumed the frame and TinyUSB
 * no longer needs the buffer.  The buffer is owned by camera_pipeline.c and
 * will be reused on the next capture, so no action is required here.
 */
static void s_uvc_fb_return_cb(uvc_fb_t *fb, void *ctx)
{
    (void)fb;
    (void)ctx;
    /* No pool — buffer is overwritten on next cam_pipeline_capture_jpeg() */
}

/*
 * stop_cb: called when the USB host closes the camera.
 */
static void s_uvc_stop_cb(void *ctx)
{
    ESP_LOGI(TAG, "Host closed camera");
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t uvc_stream_init(void)
{
    ESP_LOGI(TAG, "Initialising UVC stream (USB-A OTG HS)");

    /* Allocate UVC transfer buffer in internal RAM (TinyUSB DMA requirement) */
    uint8_t *uvc_buf = heap_caps_malloc(UVC_XFER_BUF_SIZE, MALLOC_CAP_DEFAULT | MALLOC_CAP_DMA);
    if (!uvc_buf) {
        ESP_LOGE(TAG, "UVC transfer buffer alloc failed (%u B)", UVC_XFER_BUF_SIZE);
        return ESP_FAIL;
    }

    uvc_device_config_t cfg = {
        .uvc_buffer      = uvc_buf,
        .uvc_buffer_size = UVC_XFER_BUF_SIZE,
        .start_cb        = s_uvc_start_cb,
        .fb_get_cb       = s_uvc_fb_get_cb,
        .fb_return_cb    = s_uvc_fb_return_cb,
        .stop_cb         = s_uvc_stop_cb,
        .cb_ctx          = NULL,
    };

    ESP_ERROR_CHECK(uvc_device_config(0, &cfg));
    ESP_ERROR_CHECK(uvc_device_init());

    ESP_LOGI(TAG, "UVC device ready — connect USB-A cable to host");
    return ESP_OK;
}
