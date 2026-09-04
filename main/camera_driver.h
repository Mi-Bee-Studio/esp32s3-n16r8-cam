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

/* JPEG quality bounds (lower = better quality / larger frames) — driver
 * invariant, applies to every board using esp32-camera: JPEG frame buffers
 * are sized w*h/5 (max 1:5 compression), q<10 overflows that budget on
 * complex scenes and produces truncated frames (family finding 2026-09-04,
 * PITFALLS PIT-021; validated on ai-thinker/seeed/luatos hardware).
 * NOTE: resolution cap for THIS board is not yet measured — do not copy
 * sister-board caps; measure on hardware before restricting 0-15. */
#define CAMERA_QUALITY_MIN 10
#define CAMERA_QUALITY_MAX 63

/* 板级分辨率上限（2026-09-04 两轮上板复测 + 双网复核，PIT-021/022 流程）：
 * **SVGA(11) 稳定，XGA(12) 起致命**。XGA 楔死在主网(GT)与备用网(MiBeeAP2)
 * 上完全一致（采集侧 fb_get NULL，与网络无关）。注意：推流 delivered fps
 * 是链路/NVR 订阅数约束的投递侧指标（实测 0.5-0.8fps 时板端采集仍 25-27fps，
 * 见 fbroadcast 日志）——判定上限只看采集侧。
 *   VGA：640×480 实拍确认，推流/取帧正常；
 *   SVGA：冷启动后传感器真实输出 800×600（JPEG SOF 实测），帧流正常；
 *   XGA+：冷启动即 esp_camera_fb_get 返回 NULL，取帧死，且会令板子楔死
 *   （ai 任务 wdt 未登记空转刷错，另修）。PSRAM 8MB 充裕——这是模组/
 *   传感器/DVP 组合的极限，不是内存问题，勿调 fb 参数强上。
 * 历史教训：首轮“仅 VGA”结论被两条污染链毁掉（NVS 键名 16 字符令
 * config_save 整体失败 → “关 AI”存不住 → AI 强制 VGA 钳制每晨复活；
 * 加上校验写成 `!=max` 单值锁）。详见 PIT-021/022。
 * 家族纪律：禁止沿用姐妹板数值。 */
#define CAMERA_RES_BOARD_MAX 11   /* FRAMESIZE_SVGA */
int camera_get_effective_max_res(void);

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

/**
 * @brief Detected sensor model string ("OV2640"/"OV3660"/"OV5640"/"unknown").
 */
const char *camera_sensor_name(void);

#ifdef __cplusplus
}
#endif
