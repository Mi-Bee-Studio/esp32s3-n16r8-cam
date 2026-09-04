/*
 * MiBee Cam v0.1 — Web server (ESP HTTP Server)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HTTP API + static file serving via esp_http_server.
 *
 * Design follows seeed-esp32s3-cam's web_server.c with audio/many
 * feature handlers removed.  The MJPEG stream is registered as a URI
 * handler but served asynchronously by mjpeg_streamer.c via the
 * httpd_req_async_handler_begin() API.
 */

#include "web_server.h"
#include "mjpeg_streamer.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"   /* TCP_NODELAY/KEEPALIVE sockopts（PIT-018 教训：WIP 漏 include，2026-09-04 补） */
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"   /* chip_temp：S3 温度传感器（对齐 seeed/luatos，2026-09-04） */

/* FW_VERSION 由 ota_updater.h（WIP，未入库）提供；此处兜底定义保证
 * 干净克隆可编译——OTA 合入后其同名定义优先生效（#ifndef 守卫）。 */
#ifndef FW_VERSION
#define FW_VERSION "0.1.1"
#endif
#include "freertos/FreeRTOS.h"
#include "cJSON.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "flash_led.h"
#include "ai_pipeline.h"
#include "camera_driver.h"
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"  /* for stat on SPIFFS files */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;

/* ── chip_temp：S3 温度传感器（2026-09-04 API 对齐，方案同 seeed/luatos）──
 * S3 温度传感器只有固定测量档（[-10,80]/[20,100]/[50,125]/[-30,50]），请求
 * 区间必须完整落入某一档，跨档被驱动拒绝——按高温档优先依次回退。
 * 惰性安装（首次 /api/status 请求时），此后驻留复用。 */
static temperature_sensor_handle_t s_tsens = NULL;
static float s_chip_temp = 0.0f;

static void chip_temp_ensure(void)
{
    if (s_tsens) {
        return;
    }
    static const temperature_sensor_config_t cand[] = {
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(50, 125),
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100),
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80),
    };
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        if (temperature_sensor_install(&cand[i], &s_tsens) == ESP_OK &&
            temperature_sensor_enable(s_tsens) == ESP_OK) {
            ESP_LOGI(TAG, "Temp sensor ready (range %d~%d°C)",
                     (int)cand[i].range_min, (int)cand[i].range_max);
            return;
        }
        s_tsens = NULL;
    }
    ESP_LOGW(TAG, "Temp sensor init failed — chip_temp stays 0");
}

static float chip_temp_read(void)
{
    chip_temp_ensure();
    if (s_tsens) {
        temperature_sensor_get_celsius(s_tsens, &s_chip_temp);
    }
    return s_chip_temp;
}

static void set_cors_headers(httpd_req_t *req);

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                        */
/* ------------------------------------------------------------------ */

esp_err_t json_ok(httpd_req_t *req, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON alloc failed", 17);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    if (data) {
        cJSON_AddItemToObject(root, "data", data);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON print failed", 17);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

esp_err_t json_error(httpd_req_t *req, const char *msg, int status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON alloc failed", 17);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON print failed", 17);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_err(req, status, json);
    free(json);
    return ESP_FAIL;
}

char *read_body(httpd_req_t *req, size_t max_len)
{
    size_t len = req->content_len;
    if (len == 0 || len > max_len) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        return NULL;
    }
    buf[ret] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Authentication helpers                                             */
/* ------------------------------------------------------------------ */

/* config_get_web_password() will be added to config_manager.c */
extern const char *config_get_web_password(void);

static bool check_auth(httpd_req_t *req)
{
    const char *stored_pass = config_get_web_password();
    /* If no password is set, allow access (first-time setup) */
    if (!stored_pass || stored_pass[0] == '\0') {
        return true;
    }
    
    /* Check X-Password header */
    char password[128];
    size_t password_len = sizeof(password) - 1;
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "X-Password", password, password_len);
    if (ret != ESP_OK) {
        return false;
    }
    password[password_len] = '\0';
    
    return strcmp(password, stored_pass) == 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    const char *stored_pass = config_get_web_password();
    
    /* State A: No password set — only POST /api/config with web_password field allowed */
    if (!stored_pass || stored_pass[0] == '\0') {
        return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
    }
    
    /* State B: Password is set — require X-Password header to match */
    char password[128];
    size_t password_len = sizeof(password) - 1;
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "X-Password", password, password_len);
    if (ret != ESP_OK) {
        return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
    }
    password[password_len] = '\0';

    if (strcmp(password, stored_pass) != 0) {
        return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
    }
    
    return ESP_OK;
}

/** @brief 公共鉴权入口（OTA 等模块复用）：状态A返回 SET_PASSWORD_FIRST，状态B校验 X-Password */
esp_err_t web_server_check_auth(httpd_req_t *req)
{
    const char *stored_pass = config_get_web_password();
    if (!stored_pass || stored_pass[0] == '\0') {
        return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
    }
    if (!check_auth(req)) {
        return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
    }
    return ESP_OK;
}

esp_err_t json_error_status(httpd_req_t *req, const char *msg, const char *status_line)
{
    char json[256];
    int len = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", msg);
    if (len >= (int)sizeof(json)) len = (int)sizeof(json) - 1;
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    return httpd_resp_send(req, json, len);
}

/* ------------------------------------------------------------------ */
/*  CORS helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Password");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

/* ------------------------------------------------------------------ */
/*  GET / — static file serving from SPIFFS                           */
/* ------------------------------------------------------------------ */

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    char filepath[580];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);

    /* Determine content type */
    const char *type = "text/html";
    const char *ext = strrchr(uri, '.');
    if (ext) {
        if (strcmp(ext, ".css") == 0)          type = "text/css";
        else if (strcmp(ext, ".js") == 0)      type = "application/javascript";
        else if (strcmp(ext, ".png") == 0)     type = "image/png";
        else if (strcmp(ext, ".jpg") == 0 ||
                 strcmp(ext, ".jpeg") == 0)   type = "image/jpeg";
        else if (strcmp(ext, ".ico") == 0)     type = "image/x-icon";
        else if (strcmp(ext, ".svg") == 0)     type = "image/svg+xml";
        else if (strcmp(ext, ".json") == 0)    type = "application/json";
        else if (strcmp(ext, ".html") == 0)    type = "text/html";
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, type);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /status                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* Device + WiFi (契约 v1.0 字段) */
    const char *device_name = config_get_device_name();
    cJSON_AddStringToObject(data, "device_name",
        (device_name && device_name[0]) ? device_name : "MiBeeCam");
    cJSON_AddStringToObject(data, "wifi_ssid", config_get_wifi_ssid());
    cJSON_AddStringToObject(data, "wifi_state",
        wifi_manager_is_connected() ? "connected" : "disconnected");
    cJSON_AddStringToObject(data, "wifi_net", wifi_manager_active_net());
    cJSON_AddStringToObject(data, "current_ssid", wifi_manager_current_ssid());
    cJSON_AddStringToObject(data, "ip", wifi_manager_get_ip());
    /* 2026-09-04 API 对齐：SPA 信号芯片与 WiFi 页当前连接行依赖
     * wifi_rssi/wifi_channel（三姐妹板均已下发，本板此前缺失 → 无信号显示） */
    cJSON_AddNumberToObject(data, "wifi_rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(data, "wifi_channel", wifi_manager_get_channel());

    /* Camera — 传感器型号 + 当前分辨率（细节在 /api/camera） */
    cJSON_AddStringToObject(data, "camera", camera_sensor_name());
    cJSON_AddStringToObject(data, "resolution",
        camera_framesize_name(config_get_cam_framesize()));

    /* AI status */
    cJSON *ai = cJSON_CreateObject();
    if (ai) {
        cJSON_AddBoolToObject(ai, "face",   ai_is_enabled(AI_FEATURE_FACE_DETECT));
        cJSON_AddBoolToObject(ai, "motion", ai_is_enabled(AI_FEATURE_MOTION_DETECT));
        cJSON_AddBoolToObject(ai, "qr",     ai_is_enabled(AI_FEATURE_QR_DECODE));
        cJSON_AddItemToObject(data, "ai_status", ai);
    }

    /* System */
    cJSON_AddStringToObject(data, "firmware_version", FW_VERSION);
    cJSON_AddNumberToObject(data, "free_heap",
        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(data, "min_heap",
        (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(data, "free_psram",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* S3 片内温度（对齐 seeed/luatos；经典 ESP32 无此传感器故不提供） */
    cJSON_AddNumberToObject(data, "chip_temp", (double)chip_temp_read());
    cJSON_AddNumberToObject(data, "stream_clients",
        mjpeg_stream_client_count());
    cJSON_AddNumberToObject(data, "stream_clients_max", 2);
    cJSON_AddNumberToObject(data, "uptime",
        (double)(esp_timer_get_time() / 1000000));

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /config                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    cJSON_AddStringToObject(data, "wifi_ssid",        config_get_wifi_ssid());
    /* Mask password */
    if (config_get_wifi_pass() && config_get_wifi_pass()[0]) {
        cJSON_AddStringToObject(data, "wifi_pass", "****");
    } else {
        cJSON_AddStringToObject(data, "wifi_pass", "");
    }
    cJSON_AddStringToObject(data, "wifi_ssid_2",      config_get_wifi_ssid_2());
    if (config_get_wifi_pass_2() && config_get_wifi_pass_2()[0]) {
        cJSON_AddStringToObject(data, "wifi_pass_2", "****");
    } else {
        cJSON_AddStringToObject(data, "wifi_pass_2", "");
    }
    cJSON_AddStringToObject(data, "device_name",      config_get_device_name());
    cJSON_AddNumberToObject(data, "cam_framesize",    config_get_cam_framesize());
    cJSON_AddNumberToObject(data, "cam_quality",      config_get_cam_quality());
    cJSON_AddBoolToObject(data,   "ai_face_enable",   config_get_ai_face_enable());
    cJSON_AddBoolToObject(data,   "ai_motion_enable", config_get_ai_motion_enable());
    cJSON_AddBoolToObject(data,   "ai_qr_enable",     config_get_ai_qr_enable());
    cJSON_AddStringToObject(data, "rtsp_user",        config_get_rtsp_user());
    if (config_get_rtsp_pass() && config_get_rtsp_pass()[0]) {
        cJSON_AddStringToObject(data, "rtsp_pass", "****");
    } else {
        cJSON_AddStringToObject(data, "rtsp_pass", "");
    }
    cJSON_AddBoolToObject(data,   "onvif_enable",     config_get_onvif_enable());
    cJSON_AddNumberToObject(data, "cam_brightness", config_get_cam_brightness());
    cJSON_AddNumberToObject(data, "cam_contrast",   config_get_cam_contrast());
    cJSON_AddNumberToObject(data, "cam_saturation", config_get_cam_saturation());
    cJSON_AddNumberToObject(data, "cam_sharpness",  config_get_cam_sharpness());
    cJSON_AddBoolToObject(data,   "cam_hmirror",    config_get_cam_hmirror());
    cJSON_AddBoolToObject(data,   "cam_vflip",      config_get_cam_vflip());

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /config                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char *body = read_body(req, 2048);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }
    
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }
    
    /* Auth state machine */
    const char *stored_pass = config_get_web_password();
    bool password_empty = !stored_pass || stored_pass[0] == '\0';
    
    if (password_empty) {
        /* State A: only allow if body contains web_password field */
        cJSON *pw = cJSON_GetObjectItem(json, "web_password");
        if (!pw || !cJSON_IsString(pw) || !pw->valuestring[0]) {
            cJSON_Delete(json);
            return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
        }
        /* Fall through — config_set will save the password */
    } else {
        /* State B: require X-Password header */
        if (!check_auth(req)) {
            cJSON_Delete(json);
            return json_error(req, "unauthorized", HTTPD_401_UNAUTHORIZED);
        }
    }
    /* Iterate over known config keys and apply via config_set() */
    cJSON *item;
    int updated = 0;
    bool wifi_changed = false;
    const char *known_keys[] = {
        /* 修复：web_password 此前不在白名单，首次设密实际从未持久化 */
        "wifi_ssid", "wifi_pass", "wifi_ssid_2", "wifi_pass_2", "web_password", "device_name", "cam_framesize", "cam_quality",
        "ai_face_enable", "ai_motion_enable", "ai_qr_enable",
        "rtsp_user", "rtsp_pass", "onvif_enable",
        "cam_brightness", "cam_contrast", "cam_saturation", "cam_sharpness",
        "cam_hmirror", "cam_vflip",
        NULL
    };

    for (int i = 0; known_keys[i]; i++) {
        item = cJSON_GetObjectItem(json, known_keys[i]);
        if (!item) continue;
        if (strcmp(known_keys[i], "wifi_ssid") == 0 ||
            strcmp(known_keys[i], "wifi_pass") == 0 ||
            strcmp(known_keys[i], "wifi_ssid_2") == 0 ||
            strcmp(known_keys[i], "wifi_pass_2") == 0) {
            wifi_changed = true;
        }
        /* 契约 v1.1：拒绝空/过短密码 */
        if (strcmp(known_keys[i], "web_password") == 0) {
            if (strlen(item->valuestring) < 6) {
                cJSON_Delete(json);
                return json_error(req, "web_password must be at least 6 characters", HTTPD_400_BAD_REQUEST);
            }
        }
        /* 画质边界（2026-09-04，驱动不变量）：q<10 撞 esp32-camera 的 w*h/5
         * JPEG fb 预算产生截断帧，PIT-021 */
        if (strcmp(known_keys[i], "cam_quality") == 0 && cJSON_IsNumber(item)) {
            if (item->valueint < CAMERA_QUALITY_MIN || item->valueint > CAMERA_QUALITY_MAX) {
                char msg[64];
                snprintf(msg, sizeof(msg), "cam_quality out of range (%d-%d)",
                         CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
                cJSON_Delete(json);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
        }

        char value_str[64];
        if (cJSON_IsBool(item) || cJSON_IsNumber(item)) {
            snprintf(value_str, sizeof(value_str), "%d", item->valueint);
        } else if (cJSON_IsString(item)) {
            if (strcmp(item->valuestring, "****") == 0) {
                continue;  /* unchanged */
            }
            snprintf(value_str, sizeof(value_str), "%s", item->valuestring);
        } else {
            continue;
        }

        if (config_set(known_keys[i], value_str) == ESP_OK) {
            updated++;
        }
    }

    cJSON_Delete(json);

    if (updated > 0) {
        config_save();
    }

    if (wifi_changed) {
        ESP_LOGW(TAG, "WiFi config changed -- rebooting in 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    cJSON *resp_data = cJSON_CreateObject();
    if (resp_data) {
        cJSON_AddNumberToObject(resp_data, "updated", updated);
    }
    return json_ok(req, resp_data);
}

/* ------------------------------------------------------------------ */
/*  POST /led                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t api_led_handler(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    
    char *body = read_body(req, 256);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    cJSON *brightness_item = cJSON_GetObjectItem(json, "brightness");
    if (!brightness_item || !cJSON_IsNumber(brightness_item)) {
        cJSON_Delete(json);
        return json_error(req, "Missing or invalid 'brightness' (0-100)", HTTPD_400_BAD_REQUEST);
    }

    int brightness = brightness_item->valueint;
    if (brightness < 0 || brightness > 100) {
        cJSON_Delete(json);
        return json_error(req, "Brightness must be 0-100", HTTPD_400_BAD_REQUEST);
    }

    cJSON_Delete(json);

    esp_err_t err = flash_led_set_brightness((uint8_t)brightness);
    if (err != ESP_OK) {
        return json_error(req, "Flash LED control failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddNumberToObject(data, "brightness", brightness);
    }
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /ai                                                           */
/* ------------------------------------------------------------------ */

static esp_err_t api_ai_handler(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    
    char *body = read_body(req, 512);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    /* Toggle AI features — persist config AND apply live via ai_enable(). */
    cJSON *item;
    int updated = 0;

    item = cJSON_GetObjectItem(json, "face");
    if (item && cJSON_IsBool(item)) {
        config_set("ai_face_enable", item->valueint ? "1" : "0");
        ai_enable(AI_FEATURE_FACE_DETECT, item->valueint ? true : false);
        ESP_LOGI(TAG, "AI face detection %s", item->valueint ? "enabled" : "disabled");
        updated++;
    }

    item = cJSON_GetObjectItem(json, "motion");
    if (item && cJSON_IsBool(item)) {
        config_set("ai_motion_en", item->valueint ? "1" : "0");   /* NVS 键 ≤15 字符（PIT-022） */
        ai_enable(AI_FEATURE_MOTION_DETECT, item->valueint ? true : false);
        ESP_LOGI(TAG, "AI motion detection %s", item->valueint ? "enabled" : "disabled");
        updated++;
    }

    item = cJSON_GetObjectItem(json, "qr");
    if (item && cJSON_IsBool(item)) {
        config_set("ai_qr_enable", item->valueint ? "1" : "0");
        ai_enable(AI_FEATURE_QR_DECODE, item->valueint ? true : false);
        ESP_LOGI(TAG, "AI QR detection %s", item->valueint ? "enabled" : "disabled");
        updated++;
    }

    cJSON_Delete(json);

    if (updated > 0) {
        config_save();
    }

    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddNumberToObject(data, "updated", updated);
        cJSON_AddBoolToObject(data, "face",   config_get_ai_face_enable());
        cJSON_AddBoolToObject(data, "motion", config_get_ai_motion_enable());
        cJSON_AddBoolToObject(data, "qr",     config_get_ai_qr_enable());
    }
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /ai/status                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t ai_status_get_handler(httpd_req_t *req)
{
    ai_result_t result;
    if (!ai_get_result(&result)) {
        return json_error(req, "AI pipeline not running", HTTPD_404_NOT_FOUND);
    }

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* Face */
    cJSON *face = cJSON_CreateObject();
    cJSON_AddNumberToObject(face, "count", result.face.count);
    cJSON *boxes = cJSON_CreateArray();
    for (int i = 0; i < result.face.count && i < AI_MAX_FACES; i++) {
        cJSON *box = cJSON_CreateObject();
        cJSON_AddNumberToObject(box, "x", result.face.faces[i].x);
        cJSON_AddNumberToObject(box, "y", result.face.faces[i].y);
        cJSON_AddNumberToObject(box, "w", result.face.faces[i].w);
        cJSON_AddNumberToObject(box, "h", result.face.faces[i].h);
        cJSON_AddNumberToObject(box, "confidence", result.face.faces[i].confidence);
        cJSON_AddItemToArray(boxes, box);
    }
    cJSON_AddItemToObject(face, "boxes", boxes);
    cJSON_AddItemToObject(data, "face", face);

    /* Motion */
    cJSON *motion = cJSON_CreateObject();
    cJSON_AddNumberToObject(motion, "score", result.motion.score);
    cJSON_AddItemToObject(data, "motion", motion);

    /* QR */
    cJSON *qr = cJSON_CreateObject();
    cJSON_AddNumberToObject(qr, "count", result.qr.count);
    cJSON *codes = cJSON_CreateArray();
    for (int i = 0; i < result.qr.count && i < AI_MAX_QR_CODES; i++) {
        cJSON_AddItemToArray(codes, cJSON_CreateString(result.qr.strings[i]));
    }
    cJSON_AddItemToObject(qr, "codes", codes);
    cJSON_AddItemToObject(data, "qr", qr);

    /* Sequence */
    cJSON_AddNumberToObject(data, "seq", result.frame_seq);

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/capabilities                                              */
/* ------------------------------------------------------------------ */

static esp_err_t api_capabilities_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    
    /* 契约 v1.0：12 个布尔能力位 + api_version/wifi_scan（见 docs/api-contract.md） */
    cJSON_AddStringToObject(data, "api_version", "1.1");
    cJSON_AddBoolToObject(data, "wifi_scan", true);
    cJSON_AddBoolToObject(data, "ai",        true);   /* Has AI pipeline */
    cJSON_AddBoolToObject(data, "sd",        false);  /* No SD card */
    cJSON_AddBoolToObject(data, "audio",     false);  /* No audio */
    cJSON_AddBoolToObject(data, "mic",       false);  /* No mic */
    cJSON_AddBoolToObject(data, "flash_led", true);   /* Has flash LED */
    cJSON_AddBoolToObject(data, "recording", false);  /* No recording */
    cJSON_AddBoolToObject(data, "timelapse", false);  /* No timelapse */
    cJSON_AddBoolToObject(data, "onvif",     true);   /* Has ONVIF */
    cJSON_AddBoolToObject(data, "rtsp",      true);   /* Has RTSP */
    cJSON_AddBoolToObject(data, "websocket", false);  /* No WebSocket */
    cJSON_AddBoolToObject(data, "mdns",      true);   /* Has mDNS */
    
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /camera                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t api_camera_get_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    cJSON_AddNumberToObject(data, "cam_framesize",  config_get_cam_framesize());
    cJSON_AddNumberToObject(data, "cam_quality",    config_get_cam_quality());
    /* 契约扩展（2026-09-04）：画质滑杆边界由板端声明，前端据此钳制输入 */
    cJSON_AddNumberToObject(data, "quality_min",    CAMERA_QUALITY_MIN);
    cJSON_AddNumberToObject(data, "quality_max",    CAMERA_QUALITY_MAX);
    cJSON_AddNumberToObject(data, "cam_brightness", config_get_cam_brightness());
    cJSON_AddNumberToObject(data, "cam_contrast",   config_get_cam_contrast());
    cJSON_AddNumberToObject(data, "cam_saturation", config_get_cam_saturation());
    cJSON_AddNumberToObject(data, "cam_sharpness",  config_get_cam_sharpness());
    cJSON_AddBoolToObject(data,   "cam_hmirror",    config_get_cam_hmirror());
    cJSON_AddBoolToObject(data,   "cam_vflip",      config_get_cam_vflip());
    cJSON_AddStringToObject(data, "resolution", camera_framesize_name(config_get_cam_framesize()));

    /* 契约 v1.0：分辨率列表动态下发（esp32-camera framesize_t 刻度）。
     * 三层上限（2026-09-04 家族统一）：sensor ∩ board ∩ memory，
     * 上限历史依据（SVGA 稳定/XGA 起冷启动取帧死）见 camera_driver.h。 */
    {
        static const char *res_labels[] = {
            [10] "VGA (640x480)",  [11] "SVGA (800x600)",
            [12] "XGA (1024x768)", [13] "HD (1280x720)",
            [14] "SXGA (1280x1024)", [15] "UXGA (1600x1200)",
        };
        int eff_max = camera_get_effective_max_res();
        cJSON *res_arr = cJSON_CreateArray();
        for (int fs = 10; fs <= eff_max && fs < (int)(sizeof(res_labels) / sizeof(res_labels[0])); fs++) {
            if (!res_labels[fs]) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", res_labels[fs]);
            cJSON_AddNumberToObject(item, "value", fs);
            cJSON_AddItemToArray(res_arr, item);
        }
        cJSON_AddItemToObject(data, "supported_resolutions", res_arr);
        /* 契约扩展（2026-09-04）：上限被哪一层钳制（sensor/board/memory） */
        cJSON_AddStringToObject(data, "res_cap_source", camera_res_cap_source());
    }

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /camera                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t api_camera_post_handler(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }
    
    char *body = read_body(req, 2048);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    cJSON *item;
    bool need_sensor_apply = false;
    bool need_reinit = false;
    uint8_t new_framesize = config_get_cam_framesize();
    uint8_t new_quality = config_get_cam_quality();
    int updated = 0;

    /* cam_framesize — 板级上限内自由选择（区间校验，非单值锁定） */
    item = cJSON_GetObjectItem(json, "cam_framesize");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
            if (val < 0 || val > 15) {
            cJSON_Delete(json);
            return json_error(req, "cam_framesize out of range (0-15)", HTTPD_400_BAD_REQUEST);
        }
        if (val > camera_get_effective_max_res()) {
            cJSON_Delete(json);
            char msg[96];
            snprintf(msg, sizeof(msg),
                "cam_framesize %d exceeds effective max %d (source: %s, PIT-021)",
                val, camera_get_effective_max_res(), camera_res_cap_source());
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        /* AI safety check — reject non-VGA if any AI feature is enabled */
        if (!camera_framesize_is_vga((uint8_t)val) &&
            (ai_is_enabled(AI_FEATURE_FACE_DETECT) ||
             ai_is_enabled(AI_FEATURE_MOTION_DETECT) ||
             ai_is_enabled(AI_FEATURE_QR_DECODE))) {
            cJSON_Delete(json);
            return json_error(req, "Disable AI to use non-VGA resolution", HTTPD_400_BAD_REQUEST);
        }
        new_framesize = (uint8_t)val;
        need_reinit = true;
    }

    /* cam_quality */
    item = cJSON_GetObjectItem(json, "cam_quality");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < CAMERA_QUALITY_MIN || val > CAMERA_QUALITY_MAX) {
            char msg[64];
            snprintf(msg, sizeof(msg), "cam_quality out of range (%d-%d)",
                     CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
            cJSON_Delete(json);
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        new_quality = (uint8_t)val;
        need_reinit = true;
    }

    /* Sensor keys: brightness, contrast, saturation, sharpness */
    const char *sensor_int_keys[] = { "cam_brightness", "cam_contrast", "cam_saturation", "cam_sharpness" };
    for (size_t i = 0; i < 4; i++) {
        item = cJSON_GetObjectItem(json, sensor_int_keys[i]);
        if (item && cJSON_IsNumber(item)) {
            int val = item->valueint;
            if (val < -2 || val > 2) {
                cJSON_Delete(json);
                char msg[64];
                snprintf(msg, sizeof(msg), "%s out of range (-2..+2)", sensor_int_keys[i]);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", val);
            config_set(sensor_int_keys[i], buf);
            need_sensor_apply = true;
            updated++;
        }
    }

    /* Sensor keys: hmirror, vflip */
    const char *sensor_bool_keys[] = { "cam_hmirror", "cam_vflip" };
    for (size_t i = 0; i < 2; i++) {
        item = cJSON_GetObjectItem(json, sensor_bool_keys[i]);
        if (item && cJSON_IsBool(item)) {
            config_set(sensor_bool_keys[i], item->valueint ? "1" : "0");
            need_sensor_apply = true;
            updated++;
        }
    }

    /* Persist config changes */
    if (updated > 0) {
        config_save();
    }

    /* Apply sensor settings live */
    if (need_sensor_apply) {
        camera_apply_sensor_settings();
    }

    /* Coordinated reinit for framesize/quality changes */
    if (need_reinit) {
        esp_err_t err = camera_reinit(new_framesize, new_quality);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return json_error(req, "Camera reinit failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    cJSON_Delete(json);

    /* Return current state (reuses GET handler) */
    return api_camera_get_handler(req);
}

/* ------------------------------------------------------------------ */
/*  OPTIONS — CORS preflight                                           */
/* ------------------------------------------------------------------ */

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/capture — 单帧 JPEG（契约 v1.0 核心端点）                  */
/* ------------------------------------------------------------------ */

static esp_err_t api_capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        return json_error(req, "Capture failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");
    httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/scan — WiFi 扫描（阻塞式，按 RSSI 降序）                  */
/* ------------------------------------------------------------------ */

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        return json_error(req, "Scan failed (STA not ready?)", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;

    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * (n ? n : 1));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    /* 按 RSSI 降序（简单插入排序，n≤20） */
    for (int i = 1; i < (int)n; i++) {
        wifi_ap_record_t key = recs[i];
        int j = i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < (int)n; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, ap);
    }
    free(recs);
    cJSON_AddItemToObject(data, "networks", arr);
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reset · POST /api/reboot · GET /api/auth                 */
/* ------------------------------------------------------------------ */

static esp_err_t api_reset_handler(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) return ret;

    ESP_LOGW(TAG, "Factory reset requested via web API");
    config_reset();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "Rebooting...");
    json_ok(req, data);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* not reached */
}

static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) return ret;

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "Rebooting...");
    json_ok(req, data);
    ESP_LOGW(TAG, "Reboot requested via web API");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* not reached */
}

static esp_err_t api_auth_handler(httpd_req_t *req)
{
    const char *stored = config_get_web_password();

    if (!stored || stored[0] == '\0') {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "auth", true);
        cJSON_AddBoolToObject(data, "password_set", false);
        return json_ok(req, data);
    }
    if (check_auth(req)) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "auth", true);
        cJSON_AddBoolToObject(data, "password_set", true);
        return json_ok(req, data);
    }
    return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
}

/* ------------------------------------------------------------------ */
/*  POST /api/time — 手动设置系统时间                                   */
/* ------------------------------------------------------------------ */

static esp_err_t api_time_handler(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) return ret;

    char *body = read_body(req, 256);
    if (!body) return json_error(req, "Empty body", HTTPD_400_BAD_REQUEST);
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);

    cJSON *jy = cJSON_GetObjectItem(json, "year");
    cJSON *jmo = cJSON_GetObjectItem(json, "month");
    cJSON *jd = cJSON_GetObjectItem(json, "day");
    cJSON *jh = cJSON_GetObjectItem(json, "hour");
    cJSON *jmi = cJSON_GetObjectItem(json, "min");
    cJSON *js = cJSON_GetObjectItem(json, "sec");

    if (!cJSON_IsNumber(jy) || !cJSON_IsNumber(jmo) || !cJSON_IsNumber(jd) ||
        !cJSON_IsNumber(jh) || !cJSON_IsNumber(jmi) || !cJSON_IsNumber(js)) {
        cJSON_Delete(json);
        return json_error(req, "Missing time fields", HTTPD_400_BAD_REQUEST);
    }

    struct tm tm_now = {
        .tm_year = jy->valueint - 1900,
        .tm_mon = jmo->valueint - 1,
        .tm_mday = jd->valueint,
        .tm_hour = jh->valueint,
        .tm_min = jmi->valueint,
        .tm_sec = js->valueint,
    };
    time_t epoch = mktime(&tm_now);
    cJSON_Delete(json);

    if (epoch < (time_t)1577836800) {  /* < 2020-01-01: 非法日期 */
        return json_error(req, "Invalid date", HTTPD_400_BAD_REQUEST);
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return json_ok(req, NULL);
}

/* ------------------------------------------------------------------ */
/*  GET /api/led — 闪光灯亮度读取（契约 v1.0：GET+POST 成对）           */
/* ------------------------------------------------------------------ */

static esp_err_t api_led_get_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "brightness", flash_led_get_brightness());
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /metrics — Prometheus 最小指标                                  */
/* ------------------------------------------------------------------ */

static esp_err_t metrics_handler(httpd_req_t *req)
{
    char buf[1024];
    int rssi = 0;
    wifi_ap_record_t ap;
    if (wifi_manager_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    int len = snprintf(buf, sizeof(buf),
        "# HELP esp_free_heap_bytes Free heap memory\n"
        "# TYPE esp_free_heap_bytes gauge\n"
        "esp_free_heap_bytes %lu\n"
        "# HELP esp_free_psram_bytes Free PSRAM memory\n"
        "# TYPE esp_free_psram_bytes gauge\n"
        "esp_free_psram_bytes %lu\n"
        "# HELP esp_min_free_heap_bytes Minimum free heap bytes since boot\n"
        "# TYPE esp_min_free_heap_bytes gauge\n"
        "esp_min_free_heap_bytes %lu\n"
        "# HELP esp_uptime_seconds System uptime in seconds\n"
        "# TYPE esp_uptime_seconds gauge\n"
        "esp_uptime_seconds %lu\n"
        "# HELP esp_wifi_rssi_dbm WiFi RSSI\n"
        "# TYPE esp_wifi_rssi_dbm gauge\n"
        "esp_wifi_rssi_dbm %d\n"
        "# HELP esp_mjpeg_clients MJPEG stream clients\n"
        "# TYPE esp_mjpeg_clients gauge\n"
        "esp_mjpeg_clients %d\n",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
        rssi,
        mjpeg_stream_client_count());

    set_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  URI handler registration table                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char    *uri;
    httpd_method_t method;
    esp_err_t    (*handler)(httpd_req_t *);
} uri_entry_t;

static const uri_entry_t s_uris[] = {
    /* Static files */
    { "/",              HTTP_GET,     static_file_handler          },
    /* REST API — 核心端点（契约 v1.0，四板一致） */
    { "/api/status",    HTTP_GET,     api_status_handler           },
    { "/api/config",    HTTP_GET,     api_config_get_handler       },
    { "/api/config",    HTTP_POST,    api_config_post_handler      },
    { "/api/capabilities", HTTP_GET,  api_capabilities_handler     },
    { "/api/capture",   HTTP_GET,     api_capture_handler          },
    { "/api/scan",      HTTP_GET,     api_scan_handler             },
    { "/api/reset",     HTTP_POST,    api_reset_handler            },
    { "/api/reboot",    HTTP_POST,    api_reboot_handler           },
    { "/api/auth",      HTTP_GET,     api_auth_handler             },
    { "/api/time",      HTTP_POST,    api_time_handler             },
    { "/metrics",       HTTP_GET,     metrics_handler              },
    /* 能力门控端点 */
    { "/api/led",       HTTP_POST,    api_led_handler              },
    { "/api/led",       HTTP_GET,     api_led_get_handler          },
    { "/api/ai",        HTTP_POST,    api_ai_handler               },
    { "/api/ai/status", HTTP_GET,     ai_status_get_handler        },
    { "/api/camera",    HTTP_GET,     api_camera_get_handler       },
    { "/api/camera",    HTTP_POST,    api_camera_post_handler      },
    /* OTA（契约 v1.1：与 seeed 同语义） */
    /* CORS preflight */
    { "/*",             HTTP_OPTIONS, options_handler              },
    /* Catch-all static files */
    { "/*",             HTTP_GET,     static_file_handler          },
};

#define NUM_URIS (sizeof(s_uris) / sizeof(s_uris[0]))

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Set TCP_NODELAY + keepalive on every new HTTP connection.
 * NODELAY: disable Nagle's algorithm so small HTTP writes (headers, MJPEG
 * boundaries) aren't delayed by up to 1 RTT on marginal WiFi.
 * KEEPALIVE: detect dead connections in ~11s (5s idle + 3×2s probes),
 * freeing up limited server sockets for new clients. */
static esp_err_t on_session_open(httpd_handle_t hd, int sockfd)
{
    int enable = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    int keepalive = 1;
    int keepidle  = 5;
    int keepintvl = 2;
    int keepcnt   = 3;
    setsockopt(sockfd, SOL_SOCKET,  SO_KEEPALIVE,  &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));
    return ESP_OK;
}

esp_err_t web_server_start(uint16_t port)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port        = port;
    config.max_uri_handlers   = NUM_URIS + 2;   /* room for future endpoints */
    config.stack_size         = 16384;
    config.max_open_sockets   = 7;
    config.uri_match_fn       = httpd_uri_match_wildcard;
    config.recv_wait_timeout  = 10;
    config.send_wait_timeout  = 10;
    config.keep_alive_enable  = false;
    config.lru_purge_enable   = true;
    config.open_fn            = on_session_open;  /* TCP_NODELAY + keepalive per socket */

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web server start failed on port %d: %s",
                 port, esp_err_to_name(ret));
        return ret;
    }

    /* Register all URI handlers */
    for (size_t i = 0; i < NUM_URIS; i++) {
        httpd_uri_t uri = {
            .uri      = s_uris[i].uri,
            .method   = s_uris[i].method,
            .handler  = s_uris[i].handler,
            .user_ctx = NULL,
        };
        ret = httpd_register_uri_handler(s_server, &uri);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register %s %s: %s",
                     (s_uris[i].method == HTTP_GET ? "GET" :
                      s_uris[i].method == HTTP_POST ? "POST" : "OPTIONS"),
                     s_uris[i].uri, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "Web server started on port %d (%zu handlers)", port, NUM_URIS);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}
