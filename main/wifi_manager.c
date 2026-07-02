/*
 * MiBee Cam v0.1 — WiFi AP/STA dual-mode manager
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wifi_manager.h"
#include "config_manager.h"
#include "status_led.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi_mgr";

/* ---- module state ------------------------------------------------- */

static EventGroupHandle_t s_event_group = NULL;
static esp_netif_t       *s_netif_sta   = NULL;
static esp_netif_t       *s_netif_ap    = NULL;

static int   s_sta_retry_count = 0;
static bool  s_ap_started      = false;   /* guard against double AP fallback */
static bool  s_sta_connected   = false;
static char  s_ip_str[16]      = "0.0.0.0";

/* event-group bits */
#define CONNECTED_BIT   BIT0

/* ---- constants ---------------------------------------------------- */
#define STA_MAX_RETRIES     3
#define STA_CONNECT_TIMEOUT_MS  15000   /* 15 s before AP fallback */
#define AP_CHANNEL          6
#define AP_MAX_CONNECTIONS  4

/* ---- forward declarations ---------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data);
static void get_ap_ssid(char *buf, size_t len);
static void start_ap(void);
static void switch_to_ap(void);

/* ------------------------------------------------------------------ */
/*  helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief  Build AP SSID from the last 2 bytes of the softAP MAC.
 *         Format: "MiBeeCam-XXXX"
 */
static void get_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "MiBeeCam-%02X%02X", mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/*  event handler                                                      */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGD(TAG, "STA started, connecting...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_sta_connected = false;
            xEventGroupClearBits(s_event_group, CONNECTED_BIT);
            s_sta_retry_count++;
            ESP_LOGW(TAG, "STA disconnected (retry %d/%d)",
                     s_sta_retry_count, STA_MAX_RETRIES);
            status_led_set_color(STATUS_LED_RED);

            if (s_sta_retry_count >= STA_MAX_RETRIES) {
                ESP_LOGW(TAG, "STA max retries reached, AP fallback");
                switch_to_ap();
            } else {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP mode started");
            status_led_set_color(STATUS_LED_BLUE);
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str),
                     IPSTR, IP2STR(&evt->ip_info.ip));
            s_sta_retry_count = 0;
            s_sta_connected   = true;
            xEventGroupSetBits(s_event_group, CONNECTED_BIT);
            status_led_set_color(STATUS_LED_GREEN);
            ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR,
                     IP2STR(&evt->ip_info.ip));
        }
    }
}

/* ------------------------------------------------------------------ */
/*  AP mode                                                            */
/* ------------------------------------------------------------------ */

/** Start fresh AP mode (no prior STA netif). */
static void start_ap(void)
{
    s_ap_started  = true;             /* mark AP started to prevent double-fallback */
    s_netif_ap = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = { 0 };
    get_ap_ssid((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.channel         = AP_CHANNEL;
    ap_config.ap.max_connection  = AP_MAX_CONNECTIONS;
    ap_config.ap.authmode        = WIFI_AUTH_OPEN;   /* open AP */
    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* static IP for AP: 192.168.4.1/24 */
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_netif_ap);
    esp_netif_set_ip_info(s_netif_ap, &ip_info);
    esp_netif_dhcps_start(s_netif_ap);

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    s_sta_connected = false;   /* not in STA mode */

    ESP_LOGI(TAG, "AP started, SSID: %s, IP: 192.168.4.1",
             ap_config.ap.ssid);
}

/**
 * @brief  Transition from STA to AP: stop WiFi, tear down STA netif,
 *         start AP.
 */
static void switch_to_ap(void)
{
    if (s_ap_started) {
        ESP_LOGD(TAG, "AP already started, ignoring duplicate switch_to_ap");
        return;
    }
    s_ap_started = true;

    esp_wifi_stop();

    if (s_netif_sta) {
        esp_netif_destroy(s_netif_sta);
        s_netif_sta = NULL;
    }

    start_ap();
}

/* ------------------------------------------------------------------ */
/*  STA mode                                                           */
/* ------------------------------------------------------------------ */

static void start_sta(void)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_config = { 0 };
    strlcpy((char *)sta_config.sta.ssid,
            config_get_wifi_ssid(),
            sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password,
            config_get_wifi_pass(),
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* auto-negotiate: OPEN~WPA3 */
    sta_config.sta.pmf_cfg.capable    = true;
    sta_config.sta.pmf_cfg.required   = false;
    sta_config.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;  /* WPA3 compat */
    sta_config.sta.listen_interval    = 3;  /* better multicast reception */

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA connecting to: %s", config_get_wifi_ssid());
    status_led_set_color(STATUS_LED_RED);
}

/* ------------------------------------------------------------------ */
/*  connection monitor (background)                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief  Task that waits up to 15 s for the STA connection.
 *         If it times out, falls back to AP mode.
 *         Self-deletes after completing.
 */
static void connection_monitor_task(void *arg)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_event_group, CONNECTED_BIT,
        pdFALSE,         /* don't clear on exit */
        pdFALSE,         /* don't require all bits (only one) */
        pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "STA timeout (%u ms), switching to AP",
                 (unsigned)STA_CONNECT_TIMEOUT_MS);
        switch_to_ap();
    } else {
        ESP_LOGI(TAG, "STA connection established within timeout");
    }

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t wifi_manager_init(void)
{
    /* ---- one-time netif + event infrastructure -------------------- */
    esp_netif_init();

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        return ESP_ERR_NO_MEM;
    }

    /* one-time WiFi stack init */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* register for all WiFi / IP events — we filter by id in the handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    /* ---- decide mode --------------------------------------------- */
    const char *ssid = config_get_wifi_ssid();

    if (ssid && strlen(ssid) > 0) {
        start_sta();

        /* background monitor: 15 s timeout → AP fallback */
        TaskHandle_t monitor_task = NULL;
        xTaskCreate(connection_monitor_task, "wifi_mon",
                    2048, NULL, 5, &monitor_task);
        (void)monitor_task;
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — starting AP mode");
        start_ap();
    }

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_sta_connected;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}
