/*
 * TLED - Matter-over-Thread LED Controller
 * PWM CCT Driver Interface
 */

#pragma once

#include <esp_err.h>
#include <esp_matter.h>
#include "app_nvs_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *app_driver_handle_t;

app_driver_handle_t app_driver_light_init(void);
app_driver_handle_t app_driver_button_init(void);

esp_err_t app_driver_light_set_power(app_driver_handle_t handle, bool power);
bool      app_driver_light_get_power(app_driver_handle_t handle);

esp_err_t app_driver_light_set_brightness(app_driver_handle_t handle, uint8_t brightness);
esp_err_t app_driver_light_set_brightness_with_transition(app_driver_handle_t handle,
                                                           uint8_t brightness,
                                                           uint32_t transition_ms);

esp_err_t app_driver_light_set_color_temp(app_driver_handle_t handle, uint16_t color_temp_mireds);
esp_err_t app_driver_light_set_color_temp_with_transition(app_driver_handle_t handle,
                                                           uint16_t color_temp_mireds,
                                                           uint32_t transition_ms);

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle,
                                       uint16_t endpoint_id,
                                       uint32_t cluster_id,
                                       uint32_t attribute_id,
                                       esp_matter_attr_val_t *val);

esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id);

#ifdef __cplusplus
}
#endif
