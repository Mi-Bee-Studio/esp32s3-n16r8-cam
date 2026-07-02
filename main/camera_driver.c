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
#include "config_manager.h"
#include "frame_broadcaster.h"
#include "ai_pipeline.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
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
    .frame_size   = FRAMESIZE_VGA,       /* overridden by config at init */
    .jpeg_quality = 12,                   /* overridden by config at init */
    .fb_count     = 2,

    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

/* ------------------------------------------------------------------ */
/*  camera_init()                                                      */
/* ------------------------------------------------------------------ */

esp_err_t camera_init(void)
{
    /* Read config values */
    uint8_t framesize = config_get_cam_framesize();
    uint8_t quality = config_get_cam_quality();

    /* Force VGA when AI is enabled (pipeline hardcodes 640x480) */
    if (!camera_framesize_is_vga(framesize) &&
        (config_get_ai_face_enable() || config_get_ai_motion_enable() || config_get_ai_qr_enable())) {
        ESP_LOGW(TAG, "AI enabled but framesize is not VGA (cur=%d) — forcing VGA", framesize);
        framesize = camera_framesize_from_int(10); /* FRAMESIZE_VGA */
    }

    ESP_LOGI(TAG, "Initializing OV3660 camera (framesize=%d quality=%d, PSRAM fb)", framesize, quality);

    /* Build dynamic config from static pins + config values */
    camera_config_t cfg = s_camera_cfg;
    cfg.frame_size   = (framesize_t)framesize;
    cfg.jpeg_quality = quality;

    esp_err_t err = esp_camera_init(&cfg);
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

    /* Apply sensor settings from config */
    camera_apply_sensor_settings();

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

/* ------------------------------------------------------------------ */
/*  camera_apply_sensor_settings()                                     */
/* ------------------------------------------------------------------ */

void camera_apply_sensor_settings(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        ESP_LOGW(TAG, "Cannot apply sensor settings — sensor is NULL");
        return;
    }

    int8_t brightness = config_get_cam_brightness();
    int8_t contrast   = config_get_cam_contrast();
    int8_t saturation = config_get_cam_saturation();
    int8_t sharpness  = config_get_cam_sharpness();
    int     hmirror   = config_get_cam_hmirror() ? 1 : 0;
    int     vflip     = config_get_cam_vflip()   ? 1 : 0;

    if (sensor->set_brightness) sensor->set_brightness(sensor, brightness);
    if (sensor->set_contrast)   sensor->set_contrast(sensor, contrast);
    if (sensor->set_saturation) sensor->set_saturation(sensor, saturation);
    if (sensor->set_sharpness)  sensor->set_sharpness(sensor, sharpness);
    if (sensor->set_hmirror)    sensor->set_hmirror(sensor, hmirror);
    if (sensor->set_vflip)      sensor->set_vflip(sensor, vflip);

    ESP_LOGI(TAG, "Sensor settings applied: bright=%d contrast=%d sat=%d sharp=%d mirror=%d flip=%d",
             brightness, contrast, saturation, sharpness, hmirror, vflip);
}

/* ------------------------------------------------------------------ */
/*  camera_reinit()                                                    */
/* ------------------------------------------------------------------ */

esp_err_t camera_reinit(uint8_t framesize, uint8_t quality)
{
    ESP_LOGI(TAG, "Camera reinit requested: framesize=%d quality=%d", framesize, quality);

    /* Record AI running state before stopping */
    bool ai_was_running = ai_is_enabled(AI_FEATURE_FACE_DETECT) ||
                          ai_is_enabled(AI_FEATURE_MOTION_DETECT) ||
                          ai_is_enabled(AI_FEATURE_QR_DECODE);

    /* 1. Coordinated stop: AI first (it reads frames), then broadcaster */
    if (ai_was_running) {
        ai_stop_task();
    }
    frame_broadcaster_stop();

    /* 2. Deinit camera */
    esp_err_t deinit_err = esp_camera_deinit();
    if (deinit_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_deinit() failed: %s", esp_err_to_name(deinit_err));
    }

    /* 3. Update config in memory and persist */
    char framesize_str[4];
    char quality_str[4];
    snprintf(framesize_str, sizeof(framesize_str), "%u", (unsigned)framesize);
    snprintf(quality_str, sizeof(quality_str), "%u", (unsigned)quality);
    config_set("cam_framesize", framesize_str);
    config_set("cam_quality", quality_str);
    config_save();

    /* 4. Re-init with new settings */
    camera_config_t cfg = s_camera_cfg;
    cfg.frame_size   = (framesize_t)framesize;
    cfg.jpeg_quality = quality;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera reinit failed: %s", esp_err_to_name(err));
        /* Try to restore previous config */
        cfg.frame_size   = (framesize_t)config_get_cam_framesize();
        cfg.jpeg_quality = config_get_cam_quality();
        esp_camera_init(&cfg);
        camera_apply_sensor_settings();
        frame_broadcaster_start();
        if (ai_was_running) ai_start_task();
        return err;
    }

    /* 5. Apply sensor settings */
    camera_apply_sensor_settings();

    /* 6. Restart broadcaster */
    frame_broadcaster_start();

    /* 7. Restart AI only if it was running AND framesize is VGA */
    if (ai_was_running && camera_framesize_is_vga(framesize)) {
        ai_start_task();
    } else if (ai_was_running && !camera_framesize_is_vga(framesize)) {
        ESP_LOGW(TAG, "AI was running but new framesize=%d is not VGA — AI stays stopped", framesize);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Helper functions                                                   */
/* ------------------------------------------------------------------ */

uint8_t camera_framesize_from_int(uint8_t val)
{
    /* The esp32-camera framesize_t enum has 25 valid values (0..24).
     * FRAMESIZE_INVALID = 25. If out of range, default to VGA (10). */
    if (val > 24) return 10;
    return val;
}

const char *camera_framesize_name(uint8_t framesize)
{
    /* MUST match the framesize_t enum from esp32-camera driver/include/sensor.h */
    switch (framesize) {
        case 0:  return "96X96";
        case 1:  return "QQVGA";
        case 2:  return "128X128";
        case 3:  return "QCIF";
        case 4:  return "HQVGA";
        case 5:  return "240X240";
        case 6:  return "QVGA";
        case 7:  return "320X320";
        case 8:  return "CIF";
        case 9:  return "HVGA";
        case 10: return "VGA";
        case 11: return "SVGA";
        case 12: return "XGA";
        case 13: return "HD";
        case 14: return "SXGA";
        case 15: return "UXGA";
        case 16: return "FHD";
        case 17: return "P_HD";
        case 18: return "P_3MP";
        case 19: return "QXGA";
        case 20: return "QHD";
        case 21: return "WQXGA";
        case 22: return "P_FHD";
        case 23: return "QSXGA";
        case 24: return "5MP";
        default: return "unknown";
    }
}

bool camera_framesize_is_vga(uint8_t framesize)
{
    /* FRAMESIZE_VGA = 10 in esp32-camera framesize_t enum */
    return framesize == 10;
}
