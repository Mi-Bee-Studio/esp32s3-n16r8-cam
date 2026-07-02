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
 *   5. Frame broadcaster + Web server + MJPEG streamer
 *   6. RTSP server (MJPEG-only, digest auth)
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "camera_driver.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "mjpeg_streamer.h"
#include "rtsp_server.h"
#include "frame_broadcaster.h"
#include "ai_pipeline.h"
#include "onvif_discovery.h"
#include "at_command.h"

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
    config_load();

    /* ---- 2. SPIFFS for Web UI ---------------------------------------- */
    init_spiffs();

    /* ---- 3. WiFi (AP or STA) ------------------------------------------- */
    {
        esp_err_t wf_err = wifi_manager_init();
        if (wf_err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi manager started");
        } else {
            ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(wf_err));
        }
    }

    /* ---- 4. Camera init + flash LED probe ------------------------- */
    {
        esp_err_t cam_err = camera_init();
        if (cam_err == ESP_OK) {
            /* Snapshot one frame to prove the pipeline works */
            camera_fb_t *fb = camera_capture();
            if (fb) {
                ESP_LOGI(TAG, "First frame OK: %ux%u  %u bytes",
                         fb->width, fb->height, (unsigned)fb->len);
                esp_camera_fb_return(fb);
            }

            /* ---- Probe flash LED on GPIO 2, 3, 46 ------------------ */
            const int flash_candidates[] = {2, 3, 46};
            for (int i = 0; i < 3; i++) {
                int gpio = flash_candidates[i];
                gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
                gpio_set_level(gpio, 1);
                ESP_LOGI(TAG, "Flash probe: GPIO %d = HIGH, waiting 2s...", gpio);
                vTaskDelay(pdMS_TO_TICKS(2000));
                gpio_set_level(gpio, 0);
                ESP_LOGI(TAG, "Flash probe: GPIO %d = LOW", gpio);
            }
            ESP_LOGW(TAG, "Flash LED probe complete \u2014 visually check which GPIO lit the LED");
        } else {
            ESP_LOGE(TAG, "Camera init failed, skipping frame capture + flash probe");
        }
    }

    /* ---- 5. Web server + MJPEG streamer ----------------------------- */
    {
        /* Initialize frame broadcaster (required by MJPEG streamer) */
        esp_err_t fb_err = frame_broadcaster_init();
        if (fb_err == ESP_OK) {
            ESP_LOGI(TAG, "Frame broadcaster initialized");
        } else {
            ESP_LOGE(TAG, "Frame broadcaster init failed: %s", esp_err_to_name(fb_err));
        }

        esp_err_t mjpeg_err = mjpeg_stream_init();
        if (mjpeg_err == ESP_OK) {
            ESP_LOGI(TAG, "MJPEG streamer initialized");
        } else {
            ESP_LOGE(TAG, "MJPEG streamer init failed: %s", esp_err_to_name(mjpeg_err));
        }

        /* Start frame broadcaster (grabs frames from camera in a task) */
        fb_err = frame_broadcaster_start();
        if (fb_err == ESP_OK) {
            ESP_LOGI(TAG, "Frame broadcaster started");
        } else {
            ESP_LOGE(TAG, "Frame broadcaster start failed: %s", esp_err_to_name(fb_err));
        }

        /* ---- AI pipeline (face detect + motion + QR) -------------------- */
        esp_err_t ai_err = ai_init();
        if (ai_err == ESP_OK) {
            ESP_LOGI(TAG, "AI pipeline initialized");
        } else {
            ESP_LOGE(TAG, "AI pipeline init failed: %s", esp_err_to_name(ai_err));
        }

        /* Wire AI config to live pipeline state (override defaults) */
        ai_enable(AI_FEATURE_FACE_DETECT,   config_get_ai_face_enable());
        ai_enable(AI_FEATURE_MOTION_DETECT, config_get_ai_motion_enable());
        ai_enable(AI_FEATURE_QR_DECODE,     config_get_ai_qr_enable());
        esp_err_t http_err = web_server_start(80);
        if (http_err == ESP_OK) {
            ESP_LOGI(TAG, "Web server running on port 80");
        } else {
            ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(http_err));
        }
    }

    /* ---- 6. RTSP server (MJPEG-only, digest auth) --------------------- */
    {
        esp_err_t rtsp_err = rtsp_start();
        if (rtsp_err == ESP_OK) {
            ESP_LOGI(TAG, "RTSP server started: %s", rtsp_get_url());
        } else {
            ESP_LOGE(TAG, "RTSP server start failed: %s", esp_err_to_name(rtsp_err));
        }

    /* ---- 7. ONVIF discovery + SOAP service --------------------------- */
    {
        esp_err_t onvif_err = onvif_start();
        if (onvif_err == ESP_OK) {
            ESP_LOGI(TAG, "ONVIF service started (WS-Discovery + mDNS)");
        } else {
            ESP_LOGI(TAG, "ONVIF service skipped: %s", esp_err_to_name(onvif_err));
        }
    }
    }

    /* ---- Done -------------------------------------------------------- */
    ESP_LOGI(TAG, "MiBee Cam v0.1 scaffold initialized successfully");
    ESP_LOGI(TAG, "System info: chip=%s cores=%d psram=%luMB",
             CONFIG_IDF_TARGET,
             portNUM_PROCESSORS,
             (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)));

    /* ---- 8. AT command listener (UART0) --------------------------- */
    at_command_init();

    /* Idle loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "Heartbeat: heap=%lu PSRAM=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
