/*
 * TLED - Matter-over-Thread LED Controller
 * NVS Configuration Management Implementation (PWM CCT variant)
 */

#include "app_nvs_config.h"
#include <string.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "tled_config";

#define NVS_NAMESPACE "tled_cfg"
#define NVS_KEY_CONFIG "config"

// Valid GPIO pins for ESP32-C6 PWM output
// Avoiding: 9 (boot button), 12-13 (USB), 15 (onboard LED)
static const uint8_t valid_gpio_pins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 14, 18, 19, 20, 21, 22, 23};
#define NUM_VALID_GPIOS (sizeof(valid_gpio_pins) / sizeof(valid_gpio_pins[0]))

static tled_config_t s_config;
static bool s_initialized = false;
static SemaphoreHandle_t s_config_mutex = NULL;

static void set_defaults(tled_config_t *config)
{
    config->ww_gpio = TLED_DEFAULT_WW_GPIO;
    config->cw_gpio = TLED_DEFAULT_CW_GPIO;
    config->pwm_freq_hz = TLED_DEFAULT_PWM_FREQ_HZ;
    config->color_temp_min = TLED_DEFAULT_COLOR_TEMP_MIN;
    config->color_temp_max = TLED_DEFAULT_COLOR_TEMP_MAX;
    config->max_brightness = TLED_DEFAULT_MAX_BRIGHTNESS;
    config->power_on_behavior = TLED_DEFAULT_POWER_ON;
    strncpy(config->device_name, TLED_DEFAULT_DEVICE_NAME, sizeof(config->device_name) - 1);
    config->device_name[sizeof(config->device_name) - 1] = '\0';
    config->config_version = TLED_CONFIG_VERSION;
    config->configured = false;
}

static bool validate_config(const tled_config_t *config)
{
    if (!tled_config_validate_gpio(config->ww_gpio)) {
        ESP_LOGW(TAG, "Invalid WW GPIO: %d", config->ww_gpio);
        return false;
    }
    if (!tled_config_validate_gpio(config->cw_gpio)) {
        ESP_LOGW(TAG, "Invalid CW GPIO: %d", config->cw_gpio);
        return false;
    }
    if (config->pwm_freq_hz == 0 || config->pwm_freq_hz > 40000) {
        ESP_LOGW(TAG, "Invalid PWM freq: %lu", (unsigned long)config->pwm_freq_hz);
        return false;
    }
    if (config->color_temp_min >= config->color_temp_max) {
        ESP_LOGW(TAG, "Invalid CCT range: %d-%d", config->color_temp_min, config->color_temp_max);
        return false;
    }
    if (config->power_on_behavior > POWER_ON_OFF) {
        ESP_LOGW(TAG, "Invalid power-on behavior: %d", config->power_on_behavior);
        return false;
    }
    if (config->config_version != TLED_CONFIG_VERSION) {
        ESP_LOGW(TAG, "Config version mismatch: %d (expected %d)",
                 config->config_version, TLED_CONFIG_VERSION);
        return false;
    }
    return true;
}

esp_err_t tled_config_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (s_config_mutex == NULL) {
        s_config_mutex = xSemaphoreCreateMutex();
        if (s_config_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create config mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    set_defaults(&s_config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(handle, NVS_KEY_CONFIG, NULL, &size);

        if (err == ESP_OK && size == sizeof(tled_config_t)) {
            err = nvs_get_blob(handle, NVS_KEY_CONFIG, &s_config, &size);
            if (err == ESP_OK && validate_config(&s_config)) {
                nvs_close(handle);
                ESP_LOGI(TAG, "Config loaded: WW=GPIO%d, CW=GPIO%d, freq=%luHz, CCT=%d-%d mireds",
                         s_config.ww_gpio, s_config.cw_gpio, (unsigned long)s_config.pwm_freq_hz,
                         s_config.color_temp_min, s_config.color_temp_max);
                s_initialized = true;
                return ESP_OK;
            }
        }
        // Any failure: fall back to defaults
        ESP_LOGW(TAG, "Config load failed or version changed, using defaults");
        set_defaults(&s_config);
        nvs_close(handle);
    } else {
        ESP_LOGI(TAG, "No config in NVS (first boot), using defaults");
    }

    s_initialized = true;
    return ESP_OK;
}

const tled_config_t* tled_config_get(void)
{
    if (!s_initialized) {
        tled_config_init();
    }
    return &s_config;
}

tled_config_t* tled_config_get_mutable(void)
{
    if (!s_initialized) {
        tled_config_init();
    }
    return &s_config;
}

bool tled_config_is_configured(void)
{
    if (!s_initialized) {
        tled_config_init();
    }
    return s_config.configured;
}

esp_err_t tled_config_save(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_CONFIG, &s_config, sizeof(tled_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config blob: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    return err;
}

void tled_config_reset_to_defaults(void)
{
    if (s_config_mutex) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    }
    set_defaults(&s_config);
    if (s_config_mutex) {
        xSemaphoreGive(s_config_mutex);
    }
    ESP_LOGI(TAG, "Config reset to defaults");
}

bool tled_config_validate_gpio(uint8_t gpio_pin)
{
    for (size_t i = 0; i < NUM_VALID_GPIOS; i++) {
        if (valid_gpio_pins[i] == gpio_pin) {
            return true;
        }
    }
    return false;
}
