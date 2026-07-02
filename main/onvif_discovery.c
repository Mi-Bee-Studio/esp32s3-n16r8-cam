/*
 * MiBee Cam v0.1 — ONVIF WS-Discovery UDP multicast listener
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Listens on UDP port 3702, multicast group 239.255.255.250 for
 * WS-Discovery Probe messages and responds with ProbeMatches.
 * Sends periodic multicast Hello announcements so NVRs can discover
 * the device without probing.
 *
 * References:
 *   - ONVIF Network Interface Specification
 *   - WS-Discovery 1.1 (http://schemas.xmlsoap.org/ws/2005/04/discovery)
 */

#include "onvif_discovery.h"
#include "onvif_service.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "onvif_disc";

#define ONVIF_DISCOVERY_PORT   3702
#define ONVIF_MULTICAST_GROUP  "239.255.255.250"
#define PROBE_BUF_SIZE         4096
#define RESP_BUF_SIZE          2048
#define TASK_STACK_SIZE        6144
#define TASK_PRIORITY          2
#define TASK_CORE              1

/* Fixed UUID prefix for MAC-based device UUID.
 * Format: f472b01e-0000-1000-8000-{MAC6bytes} */
#define UUID_PREFIX "f472b01e-0000-1000-8000-"

/* Task handle for stopping the discovery task */
static TaskHandle_t s_disc_task = NULL;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void generate_device_uuid(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size,
             UUID_PREFIX "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool extract_message_id(const char *body, size_t body_len,
                                char *out, size_t out_size)
{
    const char *tag = strstr(body, "MessageID");
    if (!tag) return false;

    const char *gt = strchr(tag, '>');
    if (!gt || (size_t)(gt - body) >= body_len) return false;
    gt++;

    const char *lt = strchr(gt, '<');
    if (!lt || (size_t)(lt - body) > body_len) return false;

    size_t len = lt - gt;
    if (len == 0 || len >= out_size) return false;

    memcpy(out, gt, len);
    out[len] = '\0';
    return true;
}

static bool is_probe_message(const char *body, size_t body_len)
{
    (void)body_len;
    if ((strstr(body, "wsdiscovery:Probe") ||
         strstr(body, "ws-discovery:Probe") ||
         strstr(body, ":Probe")) &&
        !strstr(body, "ProbeMatches")) {
        return true;
    }
    return false;
}

static void build_probe_matches(const char *relates_to,
                                 const char *device_uuid,
                                 const char *ip_str,
                                 char *out, size_t out_size)
{
    snprintf(out, out_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:wsdp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\">"
        "<soap:Header>"
        "<wsa:Action>"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"
        "</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
        "<wsa:RelatesTo>%s</wsa:RelatesTo>"
        "<wsa:To>"
        "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
        "</wsa:To>"
        "<wsd:AppSequence InstanceId=\"14200\" MessageNumber=\"1\"/>"
        "</soap:Header>"
        "<soap:Body>"
        "<wsd:ProbeMatches>"
        "<wsd:ProbeMatch>"
        "<wsa:EndpointReference>"
        "<wsa:Address>urn:uuid:%s</wsa:Address>"
        "</wsa:EndpointReference>"
        "<wsd:Types>tns:NetworkVideoTransmitter</wsd:Types>"
        "<wsd:Scopes>"
        "onvif://www.onvif.org/type/video_encoder "
        "onvif://www.onvif.org/type/NetworkVideoTransmitter "
        "onvif://www.onvif.org/Hardware/MiBeeCam "
        "onvif://www.onvif.org/Name/MiBeeCam "
        "onvif://www.onvif.org/Profile/Streaming"
        "</wsd:Scopes>"
        "<wsd:XAddrs>http://%s:80/onvif/device_service</wsd:XAddrs>"
        "<wsd:MetadataVersion>2</wsd:MetadataVersion>"
        "</wsd:ProbeMatch>"
        "</wsd:ProbeMatches>"
        "</soap:Body>"
        "</soap:Envelope>",
        device_uuid,
        relates_to ? relates_to : "",
        device_uuid,
        ip_str);
}

static void build_hello_message(const char *device_uuid,
                                const char *ip_str,
                                char *out, size_t out_size)
{
    snprintf(out, out_size,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope"
        " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:wsdp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\">"
        "<soap:Header>"
        "<wsa:Action>"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello"
        "</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
        "<wsa:To>"
        "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
        "</wsa:To>"
        "<wsd:AppSequence InstanceId=\"14200\" MessageNumber=\"1\"/>"
        "</soap:Header>"
        "<soap:Body>"
        "<wsd:Hello>"
        "<wsa:EndpointReference>"
        "<wsa:Address>urn:uuid:%s</wsa:Address>"
        "</wsa:EndpointReference>"
        "<wsd:Types>tns:NetworkVideoTransmitter</wsd:Types>"
        "<wsd:Scopes>"
        "onvif://www.onvif.org/type/video_encoder "
        "onvif://www.onvif.org/type/NetworkVideoTransmitter "
        "onvif://www.onvif.org/Hardware/MiBeeCam "
        "onvif://www.onvif.org/Name/MiBeeCam "
        "onvif://www.onvif.org/Profile/Streaming"
        "</wsd:Scopes>"
        "<wsd:XAddrs>http://%s:80/onvif/device_service</wsd:XAddrs>"
        "<wsd:MetadataVersion>2</wsd:MetadataVersion>"
        "</wsd:Hello>"
        "</soap:Body>"
        "</soap:Envelope>",
        device_uuid,
        device_uuid,
        ip_str);
}

/* ------------------------------------------------------------------ */
/*  Discovery Task                                                     */
/* ------------------------------------------------------------------ */

static void onvif_discovery_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ONVIF discovery task started");

    char device_uuid[64];
    generate_device_uuid(device_uuid, sizeof(device_uuid));
    ESP_LOGI(TAG, "Device UUID: %s", device_uuid);

    char *recv_buf = (char *)heap_caps_malloc(PROBE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM receive buffer");
        vTaskDelete(NULL);
        return;
    }

    int sock = -1;

    while (1) {
        /* Check if we should stop */
        if (s_disc_task == NULL) {
            break;
        }

        /* Close previous socket if reconnecting */
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }

        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            ESP_LOGW(TAG, "Failed to create socket, retrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(ONVIF_DISCOVERY_PORT);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind port %d, retrying in 10s",
                     ONVIF_DISCOVERY_PORT);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Join multicast group using actual STA IP */
        const char *local_ip = wifi_manager_get_ip();
        if (!local_ip || strcmp(local_ip, "0.0.0.0") == 0) {
            ESP_LOGW(TAG, "No IP yet, retrying in 10s");
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        struct ip_mreq imreq;
        memset(&imreq, 0, sizeof(imreq));
        imreq.imr_interface.s_addr = inet_addr(local_ip);
        imreq.imr_multiaddr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &imreq, sizeof(imreq)) < 0) {
            ESP_LOGW(TAG, "Failed to join multicast on %s, retrying in 10s",
                     local_ip);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        ESP_LOGI(TAG, "Joined multicast %s on %s",
                 ONVIF_MULTICAST_GROUP, local_ip);

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ESP_LOGI(TAG, "Listening for ONVIF Probe on UDP %s:%d",
                 ONVIF_MULTICAST_GROUP, ONVIF_DISCOVERY_PORT);

        /* Send initial multicast Hello */
        {
            const char *ip_str = wifi_manager_get_ip();
            if (ip_str && strcmp(ip_str, "0.0.0.0") != 0) {
                struct in_addr local_addr;
                local_addr.s_addr = inet_addr(ip_str);
                setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                           &local_addr, sizeof(local_addr));

                int ttl = 2;
                setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

                char hello_buf[RESP_BUF_SIZE];
                build_hello_message(device_uuid, ip_str,
                                    hello_buf, sizeof(hello_buf));
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                dest.sin_port = htons(ONVIF_DISCOVERY_PORT);
                dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
                sendto(sock, hello_buf, strlen(hello_buf), 0,
                       (struct sockaddr *)&dest, sizeof(dest));
                ESP_LOGI(TAG, "Sent initial Hello to %s:%d",
                         ONVIF_MULTICAST_GROUP, ONVIF_DISCOVERY_PORT);
            }
        }

        int hello_counter = 0;

        /* Main receive loop */
        while (1) {
            if (s_disc_task == NULL) {
                break;
            }

            struct sockaddr_in sender_addr;
            socklen_t addr_len = sizeof(sender_addr);
            memset(recv_buf, 0, PROBE_BUF_SIZE);

            int recv_len = recvfrom(sock, recv_buf, PROBE_BUF_SIZE - 1, 0,
                                    (struct sockaddr *)&sender_addr, &addr_len);
            if (recv_len <= 0) {
                /* Timeout — periodically resend Hello */
                hello_counter++;
                if (hello_counter >= 6) {
                    hello_counter = 0;
                    const char *ip_str = wifi_manager_get_ip();
                    if (ip_str && strcmp(ip_str, "0.0.0.0") != 0) {
                        char hello_buf[RESP_BUF_SIZE];
                        build_hello_message(device_uuid, ip_str,
                                            hello_buf, sizeof(hello_buf));
                        struct sockaddr_in dest;
                        memset(&dest, 0, sizeof(dest));
                        dest.sin_family = AF_INET;
                        dest.sin_port = htons(ONVIF_DISCOVERY_PORT);
                        dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
                        struct in_addr local_addr;
                        local_addr.s_addr = inet_addr(ip_str);
                        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                                   &local_addr, sizeof(local_addr));
                        sendto(sock, hello_buf, strlen(hello_buf), 0,
                               (struct sockaddr *)&dest, sizeof(dest));
                        ESP_LOGI(TAG, "Resent Hello (periodic)");
                    }
                }
                continue;
            }

            recv_buf[recv_len] = '\0';

            if (!is_probe_message(recv_buf, recv_len)) {
                continue;
            }

            const char *ip_str = wifi_manager_get_ip();
            if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
                ESP_LOGW(TAG, "WiFi not connected, skipping Probe response");
                continue;
            }

            char relates_to[128] = {0};
            if (!extract_message_id(recv_buf, recv_len,
                                     relates_to, sizeof(relates_to))) {
                strncpy(relates_to, "urn:uuid:unknown", sizeof(relates_to) - 1);
            }

            ESP_LOGI(TAG, "Received Probe from %s, sending ProbeMatches",
                     inet_ntoa(sender_addr.sin_addr));

            char resp_buf[RESP_BUF_SIZE];
            build_probe_matches(relates_to, device_uuid, ip_str,
                                resp_buf, sizeof(resp_buf));

            struct in_addr if_addr;
            if_addr.s_addr = inet_addr(ip_str);
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                       &if_addr, sizeof(if_addr));
            int sent = sendto(sock, resp_buf, strlen(resp_buf), 0,
                               (struct sockaddr *)&sender_addr,
                               sizeof(sender_addr));
            ESP_LOGI(TAG, "ProbeMatches sent to %s:%d, result=%d",
                     inet_ntoa(sender_addr.sin_addr),
                     ntohs(sender_addr.sin_port), sent);
        }
    }

    if (recv_buf) free(recv_buf);
    if (sock >= 0) close(sock);
    vTaskDelete(NULL);
}

static esp_err_t onvif_discovery_init(void)
{
    if (s_disc_task != NULL) {
        ESP_LOGW(TAG, "Discovery task already running");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        onvif_discovery_task,
        "onvif_disc",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        &s_disc_task,
        TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ONVIF discovery task");
        s_disc_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ONVIF discovery task created");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  mDNS                                                               */
/* ------------------------------------------------------------------ */

static esp_err_t init_mdns(void)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set hostname based on STA MAC */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "mibeecam-%02x%02x",
             mac[4], mac[5]);
    mdns_hostname_set(hostname);
    mdns_instance_name_set("MiBee Cam");

    /* Add _onvif._tcp service on port 80 */
    mdns_service_add(NULL, "_onvif", "_tcp", 80, NULL, 0);
    mdns_service_txt_item_set("_onvif", "_tcp", "txtvers", "1");

    ESP_LOGI(TAG, "mDNS initialized: %s.local (_onvif._tcp port 80)",
             hostname);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t onvif_start(void)
{
    if (!config_get_onvif_enable()) {
        ESP_LOGI(TAG, "ONVIF disabled by config");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting ONVIF services...");

    /* 1. mDNS */
    esp_err_t ret = init_mdns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed, ONVIF may not work");
        /* Non-fatal — continue */
    }

    /* 2. Register SOAP handlers on the web server */
    httpd_handle_t server = web_server_get_handle();
    if (server) {
        ret = onvif_register_handlers(server);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register ONVIF SOAP handlers");
            return ret;
        }
    } else {
        ESP_LOGW(TAG, "Web server not available, ONVIF SOAP handlers skipped");
    }

    /* 3. Start WS-Discovery listener */
    ret = onvif_discovery_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WS-Discovery");
        return ret;
    }

    ESP_LOGI(TAG, "ONVIF services started");
    return ESP_OK;
}

void onvif_stop(void)
{
    /* Signal discovery task to stop */
    TaskHandle_t task = s_disc_task;
    s_disc_task = NULL;
    if (task) {
        vTaskDelete(task);
    }

    /* Remove mDNS service */
    mdns_service_remove("_onvif", "_tcp");
    mdns_free();

    ESP_LOGI(TAG, "ONVIF services stopped");
}
