/*
 * MiBee Cam v0.1 — MJPEG streamer
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serves MJPEG frames via esp_http_server's async request API.
 * Each GET /stream creates a FreeRTOS task that:
 *   1. Sends HTTP 200 + multipart/x-mixed-replace headers
 *   2. Subscribes to frame_broadcaster (FRAMESUB_MJPEG)
 *   3. Loops: get_frame -> httpd_socket_send (part header + JPEG + CRLF)
 *   4. Cleanup: unsubscribe, send closing boundary, complete async req
 *
 * Max 2 concurrent clients — 3rd gets 503.
 * Frame access is non-blocking via frame_broadcaster_get_frame().
 *
 * Design: seeed-esp32s3-cam uses a separate raw TCP server on port 81.
 *   This module integrates with esp_http_server instead, using
 *   httpd_req_async_handler_begin() to run each stream as a dedicated
 *   FreeRTOS task without blocking the httpd worker.
 */

#include "mjpeg_streamer.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "frame_broadcaster.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "mjpeg";

#define MJPEG_BOUNDARY      "frame"
#define MAX_STREAM_CLIENTS  2
#define STREAM_TASK_STACK   4096

static int s_client_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* Forward declarations */
static void mjpeg_stream_task(void *arg);

/* ------------------------------------------------------------------ */
/*  Per-client async stream task                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    httpd_handle_t hd;
    int            sock_fd;
    httpd_req_t   *req;   /* async copy — call complete() when done */
} mjpeg_ctx_t;

static void mjpeg_stream_task(void *arg)
{
    mjpeg_ctx_t *ctx = (mjpeg_ctx_t *)arg;

    /* ---- Send HTTP 200 + multipart/x-mixed-replace headers ------ */
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (httpd_socket_send(ctx->hd, ctx->sock_fd, headers, strlen(headers), 0) <= 0) {
        ESP_LOGW(TAG, "Failed to send stream headers");
        goto cleanup;
    }

    /* ---- Subscribe to frame broadcaster ------------------------- */
    frame_sub_t *sub = frame_broadcaster_subscribe(FRAMESUB_MJPEG);
    if (!sub) {
        ESP_LOGE(TAG, "Failed to subscribe to frame broadcaster");
        const char *err_resp = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Length: 18\r\n\r\nStream unavailable\r\n";
        httpd_socket_send(ctx->hd, ctx->sock_fd, err_resp, strlen(err_resp), 0);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Stream client started (total %d)", s_client_count);

    /* ---- Stream loop -------------------------------------------- */
    char part_hdr[192];
    int capture_fails = 0;

    while (1) {
        frame_msg_t msg;
        if (frame_broadcaster_get_frame(sub, &msg)) {
            capture_fails = 0;

            /* Build multipart part header */
            int hdrlen = snprintf(part_hdr, sizeof(part_hdr),
                "\r\n--" MJPEG_BOUNDARY "\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "\r\n", msg.len);

            /* Send part header */
            if (httpd_socket_send(ctx->hd, ctx->sock_fd, part_hdr, hdrlen, 0) != hdrlen) {
                frame_broadcaster_release(&msg);
                break;
            }

            /* Send JPEG body in one shot (PSRAM-backed, usually < 100 KB) */
            size_t remaining = msg.len;
            const uint8_t *ptr = msg.data;
            while (remaining > 0) {
                int sent = httpd_socket_send(ctx->hd, ctx->sock_fd,
                                             (const char *)ptr, remaining, 0);
                if (sent <= 0) {
                    frame_broadcaster_release(&msg);
                    goto stream_done;
                }
                ptr += sent;
                remaining -= sent;
            }

            /* Trailing CRLF */
            if (httpd_socket_send(ctx->hd, ctx->sock_fd, "\r\n", 2, 0) != 2) {
                frame_broadcaster_release(&msg);
                break;
            }

            frame_broadcaster_release(&msg);
        } else {
            capture_fails++;
            if (capture_fails >= 10) {
                ESP_LOGW(TAG, "No frames after %d attempts, ending stream", capture_fails);
                break;
            }
        }

        /* Frame-rate throttle — ~33 fps max */
        vTaskDelay(pdMS_TO_TICKS(30));
    }

stream_done:
    frame_broadcaster_unsubscribe(sub);

    /* Send closing boundary (best-effort, ignore errors) */
    httpd_socket_send(ctx->hd, ctx->sock_fd,
                      "\r\n--" MJPEG_BOUNDARY "--\r\n",
                      6 + strlen(MJPEG_BOUNDARY), 0);

cleanup:
    /* Decrement client count */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_client_count--;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stream client disconnected (total %d)", s_client_count);

    /* Complete async request — httpd takes back socket ownership */
    httpd_req_async_handler_complete(ctx->req);
    free(ctx);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  HTTP handler — runs in httpd worker, spawns async task             */
/* ------------------------------------------------------------------ */

esp_err_t mjpeg_stream_handler(httpd_req_t *req)
{
    /* Enforce client limit */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_count >= MAX_STREAM_CLIENTS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Max stream clients (%d) reached, rejecting with 503", MAX_STREAM_CLIENTS);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Max stream connections reached", 29);
        return ESP_OK;
    }
    s_client_count++;
    xSemaphoreGive(s_mutex);

    /* Create async request copy so the handler can return immediately */
    httpd_req_t *async_req = NULL;
    esp_err_t ret = httpd_req_async_handler_begin(req, &async_req);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_req_async_handler_begin failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Async init failed", 17);
        return ESP_OK;
    }

    /* Allocate context for the streaming task */
    mjpeg_ctx_t *ctx = calloc(1, sizeof(mjpeg_ctx_t));
    if (!ctx) {
        httpd_req_async_handler_complete(async_req);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "No memory", 9);
        return ESP_OK;
    }

    ctx->hd      = async_req->handle;
    ctx->sock_fd = httpd_req_to_sockfd(async_req);
    ctx->req     = async_req;

    /* Spawn dedicated FreeRTOS task for this stream client */
    BaseType_t created = xTaskCreate(
        mjpeg_stream_task,
        "mjpeg_cli",
        STREAM_TASK_STACK,
        ctx,
        2,      /* priority */
        NULL);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task");
        free(ctx);
        httpd_req_async_handler_complete(async_req);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Task creation failed", 19);
        return ESP_OK;
    }

    /* Handler returns immediately — the streaming task owns the session */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t mjpeg_stream_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_client_count = 0;
    ESP_LOGI(TAG, "MJPEG streamer initialized (max %d clients)", MAX_STREAM_CLIENTS);
    return ESP_OK;
}

int mjpeg_stream_client_count(void)
{
    int count = 0;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        count = s_client_count;
        xSemaphoreGive(s_mutex);
    }
    return count;
}
