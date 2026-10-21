/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "status_led.h"

#include "bridge_server.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "settings.h"
#include "usb_pendant.h"
#include "wifi_manager.h"

#if CONFIG_BRIDGE_RESET_BUTTON_GPIO >= 0
static const char *TAG = "led";
#endif

#define TICK_MS          50
#define RESET_HOLD_TICKS (5000 / TICK_MS)

static void led_set(bool on)
{
#if CONFIG_BRIDGE_STATUS_LED_GPIO >= 0
#if CONFIG_BRIDGE_STATUS_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_BRIDGE_STATUS_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_BRIDGE_STATUS_LED_GPIO, on ? 1 : 0);
#endif
#endif
}

static bool pattern(uint32_t tick)
{
    wifi_status_t wifi;
    wifi_manager_get_status(&wifi);
    uint32_t ms = tick * TICK_MS;

    if (wifi.portal_active && wifi.state != WIFI_STATE_CONNECTED) {
        return (ms % 200) < 100;
    }
    if (wifi.state != WIFI_STATE_CONNECTED) {
        return (ms % 1000) < 500;
    }
    bridge_server_stats_t server;
    bridge_server_get_stats(&server);
    if (usb_pendant_attached() && server.client_connected) {
        return true;
    }
    return (ms % 2000) < 100;
}

static void led_task(void *arg)
{
    uint32_t tick = 0;
#if CONFIG_BRIDGE_RESET_BUTTON_GPIO >= 0
    uint32_t held = 0;
#endif
    while (true) {
#if CONFIG_BRIDGE_RESET_BUTTON_GPIO >= 0
        if (gpio_get_level(CONFIG_BRIDGE_RESET_BUTTON_GPIO) == 0) {
            if (++held == RESET_HOLD_TICKS) {
                ESP_LOGW(TAG, "Reset button held: clearing WiFi settings and rebooting");
                settings_clear_wifi();
                for (int i = 0; i < 10; i++) {
                    led_set(i % 2 == 0);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                esp_restart();
            }
            /* While held, show a steady light as feedback. */
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            tick++;
            continue;
        }
        held = 0;
#endif
        led_set(pattern(tick));
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        tick++;
    }
}

void status_led_start(void)
{
#if CONFIG_BRIDGE_STATUS_LED_GPIO >= 0
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);
    led_set(false);
#endif
#if CONFIG_BRIDGE_RESET_BUTTON_GPIO >= 0
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&button);
#endif
    xTaskCreate(led_task, "status_led", 3072, NULL, 2, NULL);
}
