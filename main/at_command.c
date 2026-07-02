/*
 * MiBee Cam v0.1 — AT Command Interface
 *
 * Line-based AT command listener on UART0 (shared with ESP_LOG console).
 *
 * Reference pattern: ai-thinker-esp32-cam serial_config.c
 * Extended to cover WiFi, camera, AI pipeline, config, LED, system.
 *
 * Usage (at 115200 baud terminal):
 *   AT                    — handshake
 *   AT+HELP               — list all commands
 *   AT+WIFI=ssid,password — set WiFi credentials and reboot
 *   AT+INFO               — system status
 *   ...
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_wifi.h"

#include "at_command.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "camera_driver.h"
#include "flash_led.h"
#include "ai_pipeline.h"

static const char *TAG = "at_cmd";

/* ------------------------------------------------------------------ */
/*  Command table                                                     */
/* ------------------------------------------------------------------ */

typedef void (*at_handler_t)(const char *params);

typedef struct {
    const char  *prefix;    /* e.g. "AT+WIFI" */
    at_handler_t handler;
    const char  *help;
} at_command_t;

/* Forward declarations */
static void cmd_at      (const char *p);
static void cmd_help    (const char *p);
static void cmd_reboot  (const char *p);
static void cmd_reset   (const char *p);
static void cmd_version (const char *p);
static void cmd_info    (const char *p);
static void cmd_wifi    (const char *p);
static void cmd_wifiscan(const char *p);
static void cmd_cifsr   (const char *p);
static void cmd_camcap  (const char *p);
static void cmd_camqual (const char *p);
static void cmd_camres  (const char *p);
static void cmd_aiface  (const char *p);
static void cmd_aimotion(const char *p);
static void cmd_aiqr    (const char *p);
static void cmd_config  (const char *p);
static void cmd_rtspuser(const char *p);
static void cmd_rtsppass(const char *p);
static void cmd_led     (const char *p);

static const at_command_t s_commands[] = {
    { "AT",           cmd_at,       "Handshake"                              },
    { "AT+HELP",      cmd_help,     "Show this help"                         },
    { "AT+REBOOT",    cmd_reboot,   "Reboot device"                          },
    { "AT+RESET",     cmd_reset,    "Factory reset + reboot"                 },
    { "AT+VERSION",   cmd_version,  "Firmware version"                       },
    { "AT+INFO",      cmd_info,     "System info (heap, PSRAM, IP, uptime)"  },
    { "AT+WIFI",      cmd_wifi,     "AT+WIFI? | AT+WIFI=ssid,password"       },
    { "AT+WIFISCAN",  cmd_wifiscan, "Scan for WiFi APs"                      },
    { "AT+CIFSR",     cmd_cifsr,    "Get IP address"                         },
    { "AT+CAMCAP",    cmd_camcap,   "Capture one JPEG frame"                 },
    { "AT+CAMQUAL",   cmd_camqual,  "AT+CAMQUAL? | AT+CAMQUAL=n (1-63)"      },
    { "AT+CAMRES",    cmd_camres,   "AT+CAMRES? | AT+CAMRES=n (0-13)"        },
    { "AT+AIFACE",    cmd_aiface,   "AT+AIFACE? | AT+AIFACE=on/off"          },
    { "AT+AIMOTION",  cmd_aimotion, "AT+AIMOTION? | AT+AIMOTION=on/off"      },
    { "AT+AIQR",      cmd_aiqr,     "AT+AIQR? | AT+AIQR=on/off"              },
    { "AT+CONFIG",    cmd_config,   "Print all configuration"                },
    { "AT+RTSPUSER",  cmd_rtspuser, "AT+RTSPUSER=name"                       },
    { "AT+RTSPPASS",  cmd_rtsppass, "AT+RTSPPASS=pass"                       },
    { "AT+LED",       cmd_led,      "AT+LED=n (0-100 percent)"               },
};
#define N_COMMANDS ((int)(sizeof(s_commands) / sizeof(s_commands[0])))

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static bool str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static bool str_ieq(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

/* Parse "on" / "off" / "1" / "0" → bool.  Returns false on invalid. */
static bool parse_onoff(const char *s, bool *out)
{
    if (!s || !s[0]) return false;
    if (str_ieq(s, "on") || str_eq(s, "1") || str_ieq(s, "true"))  { *out = true;  return true; }
    if (str_ieq(s, "off")|| str_eq(s, "0") || str_ieq(s, "false")) { *out = false; return true; }
    return false;
}

/* Set a config key from an integer value, then save. */
static void config_set_int_and_save(const char *key, int val)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", val);
    config_set(key, buf);
    config_save();
}

/* Set a config key from a bool, then save. */
static void config_set_bool_and_save(const char *key, bool val)
{
    config_set(key, val ? "1" : "0");
    config_save();
}

/* ------------------------------------------------------------------ */
/*  Handlers                                                          */
/* ------------------------------------------------------------------ */

static void cmd_at(const char *p)
{
    (void)p;
    printf("OK\r\n");
}

static void cmd_help(const char *p)
{
    (void)p;
    printf("=== MiBee Cam AT Commands ===\r\n");
    for (int i = 0; i < N_COMMANDS; i++) {
        printf("  %-16s %s\r\n", s_commands[i].prefix, s_commands[i].help);
    }
    printf("OK\r\n");
}

static void cmd_reboot(const char *p)
{
    (void)p;
    printf("OK — rebooting...\r\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void cmd_reset(const char *p)
{
    (void)p;
    printf("OK — factory reset, rebooting...\r\n");
    fflush(stdout);
    config_reset();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void cmd_version(const char *p)
{
    (void)p;
    printf("MiBee Cam v0.1\r\n");
    printf("ESP-IDF: v6.0.1\r\n");
    printf("Built: %s %s\r\n", __DATE__, __TIME__);
    printf("OK\r\n");
}

static void cmd_info(const char *p)
{
    (void)p;
    bool connected = wifi_manager_is_connected();
    printf("Chip:      ESP32-S3\r\n");
    printf("PSRAM:     8 MB Octal\r\n");
    printf("Free heap: %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());
    printf("Free PSRAM:%lu bytes\r\n", (unsigned long)esp_get_free_internal_heap_size());
    printf("WiFi:      %s\r\n", connected ? "STA connected" : "AP mode (disconnected)");
    printf("IP:        %s\r\n", wifi_manager_get_ip());
    printf("Uptime:    %lld s\r\n", (long long)(esp_timer_get_time() / 1000000));
    printf("OK\r\n");
}

static void cmd_wifi(const char *p)
{
    /* AT+WIFI? — query */
    if (!p || p[0] == '?' || p[0] == '\0') {
        bool connected = wifi_manager_is_connected();
        const char *ssid = config_get_wifi_ssid();
        printf("Mode:   %s\r\n", connected ? "STA" : "AP");
        printf("SSID:   %s\r\n", (ssid && ssid[0]) ? ssid : "(not set)");
        printf("IP:     %s\r\n", wifi_manager_get_ip());
        printf("Status: %s\r\n", connected ? "connected" : "disconnected");
        printf("OK\r\n");
        return;
    }

    /* AT+WIFI=ssid,password */
    char *ssid = (char *)p;
    char *comma = strchr(ssid, ',');
    if (!comma) {
        printf("ERROR: Usage: AT+WIFI=ssid,password\r\n");
        return;
    }
    *comma = '\0';
    char *pass = comma + 1;

    if (strlen(ssid) == 0) {
        printf("ERROR: SSID cannot be empty\r\n");
        return;
    }

    config_set("wifi_ssid", ssid);
    config_set("wifi_pass", pass);
    config_save();

    printf("OK — WiFi set: SSID='%s', rebooting...\r\n", ssid);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    /* unreachable */
}

static void cmd_wifiscan(const char *p)
{
    (void)p;
    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        printf("ERROR: scan failed: %s\r\n", esp_err_to_name(err));
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        printf("No APs found\r\nOK\r\n");
        return;
    }

    if (ap_num > 16) ap_num = 16;
    wifi_ap_record_t *aps = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!aps) {
        printf("ERROR: out of memory\r\n");
        return;
    }
    esp_wifi_scan_get_ap_records(&ap_num, aps);

    for (int i = 0; i < ap_num; i++) {
        printf("%2d  RSSI=%-4d  %s\r\n",
               i + 1, aps[i].rssi, (char *)aps[i].ssid);
    }
    free(aps);
    printf("OK\r\n");
}

static void cmd_cifsr(const char *p)
{
    (void)p;
    printf("%s\r\n", wifi_manager_get_ip());
    printf("OK\r\n");
}

static void cmd_camcap(const char *p)
{
    (void)p;
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        printf("ERROR: capture failed\r\n");
        return;
    }
    printf("OK — JPEG %dx%d len=%u\r\n", fb->width, fb->height, (unsigned)fb->len);
    esp_camera_fb_return(fb);
}

static void cmd_camqual(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        printf("Quality: %u\r\n", (unsigned)config_get_cam_quality());
        printf("OK\r\n");
        return;
    }
    int n = atoi(p);
    if (n < 1 || n > 63) {
        printf("ERROR: quality must be 1-63\r\n");
        return;
    }
    config_set_int_and_save("cam_quality", n);
    printf("OK — quality set to %d\r\n", n);
}

static void cmd_camres(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        printf("Frame size index: %u\r\n", (unsigned)config_get_cam_framesize());
        printf("OK\r\n");
        return;
    }
    int n = atoi(p);
    if (n < 0 || n > 13) {
        printf("ERROR: frame size index must be 0-13\r\n");
        return;
    }
    config_set_int_and_save("cam_framesize", n);
    printf("OK — frame size set to %d\r\n", n);
}

static void cmd_aiface(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        printf("Face detect: %s\r\n", ai_is_enabled(AI_FEATURE_FACE_DETECT) ? "on" : "off");
        printf("OK\r\n");
        return;
    }
    bool on;
    if (!parse_onoff(p, &on)) {
        printf("ERROR: use on/off\r\n");
        return;
    }
    ai_enable(AI_FEATURE_FACE_DETECT, on);
    config_set_bool_and_save("ai_face_enable", on);
    printf("OK — face detect %s\r\n", on ? "on" : "off");
}

static void cmd_aimotion(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        printf("Motion detect: %s\r\n", ai_is_enabled(AI_FEATURE_MOTION_DETECT) ? "on" : "off");
        printf("OK\r\n");
        return;
    }
    bool on;
    if (!parse_onoff(p, &on)) {
        printf("ERROR: use on/off\r\n");
        return;
    }
    ai_enable(AI_FEATURE_MOTION_DETECT, on);
    config_set_bool_and_save("ai_motion_enable", on);
    printf("OK — motion detect %s\r\n", on ? "on" : "off");
}

static void cmd_aiqr(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        printf("QR decode: %s\r\n", ai_is_enabled(AI_FEATURE_QR_DECODE) ? "on" : "off");
        printf("OK\r\n");
        return;
    }
    bool on;
    if (!parse_onoff(p, &on)) {
        printf("ERROR: use on/off\r\n");
        return;
    }
    ai_enable(AI_FEATURE_QR_DECODE, on);
    config_set_bool_and_save("ai_qr_enable", on);
    printf("OK — QR decode %s\r\n", on ? "on" : "off");
}

static void cmd_config(const char *p)
{
    (void)p;
    printf("wifi_ssid:     %s\r\n", config_get_wifi_ssid());
    printf("wifi_pass:     %s\r\n", config_get_wifi_pass()[0] ? "********" : "(empty)");
    printf("cam_framesize: %u\r\n", (unsigned)config_get_cam_framesize());
    printf("cam_quality:   %u\r\n", (unsigned)config_get_cam_quality());
    printf("ai_face:       %s\r\n", config_get_ai_face_enable() ? "on" : "off");
    printf("ai_motion:     %s\r\n", config_get_ai_motion_enable() ? "on" : "off");
    printf("ai_qr:         %s\r\n", config_get_ai_qr_enable() ? "on" : "off");
    printf("rtsp_user:     %s\r\n", config_get_rtsp_user());
    printf("rtsp_pass:     %s\r\n", config_get_rtsp_pass()[0] ? "********" : "(empty)");
    printf("OK\r\n");
}

static void cmd_rtspuser(const char *p)
{
    if (!p || !p[0]) {
        printf("ERROR: Usage: AT+RTSPUSER=name\r\n");
        return;
    }
    config_set("rtsp_user", p);
    config_save();
    printf("OK — RTSP user set\r\n");
}

static void cmd_rtsppass(const char *p)
{
    if (!p || !p[0]) {
        printf("ERROR: Usage: AT+RTSPPASS=pass\r\n");
        return;
    }
    config_set("rtsp_pass", p);
    config_save();
    printf("OK — RTSP pass set\r\n");
}

static void cmd_led(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        printf("ERROR: LED state not tracked. Use AT+LED=n (0-100)\r\n");
        return;
    }
    int n = atoi(p);
    if (n < 0 || n > 100) {
        printf("ERROR: brightness must be 0-100\r\n");
        return;
    }
    esp_err_t err = flash_led_set_brightness((uint8_t)n);
    if (err != ESP_OK) {
        printf("ERROR: %s\r\n", esp_err_to_name(err));
        return;
    }
    printf("OK — LED brightness %d%%\r\n", n);
}

/* ------------------------------------------------------------------ */
/*  Parser                                                            */
/* ------------------------------------------------------------------ */

static void process_line(char *line)
{
    /* Strip trailing \r\n */
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') return;

    /* Bare "AT" handshake */
    if (str_eq(line, "AT")) {
        cmd_at(NULL);
        return;
    }

    /* Make a working copy for prefix matching.
     * Strategy: split on '=' or '?' to separate command from params. */
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char *params = NULL;

    char *eq = strchr(buf, '=');
    char *q  = strchr(buf, '?');
    if (eq) {
        *eq = '\0';
        params = eq + 1;
    } else if (q) {
        *q = '\0';
        params = "?";
    }

    /* Match against command table */
    for (int i = 0; i < N_COMMANDS; i++) {
        if (str_eq(buf, s_commands[i].prefix)) {
            s_commands[i].handler(params);
            return;
        }
    }

    printf("ERROR: unknown command. Type AT+HELP\r\n");
}

/* ------------------------------------------------------------------ */
/*  Task + Init                                                       */
/* ------------------------------------------------------------------ */

static void at_task(void *arg)
{
    (void)arg;

    /* Let system settle before grabbing UART */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Install UART driver if not already (console may have it) */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, 1024, 1024, 0, NULL, 0);
    }
    /* Connect VFS so fgets()/printf() work through the UART driver */
    uart_vfs_dev_use_driver(UART_NUM_0);

    ESP_LOGI(TAG, "AT command listener ready on UART0 (115200)");

    char line[256];
    while (1) {
        if (fgets(line, sizeof(line), stdin)) {
            process_line(line);
        }
    }
}

esp_err_t at_command_init(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        at_task,
        "at_cmd",
        4096,
        NULL,
        2,            /* low priority — config/UI task */
        NULL,
        0);           /* Core 0 */

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AT command task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
