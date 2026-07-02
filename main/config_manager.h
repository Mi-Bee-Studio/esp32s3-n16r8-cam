/*
 * MiBee Cam v0.1 — NVS-backed config manager
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Persists camera and application configuration to NVS namespace "mibee_cfg".
 * Each key is stored individually (not as a blob) for forward/backward
 * compatibility.  Missing keys fall back to compiled-in defaults.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  Load config from NVS.  Missing keys keep compiled defaults.
 * @return ESP_OK on success (NVS missing is NOT an error — defaults apply)
 * @note   Call AFTER nvs_flash_init() in the main boot sequence.
 */
esp_err_t config_load(void);

/**
 * @brief  Set a single key-value pair in memory.
 * @param  key   One of: wifi_ssid, wifi_pass, cam_framesize, cam_quality,
 *               ai_face_enable, ai_motion_enable, ai_qr_enable,
 *               rtsp_user, rtsp_pass, onvif_enable,
 *               cam_brightness, cam_contrast, cam_saturation, cam_sharpness,
 *               cam_hmirror, cam_vflip
 * @param  value String representation.  Numeric keys parse uint8.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for unknown keys or bad values.
 * @note   Does NOT write to NVS — call config_save() to persist.
 */
esp_err_t config_set(const char *key, const char *value);

/**
 * @brief  Persist all in-memory values to NVS.
 * @return ESP_OK, or the first NVS error encountered.
 */
esp_err_t config_save(void);

/**
 * @brief  Reset all values to compiled defaults and persist.
 */
esp_err_t config_reset(void);

/* ------------------------------------------------------------------ */
/*  Typed accessors                                                    */
/* ------------------------------------------------------------------ */

const char *config_get_wifi_ssid(void);
const char *config_get_wifi_pass(void);
uint8_t     config_get_cam_framesize(void);
uint8_t     config_get_cam_quality(void);
bool        config_get_ai_face_enable(void);
bool        config_get_ai_motion_enable(void);
bool        config_get_ai_qr_enable(void);
const char *config_get_rtsp_user(void);
const char *config_get_rtsp_pass(void);
bool        config_get_onvif_enable(void);

int8_t     config_get_cam_brightness(void);
int8_t     config_get_cam_contrast(void);
int8_t     config_get_cam_saturation(void);
int8_t     config_get_cam_sharpness(void);
bool       config_get_cam_hmirror(void);
bool       config_get_cam_vflip(void);

#ifdef __cplusplus
}
#endif
