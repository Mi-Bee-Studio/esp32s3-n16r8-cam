/*
 * MiBee Cam v0.1 — Camera driver for OV3660
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Public API:
 *   - camera_init()       Initialize OV3660 via esp_camera, verify sensor ID
 *   - camera_capture()    Grab one JPEG frame from the camera
 */

#pragma once

#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OV3660 camera sensor.
 *
 * Pins are drawn from Kconfig (CONFIG_CAMERA_PIN_*).
 * Frame buffer is allocated in PSRAM, format JPEG, size VGA.
 *
 * @return ESP_OK on success, or an esp_err_t code on failure.
 */
esp_err_t camera_init(void);

/**
 * @brief Capture a single frame from the camera.
 *
 * Returns a pointer to the frame buffer.  The caller MUST call
 * esp_camera_fb_return(fb) after processing the frame.
 *
 * @return Pointer to camera_fb_t, or NULL if capture failed.
 */
camera_fb_t *camera_capture(void);

/**
 * @brief Apply sensor settings (brightness, contrast, saturation, sharpness, mirror, flip) from config.
 */
void camera_apply_sensor_settings(void);

/**
 * @brief Coordinated camera re-initialization.
 *
 * Stops AI + broadcaster, deinits camera, reinits with new framesize/quality,
 * restarts broadcaster + AI (if it was running and framesize is VGA).
 * The new values are also persisted to config.
 *
 * @param framesize New framesize (framesize_t enum value, 0..24).
 * @param quality   New JPEG quality (0-63).
 * @return ESP_OK on success, or esp_err_t on init failure.
 */
esp_err_t camera_reinit(uint8_t framesize, uint8_t quality);

/**
 * @brief Validate and return a framesize enum value.
 *        Clamps out-of-range values to VGA (10).
 */
uint8_t camera_framesize_from_int(uint8_t val);

/**
 * @brief Get human-readable name for a framesize enum value.
 */
const char *camera_framesize_name(uint8_t framesize);

/**
 * @brief Check if a framesize is VGA (640x480).
 *        FRAMESIZE_VGA = 10 in esp32-camera framesize_t enum.
 */
bool camera_framesize_is_vga(uint8_t framesize);

#ifdef __cplusplus
}
#endif
