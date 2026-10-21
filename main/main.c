/*
 * WHB04 Network Bridge
 *
 * Exposes an XHC WHB04B wireless CNC pendant (via its USB dongle) on the
 * network so the Carvera Controller (or any client speaking the protocol in
 * docs/PROTOCOL.md) can use it without a local USB connection.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "bridge_server.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "settings.h"
#include "status_led.h"
#include "usb_pendant.h"
#include "web_ui.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static void on_input_report(const uint8_t *data, size_t len)
{
    bridge_server_post_input(data, len);
}

static void on_dongle_status(bool attached)
{
    bridge_server_post_status(attached);
}

static void start_mdns(void)
{
    const bridge_settings_t *cfg = settings_get();
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(cfg->hostname);
    mdns_instance_name_set("WHB04 Network Bridge");
    mdns_txt_item_t txt[] = {
        {"protocol", "1"},
        {"firmware", esp_app_get_description()->version},
    };
    mdns_service_add(NULL, "_whb04", "_tcp", CONFIG_BRIDGE_TCP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", cfg->hostname);
}

static void on_wifi_connected(void)
{
    static bool mdns_started = false;
    if (!mdns_started) {
        mdns_started = true;
        start_mdns();
    }

    /* Reaching the network proves this firmware works: cancel OTA rollback. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking new firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "WHB04 Network Bridge %s (IDF %s)", app->version, app->idf_ver);

    ESP_ERROR_CHECK(settings_init());

    const usb_pendant_callbacks_t callbacks = {
        .on_input_report = on_input_report,
        .on_status = on_dongle_status,
    };
    /* WiFi first: it initialises the network stack the other modules use. */
    ESP_ERROR_CHECK(wifi_manager_start(on_wifi_connected));
    ESP_ERROR_CHECK(bridge_server_start(CONFIG_BRIDGE_TCP_PORT));
#if CONFIG_BRIDGE_USB_HOST
    ESP_ERROR_CHECK(usb_pendant_start(&callbacks));
#else
    (void)callbacks;
    ESP_LOGW(TAG, "USB host disabled (diagnostic build)");
#endif
    ESP_ERROR_CHECK(web_ui_start(CONFIG_BRIDGE_TCP_PORT));
    status_led_start();

    ESP_LOGI(TAG, "Started; pendant TCP port %d", CONFIG_BRIDGE_TCP_PORT);
}
