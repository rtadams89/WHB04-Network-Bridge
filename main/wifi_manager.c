/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Behaviour:
 *  - With credentials (saved or compile-time): join as a station and keep
 *    retrying. If not connected within CONFIG_BRIDGE_PORTAL_FALLBACK_SECONDS,
 *    additionally start the setup portal so the credentials can be fixed. The
 *    portal shuts down again once the station connects.
 *  - Without credentials: start the setup portal only.
 */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "captive_dns.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "wifi";

#define RECONNECT_DELAY_MS        2000
#define RECONNECT_DELAY_PORTAL_MS 15000 /* retry less often so portal clients aren't kicked by channel changes */

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_portal_timer;
static SemaphoreHandle_t s_scan_lock;
static wifi_connected_cb_t s_on_connected;
static bool s_have_credentials;
static volatile bool s_connected;
static volatile bool s_portal_active;
static char s_ap_ssid[33];

static void start_portal(void)
{
    if (s_portal_active) {
        return;
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    const char *ap_password = CONFIG_BRIDGE_SETUP_AP_PASSWORD;
    if (strlen(ap_password) >= 8) {
        strlcpy((char *)ap.ap.password, ap_password, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_portal_active = true;
    captive_dns_start();
    ESP_LOGW(TAG, "Setup portal active: join WiFi '%s' and open http://192.168.4.1/", s_ap_ssid);
}

static void stop_portal(void)
{
    if (!s_portal_active) {
        return;
    }
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal_active = false;
    ESP_LOGI(TAG, "Setup portal stopped");
}

static void reconnect_timer_cb(void *arg)
{
    if (s_have_credentials && !s_connected) {
        esp_wifi_connect();
    }
}

static void portal_timer_cb(void *arg)
{
    if (!s_connected) {
        ESP_LOGW(TAG, "WiFi not connected after %d s, starting setup portal", CONFIG_BRIDGE_PORTAL_FALLBACK_SECONDS);
        start_portal();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_credentials) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        if (s_connected) {
            ESP_LOGW(TAG, "Disconnected from WiFi (reason %d)", ev->reason);
        } else {
            ESP_LOGD(TAG, "Connect attempt failed (reason %d)", ev->reason);
        }
        s_connected = false;
        if (s_have_credentials) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer,
                                 1000ULL * (s_portal_active ? RECONNECT_DELAY_PORTAL_MS : RECONNECT_DELAY_MS));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        esp_timer_stop(s_portal_timer);
        stop_portal();
        if (s_on_connected) {
            s_on_connected();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Device joined setup portal");
    }
}

esp_err_t wifi_manager_start(wifi_connected_cb_t on_connected)
{
    s_on_connected = on_connected;
    const bridge_settings_t *cfg = settings_get();
    s_have_credentials = cfg->ssid[0] != '\0';
    s_scan_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, cfg->hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* Credentials are kept in our own NVS namespace. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    const esp_timer_create_args_t reconnect_args = {.callback = reconnect_timer_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));
    const esp_timer_create_args_t portal_args = {.callback = portal_timer_cb, .name = "wifi_portal"};
    ESP_ERROR_CHECK(esp_timer_create(&portal_args, &s_portal_timer));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WHB04-Bridge-%02X%02X", mac[4], mac[5]);

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg->ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->password, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.threshold.authmode = cfg->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep adds tens of ms of latency to every jog wheel event. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (s_have_credentials) {
        ESP_LOGI(TAG, "Connecting to '%s'", cfg->ssid);
        esp_timer_start_once(s_portal_timer, 1000000ULL * CONFIG_BRIDGE_PORTAL_FALLBACK_SECONDS);
    } else {
        ESP_LOGW(TAG, "No WiFi configured");
        start_portal();
    }
    return ESP_OK;
}

void wifi_manager_get_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->portal_active = s_portal_active;
    out->state = s_connected ? WIFI_STATE_CONNECTED : (s_portal_active ? WIFI_STATE_PORTAL : WIFI_STATE_CONNECTING);
    strlcpy(out->ssid, settings_get()->ssid, sizeof(out->ssid));
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    strlcpy(out->ip, "0.0.0.0", sizeof(out->ip));
    if (s_connected) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

static void json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < dst_size; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += snprintf(dst + o, dst_size - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

esp_err_t wifi_manager_scan_json(char *out, size_t out_size)
{
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_lock);
        return err;
    }

    uint16_t count = 20;
    wifi_ap_record_t records[20];
    esp_wifi_scan_get_ap_records(&count, records);
    xSemaphoreGive(s_scan_lock);

    size_t o = 0;
    o += snprintf(out + o, out_size - o, "[");
    for (uint16_t i = 0; i < count && o + 96 < out_size; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (uint16_t j = 0; j < i; j++) {
            if (strcmp((char *)records[i].ssid, (char *)records[j].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        char ssid[2 * 33 + 8];
        json_escape(ssid, sizeof(ssid), (char *)records[i].ssid);
        o += snprintf(out + o, out_size - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", o > 1 ? "," : "", ssid,
                      records[i].rssi, records[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    snprintf(out + o, out_size - o, "]");
    return ESP_OK;
}
