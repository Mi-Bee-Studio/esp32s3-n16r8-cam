/*
 * MiBee Cam v0.1 — MJPEG streamer over ESP HTTP Server
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serves GET /stream as multipart/x-mixed-replace via the esp_http_server.
 * Each client gets its own FreeRTOS task via the async request API.
 * Maximum concurrent clients: 2.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MJPEG streamer internals (mutex, client counter).
 * @return ESP_OK or ESP_ERR_NO_MEM
 */
esp_err_t mjpeg_stream_init(void);

/**
 * @brief Handler for GET /stream — register with httpd URI table.
 *
 * Creates an async request copy and spawns a FreeRTOS task that
 * streams JPEG frames via multipart/x-mixed-replace.
 * Handler returns immediately — does NOT block the httpd worker.
 */
esp_err_t mjpeg_stream_handler(httpd_req_t *req);

/**
 * @brief Return current number of active MJPEG stream clients.
 */
int mjpeg_stream_client_count(void);

#ifdef __cplusplus
}
#endif
