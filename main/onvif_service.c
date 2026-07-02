/*
 * MiBee Cam v0.1 — Minimal ONVIF SOAP service handlers for NVR discovery
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the minimum SOAP actions required for NVRs to discover and
 * add this camera: GetSystemDateAndTime, GetDeviceInformation,
 * GetCapabilities, GetProfiles, GetStreamUri, GetSnapshot.
 *
 * No XML parser is used — action detection via strstr(), response
 * generation via snprintf().
 */

#include "onvif_service.h"
#include "wifi_manager.h"
#include "rtsp_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "onvif_svc";

/* Maximum SOAP body size we accept */
#define ONVIF_BODY_MAX  4096
#define ONVIF_RESP_MAX  4096

/* ONVIF firmware version string */
#define ONVIF_FW_VER  "v0.1.0"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static char *onvif_read_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > ONVIF_BODY_MAX) {
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

static esp_err_t send_soap_fault(httpd_req_t *req)
{
    const char *fault =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\">"
        "<soap:Body>"
        "<soap:Fault>"
        "<soap:Code>"
        "<soap:Value>soap:Sender</soap:Value>"
        "<soap:Subcode>"
        "<soap:Value>ter:ActionNotSupported</soap:Value>"
        "</soap:Subcode>"
        "</soap:Code>"
        "<soap:Reason>"
        "<soap:Text xml:lang=\"en\">Action not supported</soap:Text>"
        "</soap:Reason>"
        "</soap:Fault>"
        "</soap:Body>"
        "</soap:Envelope>";

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, fault, strlen(fault));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Device Service Actions                                             */
/* ------------------------------------------------------------------ */

static esp_err_t handle_get_system_date_and_time(httpd_req_t *req)
{
    time_t now = time(NULL);
    struct tm utc_tm;
    gmtime_r(&now, &utc_tm);

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<soap:Body>"
        "<tds:GetSystemDateAndTimeResponse>"
        "<tds:SystemDateAndTime>"
        "<tt:DateTimeType>NTP</tt:DateTimeType>"
        "<tt:DaylightSavings>false</tt:DaylightSavings>"
        "<tt:TimeZone>"
        "<tt:TZ>UTC</tt:TZ>"
        "</tt:TimeZone>"
        "<tt:UTCDateTime>"
        "<tt:Time>"
        "<tt:Hour>%d</tt:Hour>"
        "<tt:Minute>%d</tt:Minute>"
        "<tt:Second>%d</tt:Second>"
        "</tt:Time>"
        "<tt:Date>"
        "<tt:Year>%d</tt:Year>"
        "<tt:Month>%d</tt:Month>"
        "<tt:Day>%d</tt:Day>"
        "</tt:Date>"
        "</tt:UTCDateTime>"
        "</tds:SystemDateAndTime>"
        "</tds:GetSystemDateAndTimeResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
        utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t handle_get_device_information(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial_no[24];
    snprintf(serial_no, sizeof(serial_no),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
        "<soap:Body>"
        "<tds:GetDeviceInformationResponse>"
        "<tds:Manufacturer>MiBee</tds:Manufacturer>"
        "<tds:Model>MiBee Cam</tds:Model>"
        "<tds:FirmwareVersion>" ONVIF_FW_VER "</tds:FirmwareVersion>"
        "<tds:SerialNumber>%s</tds:SerialNumber>"
        "<tds:HardwareId>ESP32-S3-N16R8</tds:HardwareId>"
        "</tds:GetDeviceInformationResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        serial_no);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t handle_get_capabilities(httpd_req_t *req)
{
    const char *ip_str = wifi_manager_get_ip();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        ip_str = "0.0.0.0";
    }

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<soap:Body>"
        "<tds:GetCapabilitiesResponse>"
        "<tds:Capabilities>"
        "<tt:Device>"
        "<tt:XAddr>http://%s:80/onvif/device_service</tt:XAddr>"
        "</tt:Device>"
        "<tt:Media>"
        "<tt:XAddr>http://%s:80/onvif/media_service</tt:XAddr>"
        "</tt:Media>"
        "<tt:Events>"
        "<tt:XAddr>http://%s:80/onvif/events_service</tt:XAddr>"
        "</tt:Events>"
        "<tt:Analytics>"
        "<tt:XAddr>http://%s:80/onvif/analytics_service</tt:XAddr>"
        "</tt:Analytics>"
        "</tds:Capabilities>"
        "</tds:GetCapabilitiesResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        ip_str, ip_str, ip_str, ip_str);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Media Service Actions                                              */
/* ------------------------------------------------------------------ */

static esp_err_t handle_get_profiles(httpd_req_t *req)
{
    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetProfilesResponse>"
        "<trt:Profiles token=\"MainStream\" fixed=\"true\">"
        "<tt:Name>MainStream</tt:Name>"
        "<tt:VideoSourceConfiguration>"
        "<tt:Name>VideoSource_1</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
        "</tt:VideoSourceConfiguration>"
        "<tt:VideoEncoderConfiguration>"
        "<tt:Name>VideoEncoder_1</tt:Name>"
        "<tt:Encoding>JPEG</tt:Encoding>"
        "<tt:Resolution>"
        "<tt:Width>640</tt:Width>"
        "<tt:Height>480</tt:Height>"
        "</tt:Resolution>"
        "<tt:Quality>5</tt:Quality>"
        "<tt:RateControl>"
        "<tt:FrameRateLimit>15</tt:FrameRateLimit>"
        "<tt:EncodingInterval>1</tt:EncodingInterval>"
        "<tt:BitrateLimit>4096</tt:BitrateLimit>"
        "</tt:RateControl>"
        "</tt:VideoEncoderConfiguration>"
        "</trt:Profiles>"
        "</trt:GetProfilesResponse>"
        "</soap:Body>"
        "</soap:Envelope>");

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t handle_get_stream_uri(httpd_req_t *req)
{
    const char *rtsp_url = rtsp_get_url();
    if (!rtsp_url) {
        rtsp_url = "rtsp://0.0.0.0:554/stream";
    }

    const char *ip_str = wifi_manager_get_ip();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        ip_str = "0.0.0.0";
    }

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetStreamUriResponse>"
        "<trt:MediaUri>"
        "<tt:Uri>%s</tt:Uri>"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
        "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        "<tt:Timeout>PT10S</tt:Timeout>"
        "</trt:MediaUri>"
        "</trt:GetStreamUriResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        rtsp_url);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t handle_get_snapshot(httpd_req_t *req)
{
    const char *ip_str = wifi_manager_get_ip();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        ip_str = "0.0.0.0";
    }

    char snapshot_uri[128];
    snprintf(snapshot_uri, sizeof(snapshot_uri),
             "http://%s:80/stream/snapshot", ip_str);

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "<trt:GetSnapshotUriResponse>"
        "<trt:MediaUri>"
        "<tt:Uri>%s</tt:Uri>"
        "</trt:MediaUri>"
        "</trt:GetSnapshotUriResponse>"
        "</soap:Body>"
        "</soap:Envelope>",
        snapshot_uri);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  SOAP Action Dispatch                                               */
/* ------------------------------------------------------------------ */

static void log_unsupported(const char *body)
{
    const char *act_start = strstr(body, ":Body>");
    if (act_start) {
        act_start = strchr(act_start + 6, '<');
        if (act_start) {
            act_start++;
            const char *act_end = strchr(act_start, ' ');
            if (!act_end) act_end = strchr(act_start, '>');
            if (act_end) {
                char action[64] = {0};
                int len = act_end - act_start;
                if (len > 63) len = 63;
                memcpy(action, act_start, len);
                ESP_LOGW(TAG, "Unsupported action: %s", action);
            }
        }
    }
}

static esp_err_t dispatch_device_action(httpd_req_t *req, const char *body)
{
    if (!body) {
        return send_soap_fault(req);
    }

    if (strstr(body, "GetSystemDateAndTime")) {
        return handle_get_system_date_and_time(req);
    }
    if (strstr(body, "GetDeviceInformation")) {
        return handle_get_device_information(req);
    }
    if (strstr(body, "GetCapabilities")) {
        return handle_get_capabilities(req);
    }

    log_unsupported(body);
    return send_soap_fault(req);
}

static esp_err_t dispatch_media_action(httpd_req_t *req, const char *body)
{
    if (!body) {
        return send_soap_fault(req);
    }

    if (strstr(body, "GetProfiles")) {
        return handle_get_profiles(req);
    }
    if (strstr(body, "GetStreamUri")) {
        return handle_get_stream_uri(req);
    }
    if (strstr(body, "GetSnapshotUri") || strstr(body, "GetSnapshot")) {
        return handle_get_snapshot(req);
    }

    log_unsupported(body);
    return send_soap_fault(req);
}

/* ------------------------------------------------------------------ */
/*  HTTP Handlers                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t device_service_handler(httpd_req_t *req)
{
    char *body = onvif_read_body(req);
    if (body) {
        ESP_LOGI(TAG, "DEVICE REQ [first 200]: %.200s", body);
    }
    esp_err_t ret = dispatch_device_action(req, body);
    free(body);
    return ret;
}

static esp_err_t media_service_handler(httpd_req_t *req)
{
    char *body = onvif_read_body(req);
    if (body) {
        ESP_LOGI(TAG, "MEDIA REQ [first 200]: %.200s", body);
    }
    esp_err_t ret = dispatch_media_action(req, body);
    free(body);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t onvif_register_handlers(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "Invalid server handle");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    httpd_uri_t device_uri = {
        .uri      = "/onvif/device_service",
        .method   = HTTP_POST,
        .handler  = device_service_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &device_uri);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "Device service handler already registered");
        } else {
            ESP_LOGE(TAG, "Failed to register device service: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Registered /onvif/device_service");
    }

    httpd_uri_t media_uri = {
        .uri      = "/onvif/media_service",
        .method   = HTTP_POST,
        .handler  = media_service_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &media_uri);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "Media service handler already registered");
        } else {
            ESP_LOGE(TAG, "Failed to register media service: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Registered /onvif/media_service");
    }

    return ESP_OK;
}
