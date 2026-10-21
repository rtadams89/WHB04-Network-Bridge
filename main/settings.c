/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "settings.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NVS_NS = "bridge";

static bridge_settings_t s_settings;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    strncpy(dst, src ? src : "", dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t out_size)
{
    size_t len = out_size;
    return nvs_get_str(h, key, out, &len) == ESP_OK;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_settings, 0, sizeof(s_settings));
    copy_str(s_settings.ssid, sizeof(s_settings.ssid), CONFIG_BRIDGE_DEFAULT_WIFI_SSID);
    copy_str(s_settings.password, sizeof(s_settings.password), CONFIG_BRIDGE_DEFAULT_WIFI_PASSWORD);
    copy_str(s_settings.hostname, sizeof(s_settings.hostname), CONFIG_BRIDGE_DEFAULT_HOSTNAME);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char ssid[sizeof(s_settings.ssid)];
        if (nvs_read_str(h, "ssid", ssid, sizeof(ssid)) && ssid[0] != '\0') {
            copy_str(s_settings.ssid, sizeof(s_settings.ssid), ssid);
            s_settings.password[0] = '\0';
            nvs_read_str(h, "password", s_settings.password, sizeof(s_settings.password));
            s_settings.wifi_from_nvs = true;
        }
        char hostname[sizeof(s_settings.hostname)];
        if (nvs_read_str(h, "hostname", hostname, sizeof(hostname)) && settings_hostname_valid(hostname)) {
            copy_str(s_settings.hostname, sizeof(s_settings.hostname), hostname);
        }
        nvs_close(h);
    }

    if (!settings_hostname_valid(s_settings.hostname)) {
        copy_str(s_settings.hostname, sizeof(s_settings.hostname), "whb04-bridge");
    }

    ESP_LOGI(TAG, "hostname '%s', WiFi '%s' (%s)", s_settings.hostname,
             s_settings.ssid[0] ? s_settings.ssid : "<none>",
             s_settings.wifi_from_nvs ? "saved" : (s_settings.ssid[0] ? "compile-time" : "unset"));
    return ESP_OK;
}

const bridge_settings_t *settings_get(void)
{
    return &s_settings;
}

esp_err_t settings_save(const char *ssid, const char *password, const char *hostname)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "password", password ? password : "");
    }
    if (err == ESP_OK && hostname && hostname[0]) {
        err = nvs_set_str(h, "hostname", hostname);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "password");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool settings_hostname_valid(const char *hostname)
{
    size_t len = hostname ? strlen(hostname) : 0;
    if (len == 0 || len > SETTINGS_HOSTNAME_MAX || hostname[0] == '-' || hostname[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) {
            return false;
        }
    }
    return true;
}
