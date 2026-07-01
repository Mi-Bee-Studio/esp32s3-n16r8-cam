/*
 * MiBee Cam v0.1 — Main application entry point
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Boot sequence:
 *   1. NVS flash
 *   2. SPIFFS (Web UI assets)
 *   3. WiFi (AP or STA)
 *   4. Camera (OV3660)
 *   5. Web server + MJPEG streamer
 *   6. OTA updater
 *
 * Phase 1 scaffold — app_main with boot log and resource info.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "mibee_cam";

/* ------------------------------------------------------------------ */
/*  SPIFFS mount for Web UI static assets                              */
/* ------------------------------------------------------------------ */

static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
    }
}

/* ------------------------------------------------------------------ */
/*  app_main                                                          */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "MiBee Cam v0.1 starting...");
    ESP_LOGI(TAG, "Free heap: %lu  Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- 1. NVS flash ------------------------------------------------ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }

    /* ---- 2. SPIFFS for Web UI ---------------------------------------- */
    init_spiffs();

    /* ---- 3. Camera (placeholder — Phase 2) -------------------------- */
    ESP_LOGI(TAG, "Camera init deferred to Phase 2");

    /* ---- Done -------------------------------------------------------- */
    ESP_LOGI(TAG, "MiBee Cam v0.1 scaffold initialized successfully");
    ESP_LOGI(TAG, "System info: chip=%s cores=%d psram=%luMB",
             CONFIG_IDF_TARGET,
             portNUM_PROCESSORS,
             (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)));

    /* Idle loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "Heartbeat: heap=%lu PSRAM=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
