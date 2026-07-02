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
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "cJSON.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "flash_led.h"
#include "ai_pipeline.h"
#include "camera_driver.h"
#include "esp_spiffs.h"  /* for stat on SPIFFS files */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;

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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
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

    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

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

    /* WiFi */
    cJSON_AddStringToObject(data, "wifi_ssid", config_get_wifi_ssid());
    cJSON_AddStringToObject(data, "ip", wifi_manager_get_ip());

    /* Camera */
    cJSON_AddStringToObject(data, "camera_resolution",
        camera_framesize_name(config_get_cam_framesize()));
    cJSON_AddNumberToObject(data, "camera_framesize", config_get_cam_framesize());
    cJSON_AddNumberToObject(data, "camera_quality", config_get_cam_quality());

    /* Camera sensor settings */
    cJSON_AddNumberToObject(data, "cam_brightness", config_get_cam_brightness());
    cJSON_AddNumberToObject(data, "cam_contrast",   config_get_cam_contrast());
    cJSON_AddNumberToObject(data, "cam_saturation", config_get_cam_saturation());
    cJSON_AddNumberToObject(data, "cam_sharpness",  config_get_cam_sharpness());
    cJSON_AddBoolToObject(data,   "cam_hmirror",    config_get_cam_hmirror());
    cJSON_AddBoolToObject(data,   "cam_vflip",      config_get_cam_vflip());

    /* AI status */
    cJSON *ai = cJSON_CreateObject();
    if (ai) {
        cJSON_AddBoolToObject(ai, "face",   ai_is_enabled(AI_FEATURE_FACE_DETECT));
        cJSON_AddBoolToObject(ai, "motion", ai_is_enabled(AI_FEATURE_MOTION_DETECT));
        cJSON_AddBoolToObject(ai, "qr",     ai_is_enabled(AI_FEATURE_QR_DECODE));
        cJSON_AddItemToObject(data, "ai_status", ai);
    }

    /* System */
    cJSON_AddNumberToObject(data, "free_heap",
        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(data, "free_psram",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(data, "mjpeg_clients",
        mjpeg_stream_client_count());
    cJSON_AddNumberToObject(data, "uptime",
        (double)(xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000.0);

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
    cJSON_AddNumberToObject(data, "mjpeg_clients",    mjpeg_stream_client_count());

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

    /* Iterate over known config keys and apply via config_set() */
    cJSON *item;
    int updated = 0;
    bool wifi_changed = false;
    const char *known_keys[] = {
        "wifi_ssid", "wifi_pass", "cam_framesize", "cam_quality",
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
            strcmp(known_keys[i], "wifi_pass") == 0) {
            wifi_changed = true;
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
        config_set("ai_motion_enable", item->valueint ? "1" : "0");
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
    cJSON_AddNumberToObject(data, "cam_brightness", config_get_cam_brightness());
    cJSON_AddNumberToObject(data, "cam_contrast",   config_get_cam_contrast());
    cJSON_AddNumberToObject(data, "cam_saturation", config_get_cam_saturation());
    cJSON_AddNumberToObject(data, "cam_sharpness",  config_get_cam_sharpness());
    cJSON_AddBoolToObject(data,   "cam_hmirror",    config_get_cam_hmirror());
    cJSON_AddBoolToObject(data,   "cam_vflip",      config_get_cam_vflip());
    cJSON_AddStringToObject(data, "cam_framesize_name", camera_framesize_name(config_get_cam_framesize()));

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /camera                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t api_camera_post_handler(httpd_req_t *req)
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

    cJSON *item;
    bool need_sensor_apply = false;
    bool need_reinit = false;
    uint8_t new_framesize = config_get_cam_framesize();
    uint8_t new_quality = config_get_cam_quality();
    int updated = 0;

    /* cam_framesize */
    item = cJSON_GetObjectItem(json, "cam_framesize");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val < 0 || val > 24) {
            cJSON_Delete(json);
            return json_error(req, "cam_framesize out of range (0-24)", HTTPD_400_BAD_REQUEST);
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
        if (val < 0 || val > 63) {
            cJSON_Delete(json);
            return json_error(req, "cam_quality out of range (0-63)", HTTPD_400_BAD_REQUEST);
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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
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
    { "/",          HTTP_GET,     static_file_handler     },
    /* MJPEG stream (async — spawns its own task) */
    { "/stream",    HTTP_GET,     mjpeg_stream_handler    },
    /* REST API */
    { "/status",    HTTP_GET,     api_status_handler      },
    { "/config",    HTTP_GET,     api_config_get_handler  },
    { "/config",    HTTP_POST,    api_config_post_handler },
    { "/led",       HTTP_POST,    api_led_handler         },
    { "/ai",        HTTP_POST,    api_ai_handler          },
    { "/ai/status", HTTP_GET,     ai_status_get_handler },
    { "/camera",    HTTP_GET,     api_camera_get_handler  },
    { "/camera",    HTTP_POST,    api_camera_post_handler },
    /* CORS preflight */
    { "/*",         HTTP_OPTIONS, options_handler         },
    /* Catch-all static files */
    { "/*",         HTTP_GET,     static_file_handler     },
};

#define NUM_URIS (sizeof(s_uris) / sizeof(s_uris[0]))

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

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
