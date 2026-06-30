/*
 * TLED - Matter-over-Thread LED Controller
 * NVS Configuration Management (PWM CCT variant)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

// Power-on behavior options
typedef enum {
    POWER_ON_RESTORE = 0,   // Restore last state (default)
    POWER_ON_ON = 1,        // Always turn on
    POWER_ON_OFF = 2,       // Always stay off
} tled_power_on_t;

// Configuration structure
typedef struct {
    uint8_t ww_gpio;            // Warm white PWM GPIO pin
    uint8_t cw_gpio;            // Cool white PWM GPIO pin
    uint32_t pwm_freq_hz;       // PWM frequency in Hz
    uint16_t color_temp_min;    // Min color temp in mireds (coolest)
    uint16_t color_temp_max;    // Max color temp in mireds (warmest)
    uint8_t max_brightness;     // Max brightness limit (0-255)
    uint8_t power_on_behavior;  // Power-on behavior (tled_power_on_t)
    char device_name[32];       // Custom device name
    uint8_t config_version;     // Config version for migration
    bool configured;            // True if config has been explicitly set
} tled_config_t;

#include <sdkconfig.h>

#define TLED_DEFAULT_WW_GPIO        CONFIG_TLED_WW_GPIO_PIN
#define TLED_DEFAULT_CW_GPIO        CONFIG_TLED_CW_GPIO_PIN
#define TLED_DEFAULT_PWM_FREQ_HZ    CONFIG_TLED_PWM_FREQ_HZ
#define TLED_DEFAULT_COLOR_TEMP_MIN CONFIG_TLED_COLOR_TEMP_MIN
#define TLED_DEFAULT_COLOR_TEMP_MAX CONFIG_TLED_COLOR_TEMP_MAX
#define TLED_DEFAULT_MAX_BRIGHTNESS CONFIG_TLED_MAX_BRIGHTNESS
#define TLED_DEFAULT_DEVICE_NAME    "TLED"
#define TLED_DEFAULT_POWER_ON       POWER_ON_RESTORE
#define TLED_CONFIG_VERSION         4  // Bumped for PWM CCT variant

esp_err_t tled_config_init(void);
const tled_config_t* tled_config_get(void);
tled_config_t* tled_config_get_mutable(void);
bool tled_config_is_configured(void);
esp_err_t tled_config_save(void);
void tled_config_reset_to_defaults(void);
bool tled_config_validate_gpio(uint8_t gpio_pin);

#define tled_config_reset() tled_config_reset_to_defaults()
