/*
 * MiBee Cam v0.1 — NVS-backed config manager
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NVS namespace: "mibee_cfg"
 *
 * Design:
 *   - Individual key-value pairs (NOT a blob) so future firmware versions
 *     can add/remove keys without migration code.
 *   - config_load() reads every known key; NVS_ERR_NOT_FOUND keeps the
 *     compiled default for that key.
 *   - config_save() writes every key then calls nvs_commit().
 *   - config_set() accepts string values and parses numeric keys as uint8.
 *   - A FreeRTOS mutex protects write paths (config_set, config_save,
 *     config_reset) for safe concurrent access from web server and
 *     AT command tasks.
 */

#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define NVS_NAMESPACE  "mibee_cfg"
#define TAG            "config"

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char    wifi_ssid[33];
    char    wifi_pass[65];
    uint8_t cam_framesize;
    uint8_t cam_quality;
    bool    ai_face_enable;
    bool    ai_motion_enable;
    bool    ai_qr_enable;
    char    rtsp_user[33];
    char    rtsp_pass[33];
    bool    onvif_enable;
    int8_t  cam_brightness;   /* OV3660 brightness: -2..+2 */
    int8_t  cam_contrast;     /* OV3660 contrast: -2..+2 */
    int8_t  cam_saturation;   /* OV3660 saturation: -2..+2 */
    int8_t  cam_sharpness;    /* OV3660 sharpness range TBD from sensor_t */
    bool    cam_hmirror;      /* horizontal mirror */
    bool    cam_vflip;        /* vertical flip */
} config_t;

static config_t s_config;

static SemaphoreHandle_t s_config_mutex = NULL;

static const config_t s_defaults = {
    .wifi_ssid       = "",
    .wifi_pass       = "",
    .cam_framesize   = 10,              /* FRAMESIZE_VGA — safe for OV3660 smoke test */
    .cam_quality     = 12,
    .ai_face_enable  = true,
    .ai_motion_enable = true,
    .ai_qr_enable    = true,
    .rtsp_user       = "admin",
    .rtsp_pass       = "admin",
    .onvif_enable    = true,
    .cam_brightness  = 0,
    .cam_contrast    = 0,
    .cam_saturation  = 0,
    .cam_sharpness   = 0,
    .cam_hmirror     = false,
    .cam_vflip       = false,
};

/* ------------------------------------------------------------------ */
/*  Key table — maps string key names to their metadata                */
/* ------------------------------------------------------------------ */

typedef enum {
    TYPE_STRING,
    TYPE_U8,
    TYPE_I8,
} key_type_t;

typedef struct {
    const char *name;
    key_type_t  type;
    size_t      offset;         /* byte offset within config_t */
    size_t      max_len;        /* for strings: buffer size    */
} key_entry_t;

/* Offsets for string members */
#define OFF_STR(field)  offsetof(config_t, field), sizeof(((config_t *)0)->field)
#define OFF_U8(field)   offsetof(config_t, field), 1u
#define OFF_I8(field)   offsetof(config_t, field), 1u

static const key_entry_t s_keys[] = {
    { "wifi_ssid",       TYPE_STRING, OFF_STR(wifi_ssid)       },
    { "wifi_pass",       TYPE_STRING, OFF_STR(wifi_pass)       },
    { "cam_framesize",   TYPE_U8,     OFF_U8(cam_framesize)    },
    { "cam_quality",     TYPE_U8,     OFF_U8(cam_quality)      },
    { "ai_face_enable",  TYPE_U8,     OFF_U8(ai_face_enable)   },
    { "ai_motion_enable", TYPE_U8,    OFF_U8(ai_motion_enable) },
    { "ai_qr_enable",    TYPE_U8,     OFF_U8(ai_qr_enable)     },
    { "rtsp_user",       TYPE_STRING, OFF_STR(rtsp_user)       },
    { "rtsp_pass",       TYPE_STRING, OFF_STR(rtsp_pass)       },
    { "onvif_enable",    TYPE_U8,     OFF_U8(onvif_enable)     },
    { "cam_brightness", TYPE_I8, OFF_I8(cam_brightness) },
    { "cam_contrast",   TYPE_I8, OFF_I8(cam_contrast)   },
    { "cam_saturation", TYPE_I8, OFF_I8(cam_saturation) },
    { "cam_sharpness",  TYPE_I8, OFF_I8(cam_sharpness)  },
    { "cam_hmirror",    TYPE_U8, OFF_U8(cam_hmirror)    },
    { "cam_vflip",      TYPE_U8, OFF_U8(cam_vflip)      },
};

#define NUM_KEYS (sizeof(s_keys) / sizeof(s_keys[0]))

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static const key_entry_t *find_key(const char *name)
{
    for (size_t i = 0; i < NUM_KEYS; i++) {
        if (strcmp(s_keys[i].name, name) == 0) {
            return &s_keys[i];
        }
    }
    return NULL;
}

static void *field_ptr(const key_entry_t *k)
{
    return (uint8_t *)&s_config + k->offset;
}


/* Read a string key from NVS; if missing, keep the default. */
static esp_err_t read_str_nvs(nvs_handle_t h, const key_entry_t *k)
{
    char *buf = (char *)field_ptr(k);

    size_t len = 0;
    esp_err_t err = nvs_get_str(h, k->name, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* keep default */
    }
    if (err != ESP_OK) {
        return err;
    }
    if (len > k->max_len) {
        len = k->max_len;
    }
    return nvs_get_str(h, k->name, buf, &len);
}

/* Read a uint8 key from NVS; if missing, keep the default. */
static esp_err_t read_u8_nvs(nvs_handle_t h, const key_entry_t *k)
{
    uint8_t *val = (uint8_t *)field_ptr(k);
    esp_err_t err = nvs_get_u8(h, k->name, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* keep default */
    }
    return err;
}

/* Read an int8 key from NVS; if missing, keep the default. */
static esp_err_t read_i8_nvs(nvs_handle_t h, const key_entry_t *k)
{
    int8_t *val = (int8_t *)field_ptr(k);
    esp_err_t err = nvs_get_i8(h, k->name, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* keep default */
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t config_load(void)
{
    /* Start with defaults */
    memcpy(&s_config, &s_defaults, sizeof(s_config));

    /* Create mutex for thread-safe write access */
    s_config_mutex = xSemaphoreCreateMutex();
    if (!s_config_mutex) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored config, using defaults");
        return ESP_OK;   /* defaults already applied */
    }

    for (size_t i = 0; i < NUM_KEYS; i++) {
        const key_entry_t *k = &s_keys[i];
        if (k->type == TYPE_STRING) {
            read_str_nvs(h, k);
        } else if (k->type == TYPE_U8) {
            read_u8_nvs(h, k);
        } else if (k->type == TYPE_I8) {
            read_i8_nvs(h, k);
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded from NVS");
    return ESP_OK;
}

esp_err_t config_set(const char *key, const char *value)
{
    const key_entry_t *k = find_key(key);
    if (!k || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (k->type == TYPE_STRING) {
        char *buf = (char *)field_ptr(k);
        strncpy(buf, value, k->max_len - 1);
        buf[k->max_len - 1] = '\0';
    } else if (k->type == TYPE_U8) {
        unsigned long uv = strtoul(value, NULL, 10);
        if (uv > 255) {
            xSemaphoreGive(s_config_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        *(uint8_t *)field_ptr(k) = (uint8_t)uv;
    } else if (k->type == TYPE_I8) {
        long sv = strtol(value, NULL, 10);
        if (sv < -128 || sv > 127) {
            xSemaphoreGive(s_config_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        *(int8_t *)field_ptr(k) = (int8_t)sv;
    }

    xSemaphoreGive(s_config_mutex);

    return ESP_OK;
}

esp_err_t config_save(void)
{

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        xSemaphoreGive(s_config_mutex);
        return err;
    }

    for (size_t i = 0; i < NUM_KEYS; i++) {
        const key_entry_t *k = &s_keys[i];
        if (k->type == TYPE_STRING) {
            const char *val = (const char *)field_ptr(k);
            err = nvs_set_str(h, k->name, val);
        } else if (k->type == TYPE_U8) {
            uint8_t val = *(const uint8_t *)field_ptr(k);
            err = nvs_set_u8(h, k->name, val);
        } else if (k->type == TYPE_I8) {
            int8_t val = *(const int8_t *)field_ptr(k);
            err = nvs_set_i8(h, k->name, val);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write NVS key '%s': %s",
                     k->name, esp_err_to_name(err));
            nvs_close(h);
            xSemaphoreGive(s_config_mutex);
            return err;
        }
    }

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS");
    } else {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_reset(void)
{

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(&s_config, &s_defaults, sizeof(s_config));
    xSemaphoreGive(s_config_mutex);
    ESP_LOGI(TAG, "Config reset to factory defaults");
    return config_save();
}

/* ------------------------------------------------------------------ */
/*  Typed accessors                                                    */
/* ------------------------------------------------------------------ */

const char *config_get_wifi_ssid(void)      { return s_config.wifi_ssid; }
const char *config_get_wifi_pass(void)      { return s_config.wifi_pass; }
uint8_t     config_get_cam_framesize(void)  { return s_config.cam_framesize; }
uint8_t     config_get_cam_quality(void)    { return s_config.cam_quality; }
bool        config_get_ai_face_enable(void) { return s_config.ai_face_enable; }
bool        config_get_ai_motion_enable(void) { return s_config.ai_motion_enable; }
bool        config_get_ai_qr_enable(void)   { return s_config.ai_qr_enable; }
const char *config_get_rtsp_user(void)      { return s_config.rtsp_user; }
const char *config_get_rtsp_pass(void)      { return s_config.rtsp_pass; }
bool        config_get_onvif_enable(void)   { return s_config.onvif_enable; }

int8_t config_get_cam_brightness(void) { return s_config.cam_brightness; }
int8_t config_get_cam_contrast(void)   { return s_config.cam_contrast; }
int8_t config_get_cam_saturation(void) { return s_config.cam_saturation; }
int8_t config_get_cam_sharpness(void)  { return s_config.cam_sharpness; }
bool   config_get_cam_hmirror(void)    { return s_config.cam_hmirror; }
bool   config_get_cam_vflip(void)      { return s_config.cam_vflip; }
