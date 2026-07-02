/*
 * MiBee Cam v0.1 — OV3660 camera driver
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wraps the espressif/esp32-camera component with:
 *   - Pin configuration from Kconfig (CONFIG_CAMERA_PIN_*)
 *   - Sensor ID verification  (expects PID 0x77 for OV3660)
 *   - Single-frame JPEG capture helper
 */

#include "camera_driver.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "camera_drv";

/* ------------------------------------------------------------------ */
/*  Camera pin mapping — sourced from sdkconfig.defaults via Kconfig   */
/* ------------------------------------------------------------------ */

static const camera_config_t s_camera_cfg = {
    .pin_pwdn     = CONFIG_CAMERA_PIN_PWDN,
    .pin_reset    = CONFIG_CAMERA_PIN_RESET,
    .pin_xclk     = CONFIG_CAMERA_PIN_XCLK,
    .pin_sccb_sda = CONFIG_CAMERA_PIN_SIOD,
    .pin_sccb_scl = CONFIG_CAMERA_PIN_SIOC,

    .pin_d7       = CONFIG_CAMERA_PIN_D7,
    .pin_d6       = CONFIG_CAMERA_PIN_D6,
    .pin_d5       = CONFIG_CAMERA_PIN_D5,
    .pin_d4       = CONFIG_CAMERA_PIN_D4,
    .pin_d3       = CONFIG_CAMERA_PIN_D3,
    .pin_d2       = CONFIG_CAMERA_PIN_D2,
    .pin_d1       = CONFIG_CAMERA_PIN_D1,
    .pin_d0       = CONFIG_CAMERA_PIN_D0,

    .pin_vsync    = CONFIG_CAMERA_PIN_VSYNC,
    .pin_href     = CONFIG_CAMERA_PIN_HREF,
    .pin_pclk     = CONFIG_CAMERA_PIN_PCLK,

    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_VGA,
    .jpeg_quality = 12,
    .fb_count     = 2,

    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

/* ------------------------------------------------------------------ */
/*  camera_init()                                                      */
/* ------------------------------------------------------------------ */

esp_err_t camera_init(void)
{
    ESP_LOGI(TAG, "Initializing OV3660 camera (VGA JPEG, PSRAM fb)");

    esp_err_t err = esp_camera_init(&s_camera_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init() failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "esp_camera_init() OK — probing sensor ID");

    /* ---- Verify sensor is OV3660 (PID 0x77) --------------------- */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGE(TAG, "esp_camera_sensor_get() returned NULL");
        return ESP_ERR_CAMERA_NOT_DETECTED;
    }

    uint16_t pid = sensor->id.PID;
    ESP_LOGI(TAG, "Sensor PID: 0x%04X  MID: 0x%02X:%02X", pid, sensor->id.MIDH, sensor->id.MIDL);

    if (pid == 0x77) {
        ESP_LOGI(TAG, "OV3660 sensor confirmed (PID=0x77)");
    } else if (pid == 0x26 || pid == 0x42) {
        ESP_LOGE(TAG, "OV2640 detected (PID=0x%02X) — expected OV3660 (0x77)", pid);
        return ESP_ERR_INVALID_VERSION;
    } else {
        ESP_LOGW(TAG, "Unknown sensor PID=0x%04X — continuing anyway", pid);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  camera_capture()                                                   */
/* ------------------------------------------------------------------ */

camera_fb_t *camera_capture(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Frame capture failed (esp_camera_fb_get returned NULL)");
        return NULL;
    }

    ESP_LOGI(TAG, "Captured frame: %ux%u  format=%u  len=%u",
             fb->width, fb->height,
             (unsigned)fb->format,
             (unsigned)fb->len);

    return fb;
}
