/*
 * MiBee Cam v0.1 — ONVIF SOAP service handlers
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Registers POST handlers for /onvif/device_service and /onvif/media_service
 * on the existing HTTP web server. All SOAP XML responses are hand-crafted
 * via snprintf (no XML library dependency).
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register ONVIF SOAP URI handlers on an HTTP server instance.
 *
 * Registers:
 *   POST /onvif/device_service — GetSystemDateAndTime, GetDeviceInformation, GetCapabilities
 *   POST /onvif/media_service  — GetProfiles, GetStreamUri, GetSnapshot
 *
 * @param server  httpd_handle_t from web_server_get_handle()
 * @return ESP_OK on success
 */
esp_err_t onvif_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
