/*
 * uvc_stream.h — USB UVC device initialisation
 *
 * Configures and starts the usb_device_uvc component (esp-iot-solution).
 * After uvc_stream_init() the ESP32-P4 USB-A OTG port enumerates as a
 * standard UVC webcam on the host (macOS / Windows / Linux — no driver needed).
 *
 * The usb_device_uvc component spawns two FreeRTOS tasks:
 *   • TinyUSB task  — handles USB protocol (priority CONFIG_UVC_TINYUSB_TASK_PRIORITY)
 *   • Camera task   — calls fb_get_cb to pull frames (priority CONFIG_UVC_CAM1_TASK_PRIORITY)
 *
 * fb_get_cb delegates to cam_pipeline_capture_jpeg(), which blocks on the
 * DMA frame semaphore.  The camera pipeline must therefore be initialised
 * (cam_pipeline_init) before uvc_stream_init() is called.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise and start the USB UVC device stream.
 *
 * Allocates the UVC transfer buffer, registers the four callbacks
 * (start / fb_get / fb_return / stop) and calls uvc_device_init().
 * After this call the USB-A OTG port is active and will enumerate on
 * the host as a webcam when connected.
 *
 * Requires cam_pipeline_init() to have completed successfully first.
 *
 * @return ESP_OK on success.
 */
esp_err_t uvc_stream_init(void);

#ifdef __cplusplus
}
#endif
