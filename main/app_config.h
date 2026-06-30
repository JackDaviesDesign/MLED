/*
 * TLED - Matter-over-Thread LED Controller
 * Configuration constants (PWM CCT variant)
 */

#pragma once

#include <sdkconfig.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

// Hardware Configuration - DFRobot Beetle ESP32-C6
#define TLED_ONBOARD_LED_GPIO   15      // Onboard LED (unused)
#define TLED_BOOT_BUTTON_GPIO   9       // Boot button / factory reset

// PWM resolution (13-bit = 8192 levels)
#define TLED_PWM_RESOLUTION_BITS    13
#define TLED_PWM_MAX_DUTY           ((1 << TLED_PWM_RESOLUTION_BITS) - 1)

// Matter attribute ranges
#define MATTER_BRIGHTNESS_MAX   254
#define MATTER_COLOR_TEMP_MIN   CONFIG_TLED_COLOR_TEMP_MIN   // mireds
#define MATTER_COLOR_TEMP_MAX   CONFIG_TLED_COLOR_TEMP_MAX   // mireds

// Matter default values
#define TLED_DEFAULT_POWER          false
#define TLED_DEFAULT_BRIGHTNESS     127     // ~50% (0-254)
#define TLED_DEFAULT_COLOR_TEMP     ((CONFIG_TLED_COLOR_TEMP_MIN + CONFIG_TLED_COLOR_TEMP_MAX) / 2)

// Transition settings
#define TLED_TRANSITION_TASK_STACK  4096
#define TLED_TRANSITION_TASK_PRIO   5
#define TLED_TRANSITION_TICK_MS     20      // 50 FPS
#define TLED_DEFAULT_TRANSITION_MS  CONFIG_TLED_DEFAULT_TRANSITION_MS

// Device identification
#define TLED_DEVICE_NAME        "TLED"
#define TLED_VENDOR_NAME        "TLED Project"

// OpenThread configuration for ESP32-C6 native radio
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif
