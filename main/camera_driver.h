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

#ifdef __cplusplus
}
#endif
