/*
 * MiBee Cam v0.1 — AT Command Interface
 *
 * Line-based AT command listener on UART0 (shared with console).
 * Reference: ai-thinker-esp32-cam serial_config pattern, extended for
 * WiFi / camera / AI / config / LED / system control.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the AT command listener on UART0.
 *
 * Spawns a FreeRTOS task (Core 0, priority 2) that reads serial lines
 * via fgets() and dispatches AT commands.  Call once after all
 * subsystems are initialized.
 *
 * @return ESP_OK on success.
 */
esp_err_t at_command_init(void);

#ifdef __cplusplus
}
#endif
