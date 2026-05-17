/*
 * main.c — OV5647 UVC Webcam
 *
 * Orchestrates the two subsystems:
 *   1. cam_pipeline_init()  — ISP + CSI + OV5647 + JPEG encoder
 *   2. uvc_stream_init()    — TinyUSB UVC device on USB-A OTG HS port
 *
 * After both inits complete the FreeRTOS tasks spawned by usb_device_uvc
 * drive the entire pipeline.  app_main loops with a heartbeat log so the
 * JTAG serial port remains available for debug output.
 *
 * Hardware:
 *   Waveshare ESP32-P4 Nano (chip rev v1.3)
 *   OV5647 camera on MIPI CSI-2 connector  (SDA=GPIO7, SCL=GPIO8)
 *   USB-C  → JTAG/serial (debug logs, programming)
 *   USB-A  → OTG HS      (UVC webcam to host, this project)
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_pipeline.h"
#include "uvc_stream.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG,              ESP_LOG_INFO);
    esp_log_level_set("cam_pipeline",   ESP_LOG_INFO);
    esp_log_level_set("uvc_stream",     ESP_LOG_INFO);
    esp_log_level_set("usbd_uvc",    ESP_LOG_INFO);  /* usb_device_uvc component (TAG="usbd_uvc") */
    esp_log_level_set("tusb_video", ESP_LOG_INFO);  /* TinyUSB UVC class */

    ESP_LOGI(TAG, "OV5647 UVC Webcam starting");
    ESP_LOGI(TAG, "USB-C = JTAG/debug   USB-A = UVC webcam to host");

    /* Initialise camera pipeline (ISP + CSI + OV5647 + JPEG encoder) */
    ESP_ERROR_CHECK(cam_pipeline_init());

    /* Initialise USB UVC device (usb_device_uvc spawns TinyUSB + camera tasks) */
    ESP_ERROR_CHECK(uvc_stream_init());

    ESP_LOGI(TAG, "Ready — connect USB-A to host.  JTAG log remains active on USB-C.");

    /* Heartbeat: keep app_main alive so JTAG logs remain visible */
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "heartbeat %lu min", (unsigned long)(++tick / 2));
    }
}
