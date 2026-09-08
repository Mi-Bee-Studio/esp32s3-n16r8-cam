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
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "camera_driver.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "csi_motion.h"
#include "web_server.h"
#include "mjpeg_streamer.h"
#include "rtsp_server.h"
#include "frame_broadcaster.h"
#include "ai_pipeline.h"
#include "onvif_discovery.h"
#include "at_command.h"
#include "ota_updater.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "main";

/* httpd :80 self-heal probe — sends a real HTTP request to localhost:80.
 * TCP connect alone is insufficient: LWIP accepts connections even when
 * httpd has no free worker. Only a real request proves the event loop is alive.
 *
 * 2026-09-04 自愈误杀修复（PIT-002 家族教训，方案同 seeed/ai-thinker）：
 * NVR 多路订阅 + 弱链路时 lwIP 池被 TIME_WAIT 挤占，探针自己 socket()/
 * connect() 拿不到资源（EMFILE/ENOBUFS，串口同时可见 httpd accept(23)），
 * 旧实现把它计为"httpd 死"→ 2/2 → esp_restart —— 资源紧张被翻译成重启，
 * 实测 2-7 分钟循环（rst:0xc，无 panic）。修复：资源类失败一律不计数
 * （池子 15s MSL 后自行恢复）；只有"TCP 连上但应用层无响应"才判疑似卡死。 */
static bool probe_httpd_port80(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGW(TAG, "httpd probe: no socket available (errno=%d) — not counted", errno);
        return true;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(80),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
        static const char req[] =
            "GET /api/status HTTP/1.0\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n";
        if (send(sock, req, sizeof(req) - 1, 0) > 0) {
            char buf[32];
            int n = recv(sock, buf, sizeof(buf), 0);
            ok = (n > 0);
        }
    } else {
        /* 本机回环 connect 失败：EMFILE/ENOBUFS 属资源紧张，不计数 */
        ESP_LOGW(TAG, "httpd probe: connect failed (errno=%d) — not counted", errno);
        ok = true;
    }
    close(sock);
    return ok;
}


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

    /* 时区（契约 §3.1）：非空即在启动时应用——本板无 NTP，时区供
     * /api/time 手动设时后的 localtime() 换算使用（POST /api/config
     * 改 timezone 时立即重设，同 seeed/ai-thinker） */
    {
        const char *tz = config_get_timezone();
        if (tz && tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
            ESP_LOGI(TAG, "Timezone applied: %s", tz);
        }
    }

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

    /* ---- 3a. ESPectre CSI motion sensing (optional, after WiFi) ------- */
    csi_motion_init();

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
        esp_err_t ota_err = ota_updater_init();
        esp_err_t http_err = web_server_start(80);
        if (http_err == ESP_OK) {
            ESP_LOGI(TAG, "Web server running on port 80");
        } else {
            ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(http_err));
        }

        /* ---- MJPEG streamer on port 81 (independent TCP server) ------- */
        esp_err_t mjpeg_srv_err = mjpeg_stream_server_start(81);
        if (mjpeg_srv_err == ESP_OK) {
            ESP_LOGI(TAG, "MJPEG streamer running on port 81");
        } else {
            ESP_LOGE(TAG, "MJPEG streamer start failed: %s", esp_err_to_name(mjpeg_srv_err));
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
        /* httpd :80 self-heal: probe every 60s cycle.
         * 2 consecutive failures (120s unresponsive) → reboot.
         * WiFi 未连接时不计数（ai-thinker 2026-09-03 同款：掉线≠httpd 死）。 */
        static int httpd_stuck_count = 0;
        if (!probe_httpd_port80()) {
            if (!wifi_manager_is_connected()) {
                httpd_stuck_count = 0;
                ESP_LOGW(TAG, "httpd probe failed but WiFi down — not counting");
            } else {
                httpd_stuck_count++;
                ESP_LOGW(TAG, "httpd :80 probe failed (%d/2)", httpd_stuck_count);
                if (httpd_stuck_count >= 2) {
                    ESP_LOGE(TAG, "httpd :80 unresponsive for 120s — rebooting");
                    esp_restart();
                }
            }
        } else {
            httpd_stuck_count = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "Heartbeat: heap=%lu PSRAM=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
