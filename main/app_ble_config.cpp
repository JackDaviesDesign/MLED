/*
 * TLED - Matter-over-Thread LED Controller
 * BLE Configuration Service - stub (not yet implemented for PWM CCT variant)
 */

#include "app_ble_config.h"
#include <esp_log.h>

static const char *TAG = "tled_ble_cfg";

esp_err_t tled_ble_config_start(uint32_t timeout_ms)
{
    ESP_LOGW(TAG, "BLE config mode not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

void tled_ble_config_stop(void)
{
    // Not implemented
}

bool tled_ble_config_is_active(void)
{
    return false;
}

bool tled_ble_config_was_saved(void)
{
    return false;
}
