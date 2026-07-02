/*
 * MiBee Cam v0.1 — ONVIF WS-Discovery + mDNS + SOAP service entry
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * High-level entry point for all ONVIF functionality. Call onvif_start()
 * after the RTSP server is running. It checks config_get_onvif_enable(),
 * initialises mDNS, registers SOAP handlers on the existing web server,
 * and starts the WS-Discovery UDP multicast listener.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start ONVIF services (mDNS + WS-Discovery + SOAP handlers).
 *
 * Checks config_get_onvif_enable() first. If disabled, returns ESP_OK
 * immediately without starting anything.
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t onvif_start(void);

/**
 * @brief Stop ONVIF services and release resources.
 */
void onvif_stop(void);

#ifdef __cplusplus
}
#endif
