/*
 * TLED - Matter-over-Thread LED Controller
 * PWM CCT Driver (dual 0-10V channel: warm white + cool white)
 */

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <math.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>
#include "app_driver.h"
#include "app_config.h"
#include "app_nvs_config.h"

#include <iot_button.h>
#include <button_gpio.h>

#define NVS_NAMESPACE       "tled_state"
#define NVS_KEY_POWER       "power"
#define NVS_KEY_BRIGHTNESS  "brightness"
#define NVS_KEY_COLOR_TEMP  "color_temp"

#define NVS_SAVE_DEBOUNCE_MS    5000
#define CCT_UPDATE_DEBOUNCE_MS  50

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "tled_driver";

extern uint16_t light_endpoint_id;

// Transition state for smooth fades
typedef struct {
    float start_bri;
    float start_cct;    // mireds

    float target_bri;
    float target_cct;   // mireds

    float current_bri;
    float current_cct;  // mireds

    TickType_t transition_start_ticks;
    uint32_t transition_duration_ms;
    bool transitioning;
} transition_state_t;

typedef struct {
    bool power;
    uint8_t brightness;         // 0-254 Matter range
    uint16_t color_temp;        // mireds

    // Hardware config
    uint8_t ww_gpio;
    uint8_t cw_gpio;
    uint16_t color_temp_min;    // coolest (smallest mireds value)
    uint16_t color_temp_max;    // warmest (largest mireds value)
    uint8_t max_brightness;     // 0-255 limit

    transition_state_t transition;
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;

    bool cct_update_pending;
    TickType_t cct_update_time;

    bool nvs_save_pending;
    TimerHandle_t nvs_save_timer;
} light_driver_t;

static light_driver_t s_driver = {
    .power = false,
    .brightness = TLED_DEFAULT_BRIGHTNESS,
    .color_temp = TLED_DEFAULT_COLOR_TEMP,
    .ww_gpio = TLED_DEFAULT_WW_GPIO,
    .cw_gpio = TLED_DEFAULT_CW_GPIO,
    .color_temp_min = TLED_DEFAULT_COLOR_TEMP_MIN,
    .color_temp_max = TLED_DEFAULT_COLOR_TEMP_MAX,
    .max_brightness = TLED_DEFAULT_MAX_BRIGHTNESS,
    .transition = {0},
    .task_handle = NULL,
    .mutex = NULL,
    .cct_update_pending = false,
    .cct_update_time = 0,
    .nvs_save_pending = false,
    .nvs_save_timer = NULL,
};

// ── PWM output ────────────────────────────────────────────────────────────────

static void pwm_set_channels(light_driver_t *driver, float bri_normalized, float cct_mireds)
{
    if (bri_normalized < 0.0f) bri_normalized = 0.0f;
    if (bri_normalized > 1.0f) bri_normalized = 1.0f;

    float max_scale = (float)driver->max_brightness / 255.0f;
    float effective_bri = bri_normalized * max_scale;

#if CONFIG_MLED_SINGLE_CHANNEL
    uint32_t ww_duty = (uint32_t)(effective_bri * TLED_PWM_MAX_DUTY);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ww_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "PWM single: bri=%.2f duty=%lu", effective_bri, (unsigned long)ww_duty);
#else
    float cct_range = (float)(driver->color_temp_max - driver->color_temp_min);
    float ratio = (cct_range > 0.0f)
        ? (cct_mireds - (float)driver->color_temp_min) / cct_range
        : 0.5f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    uint32_t ww_duty = (uint32_t)(ratio * effective_bri * TLED_PWM_MAX_DUTY);
    uint32_t cw_duty = (uint32_t)((1.0f - ratio) * effective_bri * TLED_PWM_MAX_DUTY);

    esp_err_t r0 = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ww_duty);
    esp_err_t r1 = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    esp_err_t r2 = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, cw_duty);
    esp_err_t r3 = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);

    ESP_LOGI(TAG, "PWM: bri=%.2f cct=%.0f ww=%lu cw=%lu err=%d/%d/%d/%d",
             effective_bri, cct_mireds, (unsigned long)ww_duty, (unsigned long)cw_duty,
             r0, r1, r2, r3);
#endif
}

static void apply_current_state(light_driver_t *driver)
{
    if (!driver->power) {
        pwm_set_channels(driver, 0.0f, driver->color_temp);
        return;
    }
    float bri = (float)driver->transition.current_bri / (float)MATTER_BRIGHTNESS_MAX;
    pwm_set_channels(driver, bri, driver->transition.current_cct);
}

// ── NVS persistence ───────────────────────────────────────────────────────────

static esp_err_t do_save_state_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(handle, NVS_KEY_POWER, s_driver.power ? 1 : 0);
    nvs_set_u8(handle, NVS_KEY_BRIGHTNESS, s_driver.brightness);
    nvs_set_u16(handle, NVS_KEY_COLOR_TEMP, s_driver.color_temp);

    err = nvs_commit(handle);
    nvs_close(handle);
    s_driver.nvs_save_pending = false;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "State saved: power=%d bri=%d cct=%d mireds",
                 s_driver.power, s_driver.brightness, s_driver.color_temp);
    }
    return err;
}

static void nvs_save_timer_callback(TimerHandle_t timer)
{
    do_save_state_to_nvs();
}

static void schedule_save_state_to_nvs(void)
{
    s_driver.nvs_save_pending = true;
    if (s_driver.nvs_save_timer != NULL) {
        if (xTimerReset(s_driver.nvs_save_timer, 0) != pdPASS) {
            do_save_state_to_nvs();
        }
    } else {
        do_save_state_to_nvs();
    }
}

static esp_err_t load_state_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved state, using defaults");
        return err;
    }

    uint8_t val8;
    uint16_t val16;
    if (nvs_get_u8(handle, NVS_KEY_POWER, &val8) == ESP_OK)
        s_driver.power = (val8 != 0);
    if (nvs_get_u8(handle, NVS_KEY_BRIGHTNESS, &val8) == ESP_OK)
        s_driver.brightness = val8;
    if (nvs_get_u16(handle, NVS_KEY_COLOR_TEMP, &val16) == ESP_OK)
        s_driver.color_temp = val16;

    nvs_close(handle);
    ESP_LOGI(TAG, "State loaded: power=%d bri=%d cct=%d mireds",
             s_driver.power, s_driver.brightness, s_driver.color_temp);
    return ESP_OK;
}

// ── Transition engine ─────────────────────────────────────────────────────────

static float lerp(float from, float to, float t)
{
    return from + (to - from) * t;
}

static void start_transition(light_driver_t *driver, uint8_t target_bri,
                              uint16_t target_cct, uint32_t duration_ms)
{
    xSemaphoreTake(driver->mutex, portMAX_DELAY);

    driver->transition.start_bri = driver->transition.current_bri;
    driver->transition.start_cct = driver->transition.current_cct;
    driver->transition.target_bri = (float)target_bri;
    driver->transition.target_cct = (float)target_cct;
    driver->transition.transition_start_ticks = xTaskGetTickCount();
    driver->transition.transition_duration_ms = duration_ms > 0 ? duration_ms : 1;
    driver->transition.transitioning = true;

    ESP_LOGI(TAG, "Transition: bri %.0f->%d cct %.0f->%d over %lums",
             driver->transition.start_bri, target_bri,
             driver->transition.start_cct, target_cct,
             (unsigned long)duration_ms);

    xSemaphoreGive(driver->mutex);
}

static void transition_task(void *arg)
{
    light_driver_t *driver = (light_driver_t *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        xSemaphoreTake(driver->mutex, portMAX_DELAY);

        // Handle debounced CCT update
        if (driver->cct_update_pending) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms = (uint32_t)(now - driver->cct_update_time) * portTICK_PERIOD_MS;
            if (elapsed_ms >= CCT_UPDATE_DEBOUNCE_MS) {
                driver->cct_update_pending = false;
                uint16_t cct = driver->color_temp;
                uint8_t bri = driver->brightness;
                xSemaphoreGive(driver->mutex);

                start_transition(driver, bri, cct, TLED_DEFAULT_TRANSITION_MS);
                xSemaphoreTake(driver->mutex, portMAX_DELAY);
            }
        }

        if (!driver->power) {
            if (driver->transition.transitioning && driver->transition.target_bri == 0.0f) {
                // Let fade-out complete
            } else {
                // Off and not fading out
                driver->transition.current_bri = 0.0f;
                driver->transition.transitioning = false;
                pwm_set_channels(driver, 0.0f, driver->color_temp);
                xSemaphoreGive(driver->mutex);
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
                continue;
            }
        }

        if (driver->transition.transitioning) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed = (uint32_t)(now - driver->transition.transition_start_ticks) * portTICK_PERIOD_MS;

            if (elapsed >= driver->transition.transition_duration_ms) {
                driver->transition.current_bri = driver->transition.target_bri;
                driver->transition.current_cct = driver->transition.target_cct;
                driver->transition.transitioning = false;
                ESP_LOGI(TAG, "Transition complete");
            } else {
                float t = (float)elapsed / (float)driver->transition.transition_duration_ms;
                driver->transition.current_bri = lerp(driver->transition.start_bri, driver->transition.target_bri, t);
                driver->transition.current_cct = lerp(driver->transition.start_cct, driver->transition.target_cct, t);
            }

            apply_current_state(driver);
        }

        xSemaphoreGive(driver->mutex);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TLED_TRANSITION_TICK_MS));
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t app_driver_light_set_power(app_driver_handle_t handle, bool power)
{
    light_driver_t *driver = (light_driver_t *)handle;
    if (!driver) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Power -> %s", power ? "ON" : "OFF");

    if (power) {
        driver->power = true;
        driver->transition.current_bri = 0.0f;
        start_transition(driver, driver->brightness, driver->color_temp, TLED_DEFAULT_TRANSITION_MS);
    } else {
        start_transition(driver, 0, driver->color_temp, TLED_DEFAULT_TRANSITION_MS);
        driver->power = false;
    }

    schedule_save_state_to_nvs();
    return ESP_OK;
}

bool app_driver_light_get_power(app_driver_handle_t handle)
{
    light_driver_t *driver = (light_driver_t *)handle;
    if (!driver) return false;
    xSemaphoreTake(driver->mutex, portMAX_DELAY);
    bool p = driver->power;
    xSemaphoreGive(driver->mutex);
    return p;
}

esp_err_t app_driver_light_set_brightness(app_driver_handle_t handle, uint8_t brightness)
{
    return app_driver_light_set_brightness_with_transition(handle, brightness, TLED_DEFAULT_TRANSITION_MS);
}

esp_err_t app_driver_light_set_brightness_with_transition(app_driver_handle_t handle,
                                                           uint8_t brightness,
                                                           uint32_t transition_ms)
{
    light_driver_t *driver = (light_driver_t *)handle;
    if (!driver) return ESP_ERR_INVALID_ARG;

    driver->brightness = brightness;
    ESP_LOGI(TAG, "Brightness -> %d (trans %lums)", brightness, (unsigned long)transition_ms);

    schedule_save_state_to_nvs();

    if (transition_ms == 0) {
        xSemaphoreTake(driver->mutex, portMAX_DELAY);
        driver->transition.current_bri = brightness;
        driver->transition.transitioning = false;
        apply_current_state(driver);
        xSemaphoreGive(driver->mutex);
        return ESP_OK;
    }

    start_transition(driver, brightness, driver->color_temp, transition_ms);
    return ESP_OK;
}

esp_err_t app_driver_light_set_color_temp(app_driver_handle_t handle, uint16_t color_temp_mireds)
{
    return app_driver_light_set_color_temp_with_transition(handle, color_temp_mireds, TLED_DEFAULT_TRANSITION_MS);
}

esp_err_t app_driver_light_set_color_temp_with_transition(app_driver_handle_t handle,
                                                           uint16_t color_temp_mireds,
                                                           uint32_t transition_ms)
{
    light_driver_t *driver = (light_driver_t *)handle;
    if (!driver) return ESP_ERR_INVALID_ARG;

    driver->color_temp = color_temp_mireds;
    ESP_LOGI(TAG, "CCT -> %d mireds (trans %lums)", color_temp_mireds, (unsigned long)transition_ms);

    schedule_save_state_to_nvs();

    if (transition_ms == 0) {
        xSemaphoreTake(driver->mutex, portMAX_DELAY);
        driver->transition.current_cct = color_temp_mireds;
        driver->transition.transitioning = false;
        apply_current_state(driver);
        xSemaphoreGive(driver->mutex);
        return ESP_OK;
    }

    start_transition(driver, driver->brightness, color_temp_mireds, transition_ms);
    return ESP_OK;
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle,
                                       uint16_t endpoint_id,
                                       uint32_t cluster_id,
                                       uint32_t attribute_id,
                                       esp_matter_attr_val_t *val)
{
    light_driver_t *driver = (light_driver_t *)driver_handle;
    if (!driver || !driver->mutex) return ESP_OK;
    if (endpoint_id != light_endpoint_id) return ESP_OK;

    if (cluster_id == OnOff::Id) {
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            return app_driver_light_set_power(driver_handle, val->val.b);
        }
    } else if (cluster_id == LevelControl::Id) {
        if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
            return app_driver_light_set_brightness(driver_handle, val->val.u8);
        }
#if !CONFIG_MLED_SINGLE_CHANNEL
    } else if (cluster_id == ColorControl::Id) {
        if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
            // Debounce rapid CCT updates from HA sliders
            xSemaphoreTake(driver->mutex, portMAX_DELAY);
            driver->color_temp = val->val.u16;
            driver->cct_update_pending = true;
            driver->cct_update_time = xTaskGetTickCount();
            xSemaphoreGive(driver->mutex);
            schedule_save_state_to_nvs();
        }
    }
#else
    }
#endif

    return ESP_OK;
}

esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id)
{
    void *priv_data = endpoint::get_priv_data(endpoint_id);
    if (!priv_data) {
        ESP_LOGE(TAG, "No priv_data for endpoint %d", endpoint_id);
        return ESP_ERR_INVALID_STATE;
    }
    light_driver_t *driver = (light_driver_t *)priv_data;
    const tled_config_t *config = tled_config_get();

    bool restored = (load_state_from_nvs() == ESP_OK);

    // Apply power-on behavior override
    if (restored) {
        switch (config->power_on_behavior) {
            case POWER_ON_ON:
                driver->power = true;
                break;
            case POWER_ON_OFF:
                driver->power = false;
                break;
            default:
                break;
        }
    }

    // Clamp restored color_temp to configured range
    if (driver->color_temp < driver->color_temp_min)
        driver->color_temp = driver->color_temp_min;
    if (driver->color_temp > driver->color_temp_max)
        driver->color_temp = driver->color_temp_max;

    // Push values to Matter attributes
    esp_matter_attr_val_t val;

    val = esp_matter_bool(driver->power);
    attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);

    val = esp_matter_uint8(driver->brightness);
    attribute::update(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &val);

#if !CONFIG_MLED_SINGLE_CHANNEL
    val = esp_matter_uint16(driver->color_temp);
    attribute::update(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id, &val);
#endif

    // Sync transition state
    driver->transition.current_bri = driver->power ? (float)driver->brightness : 0.0f;
    driver->transition.current_cct = (float)driver->color_temp;
    driver->transition.start_bri = driver->transition.current_bri;
    driver->transition.start_cct = driver->transition.current_cct;
    driver->transition.transitioning = false;

    // Apply to hardware immediately
    apply_current_state(driver);

    ESP_LOGI(TAG, "Defaults applied: power=%d bri=%d cct=%d mireds",
             driver->power, driver->brightness, driver->color_temp);
    return ESP_OK;
}

// ── Button ────────────────────────────────────────────────────────────────────

static void app_driver_button_toggle_cb(void *button_handle, void *usr_data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        attribute_t *attr = attribute::get(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
        if (!attr) return;
        esp_matter_attr_val_t val = esp_matter_invalid(NULL);
        attribute::get_val(attr, &val);
        val.val.b = !val.val.b;
        attribute::update(light_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
    });
}

// ── Init ──────────────────────────────────────────────────────────────────────

app_driver_handle_t app_driver_light_init(void)
{
    const tled_config_t *config = tled_config_get();

    s_driver.ww_gpio = config->ww_gpio;
    s_driver.cw_gpio = config->cw_gpio;
    s_driver.color_temp_min = config->color_temp_min;
    s_driver.color_temp_max = config->color_temp_max;
    s_driver.max_brightness = config->max_brightness;

    ESP_LOGI(TAG, "PWM CCT init: WW=GPIO%d CW=GPIO%d freq=%luHz CCT=%d-%d mireds max_bri=%d",
             s_driver.ww_gpio, s_driver.cw_gpio, (unsigned long)config->pwm_freq_hz,
             s_driver.color_temp_min, s_driver.color_temp_max, s_driver.max_brightness);

    s_driver.mutex = xSemaphoreCreateMutex();
    if (!s_driver.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return NULL;
    }

    // LEDC timer
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)TLED_PWM_RESOLUTION_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = config->pwm_freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    // Warm white channel
    ledc_channel_config_t ww_cfg = {};
    ww_cfg.gpio_num   = s_driver.ww_gpio;
    ww_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ww_cfg.channel    = LEDC_CHANNEL_0;
    ww_cfg.intr_type  = LEDC_INTR_DISABLE;
    ww_cfg.timer_sel  = LEDC_TIMER_0;
    ww_cfg.duty       = 0;
    ww_cfg.hpoint     = 0;
    ESP_LOGI(TAG, "LEDC WW: gpio=%d channel=0", s_driver.ww_gpio);
    ESP_ERROR_CHECK(ledc_channel_config(&ww_cfg));

#if !CONFIG_MLED_SINGLE_CHANNEL
    // Cool white channel
    ledc_channel_config_t cw_cfg = {};
    cw_cfg.gpio_num   = s_driver.cw_gpio;
    cw_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    cw_cfg.channel    = LEDC_CHANNEL_1;
    cw_cfg.intr_type  = LEDC_INTR_DISABLE;
    cw_cfg.timer_sel  = LEDC_TIMER_0;
    cw_cfg.duty       = 0;
    cw_cfg.hpoint     = 0;
    ESP_LOGI(TAG, "LEDC CW: gpio=%d channel=1", s_driver.cw_gpio);
    ESP_ERROR_CHECK(ledc_channel_config(&cw_cfg));
#endif

    // Initial transition state
    s_driver.transition.current_bri = 0.0f;
    s_driver.transition.current_cct = (float)TLED_DEFAULT_COLOR_TEMP;
    s_driver.transition.start_bri = 0.0f;
    s_driver.transition.start_cct = s_driver.transition.current_cct;
    s_driver.transition.transitioning = false;

    // NVS save timer
    s_driver.nvs_save_timer = xTimerCreate(
        "nvs_save",
        pdMS_TO_TICKS(NVS_SAVE_DEBOUNCE_MS),
        pdFALSE,
        NULL,
        nvs_save_timer_callback
    );
    if (!s_driver.nvs_save_timer) {
        ESP_LOGW(TAG, "Failed to create NVS save timer, saves will be immediate");
    }

    // Start transition task
    BaseType_t ret = xTaskCreate(
        transition_task,
        "tled_transition",
        TLED_TRANSITION_TASK_STACK,
        &s_driver,
        TLED_TRANSITION_TASK_PRIO,
        &s_driver.task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create transition task");
        return NULL;
    }

    ESP_LOGI(TAG, "PWM CCT driver initialized");
    return (app_driver_handle_t)&s_driver;
}

app_driver_handle_t app_driver_button_init(void)
{
    ESP_LOGI(TAG, "Button init on GPIO%d", TLED_BOOT_BUTTON_GPIO);

    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = TLED_BOOT_BUTTON_GPIO,
        .active_level = 0,
    };

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button");
        return NULL;
    }

    esp_err_t err = iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL,
                                            app_driver_button_toggle_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register button callback: %s", esp_err_to_name(err));
    }

    return (app_driver_handle_t)handle;
}
