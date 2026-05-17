/*
 * camera_pipeline.h — OV5647 MIPI CSI-2 → ISP → JPEG pipeline
 *
 * Owns: LDO, frame/preview/JPEG buffers, ISP, CSI controller,
 *       OV5647 sensor init, JPEG HW encoder, DMA frame semaphore.
 *
 * The pipeline is ISR-driven: on_trans_finished fires from DMA ISR
 * and gives s_frame_sem.  cam_pipeline_capture_jpeg() waits on that
 * semaphore, validates the frame, cache-syncs, downsamples and encodes.
 * It is designed to be called directly from the UVC fb_get_cb, which
 * runs in the UVC component's dedicated camera FreeRTOS task.
 */
#pragma once

#include "esp_err.h"
#include "usb_device_uvc.h"   /* uvc_fb_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate buffers and initialise the full camera pipeline.
 *
 * Must be called once before cam_pipeline_capture_jpeg().
 * Sequence:
 *   LDO → frame/preview/JPEG bufs → JPEG encoder → semaphore →
 *   ISP (demosaic GBRG + CCM + gamma) → CSI → OV5647 sensor
 *
 * @return ESP_OK on success, ESP_FAIL / propagated ESP_ERROR_CHECK on failure.
 */
esp_err_t cam_pipeline_init(void);

/**
 * @brief Capture one valid frame, encode it as JPEG, and fill a uvc_fb_t.
 *
 * Blocks until a valid DMA frame arrives (or timeout_ms elapses).
 * Automatically skips frames whose DMA received_size doesn't match
 * the expected RAW8 size (chip-rev v1.3 quirk).
 *
 * On success the caller receives a pointer to the internally-owned JPEG
 * buffer.  The buffer stays valid until the next call to this function.
 * No explicit release call is needed — the UVC component may call
 * fb_return_cb at any time, but we don't need to act on it because the
 * JPEG buffer is not returned to a pool; it is overwritten on the next
 * capture cycle.
 *
 * @param[out] fb          uvc_fb_t to fill (buf, len, width, height, format).
 * @param[in]  timeout_ms  Maximum wait time for a DMA frame (milliseconds).
 *
 * @return ESP_OK            Frame captured and JPEG encoded successfully.
 * @return ESP_ERR_TIMEOUT   No DMA frame arrived within timeout_ms.
 * @return ESP_ERR_INVALID_SIZE  DMA received_size mismatch (frame skipped).
 * @return ESP_FAIL          JPEG encoder error.
 */
esp_err_t cam_pipeline_capture_jpeg(uvc_fb_t *fb, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
