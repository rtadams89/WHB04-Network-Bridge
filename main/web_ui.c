/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "web_ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_server.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "settings.h"
#include "usb_pendant.h"
#include "wifi_manager.h"

static const char *TAG = "web";

#if CONFIG_BRIDGE_WEB_OTA
#define WEB_OTA_ENABLED 1
#else
#define WEB_OTA_ENABLED 0
#endif

static uint16_t s_tcp_port;
static esp_timer_handle_t s_restart_timer;

static const char INDEX_HTML[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WHB04 Network Bridge</title><style>"
    ":root{--bg:#f4f5f7;--card:#fff;--fg:#1d2228;--mut:#68717c;--line:#dde1e6;--acc:#1f6feb;--ok:#1a7f37;--bad:#cf222e}"
    "@media(prefers-color-scheme:dark){:root{--bg:#111418;--card:#1a1f25;--fg:#e6e9ec;--mut:#98a1ab;--line:#2c333b;"
    "--acc:#4c8dff;--ok:#3fb950;--bad:#f85149}}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
    "font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
    "main{max-width:560px;margin:0 auto;padding:16px}h1{font-size:20px;margin:8px 0 16px}"
    "h2{font-size:15px;margin:0 0 12px}section{background:var(--card);border:1px solid var(--line);"
    "border-radius:10px;padding:16px;margin-bottom:14px}dl{display:grid;grid-template-columns:max-content 1fr;"
    "gap:6px 16px;margin:0}dt{color:var(--mut)}dd{margin:0;word-break:break-all}"
    ".ok{color:var(--ok)}.bad{color:var(--bad)}label{display:block;margin:10px 0 4px;color:var(--mut)}"
    "input,select{width:100%;padding:9px 10px;border:1px solid var(--line);border-radius:7px;"
    "background:var(--bg);color:var(--fg);font:inherit}"
    "button{margin-top:14px;padding:9px 16px;border:0;border-radius:7px;background:var(--acc);color:#fff;"
    "font:inherit;cursor:pointer}button.sec{background:transparent;color:var(--acc);border:1px solid var(--line)}"
    ".row{display:flex;gap:8px;align-items:flex-end}.row>div{flex:1}.msg{margin-top:10px;color:var(--mut)}"
    "small{color:var(--mut)}</style></head><body><main>"
    "<h1>WHB04 Network Bridge</h1>"
    "<section><h2>Status</h2><dl id=st><dt>Loading&hellip;</dt><dd></dd></dl></section>"
    "<section><h2>WiFi</h2><form id=wf>"
    "<div class=row><div><label for=ssid>Network (SSID)</label><input id=ssid name=ssid maxlength=32 required "
    "list=nets></div><button type=button class=sec id=scan>Scan</button></div><datalist id=nets></datalist>"
    "<label for=pw>Password</label><input id=pw name=password type=password maxlength=64 "
    "placeholder='leave empty for an open network'>"
    "<label for=hn>Hostname</label><input id=hn name=hostname maxlength=32 pattern='[a-z0-9]([a-z0-9-]*[a-z0-9])?'>"
    "<button>Save and reboot</button><div class=msg id=wm></div></form></section>"
    "<section id=ota><h2>Firmware update</h2><small>Upload <b>whb04-bridge-ota.bin</b> from a release "
    "(not the -full image).</small><input id=fw type=file accept=.bin style='margin-top:10px'>"
    "<button id=up>Upload</button><div class=msg id=um></div></section>"
    "</main><script>"
    "const $=i=>document.getElementById(i);"
    "function esc(s){return String(s).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
    "let filled=false;"
    "async function st(){try{const r=await fetch('/api/status');const s=await r.json();"
    "const rows=[['Firmware',s.firmware],['WiFi',s.wifi_state==='connected'?"
    "'<span class=ok>connected</span> to '+esc(s.ssid)+' ('+s.rssi+' dBm)':esc(s.wifi_state)],"
    "['Address',s.wifi_state==='connected'?esc(s.ip)+':'+s.port+' &nbsp;<small>('+esc(s.hostname)+')</small>':'-'],"
    "['Pendant dongle',s.dongle?'<span class=ok>attached</span>':'<span class=bad>not detected</span>'],"
    "['Controller',s.client?'<span class=ok>connected</span> from '+esc(s.client_addr):'none'],"
    "['Reports',s.input_reports+' in / '+s.feature_reports+' out'+(s.feature_errors?' ('+s.feature_errors+' errors)':'')],"
    "['Uptime',Math.floor(s.uptime/3600)+'h '+Math.floor(s.uptime%3600/60)+'m']];"
    "if(s.portal)rows.splice(1,0,['Setup AP','<span class=bad>active</span> ('+esc(s.ap_ssid)+')']);"
    "$('st').innerHTML=rows.map(r=>'<dt>'+r[0]+'</dt><dd>'+r[1]+'</dd>').join('');"
    "if(!filled){$('ssid').value=s.ssid;$('hn').value=s.hostname;filled=true}"
    "$('ota').style.display=s.ota?'':'none'}catch(e){}}"
    "st();setInterval(st,2000);"
    "$('scan').onclick=async()=>{$('wm').textContent='Scanning\\u2026';try{const r=await fetch('/api/scan');"
    "const n=await r.json();$('nets').innerHTML=n.map(x=>'<option value=\"'+esc(x.ssid)+'\">'+x.rssi+' dBm'"
    "+(x.secure?'':' (open)')+'</option>').join('');$('wm').textContent=n.length+' networks found';"
    "$('ssid').focus()}catch(e){$('wm').textContent='Scan failed, try again'}};"
    "$('wf').onsubmit=async e=>{e.preventDefault();const b=new URLSearchParams(new FormData($('wf')));"
    "$('wm').textContent='Saving\\u2026';const r=await fetch('/api/wifi',{method:'POST',body:b});"
    "$('wm').textContent=await r.text()};"
    "$('up').onclick=async()=>{const f=$('fw').files[0];if(!f)return;$('um').textContent='Uploading\\u2026 '"
    "+'(do not power off)';const r=await fetch('/api/update',{method:'POST',body:f});"
    "$('um').textContent=await r.text()};"
    "</script></body></html>";

static void restart_cb(void *arg)
{
    esp_restart();
}

static void schedule_restart(void)
{
    esp_timer_start_once(s_restart_timer, 1500 * 1000);
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

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t wifi;
    wifi_manager_get_status(&wifi);
    usb_pendant_stats_t usb;
    usb_pendant_get_stats(&usb);
    bridge_server_stats_t server;
    bridge_server_get_stats(&server);
    const bridge_settings_t *cfg = settings_get();

    static const char *state_names[] = {"connecting", "connected", "setup portal"};
    char ssid[80], ap_ssid[80], hostname[80], fw[80];
    json_escape(ssid, sizeof(ssid), wifi.ssid);
    json_escape(ap_ssid, sizeof(ap_ssid), wifi.ap_ssid);
    json_escape(hostname, sizeof(hostname), cfg->hostname);
    json_escape(fw, sizeof(fw), esp_app_get_description()->version);

    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"firmware\":\"%s\",\"hostname\":\"%s\",\"wifi_state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
                     "\"rssi\":%d,\"portal\":%s,\"ap_ssid\":\"%s\",\"port\":%u,\"dongle\":%s,\"vid\":%u,\"pid\":%u,"
                     "\"client\":%s,\"client_addr\":\"%s\",\"clients_accepted\":%lu,\"input_reports\":%lu,"
                     "\"feature_reports\":%lu,\"feature_errors\":%lu,\"feature_dropped\":%lu,\"frames_dropped\":%lu,"
                     "\"uptime\":%lld,\"ota\":%s}",
                     fw, hostname, state_names[wifi.state], ssid, wifi.ip, wifi.rssi,
                     wifi.portal_active ? "true" : "false", ap_ssid, s_tcp_port,
                     usb_pendant_attached() ? "true" : "false", usb.vid, usb.pid,
                     server.client_connected ? "true" : "false", server.client_addr,
                     (unsigned long)server.clients_accepted, (unsigned long)usb.input_reports,
                     (unsigned long)usb.feature_reports, (unsigned long)usb.feature_errors,
                     (unsigned long)usb.feature_dropped, (unsigned long)server.frames_dropped,
                     (long long)(esp_timer_get_time() / 1000000), WEB_OTA_ENABLED ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    char *json = malloc(2048);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    esp_err_t err = wifi_manager_scan_json(json, 2048);
    if (err != ESP_OK) {
        free(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
    }
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[384];
    if (req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
    }
    int received = 0;
    while (received < (int)req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = '\0';

    char ssid[3 * SETTINGS_SSID_MAX + 1] = "";
    char password[3 * SETTINGS_PASSWORD_MAX + 1] = "";
    char hostname[3 * SETTINGS_HOSTNAME_MAX + 1] = "";
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "password", password, sizeof(password));
    httpd_query_key_value(body, "hostname", hostname, sizeof(hostname));
    url_decode(ssid);
    url_decode(password);
    url_decode(hostname);

    if (ssid[0] == '\0' || strlen(ssid) > SETTINGS_SSID_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID must be 1-32 characters");
    }
    if (strlen(password) > SETTINGS_PASSWORD_MAX || (password[0] && strlen(password) < 8)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password must be 8-64 characters (or empty)");
    }
    for (char *p = hostname; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    if (hostname[0] && !settings_hostname_valid(hostname)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Hostname may only contain a-z, 0-9 and '-' (max 32)");
    }

    esp_err_t err = settings_save(ssid, password, hostname);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
    }
    ESP_LOGI(TAG, "WiFi settings saved (SSID '%s'), rebooting", ssid);
    httpd_resp_sendstr(req, "Saved. Rebooting and joining the network - reconnect to your normal WiFi.");
    schedule_restart();
    return ESP_OK;
}

#if CONFIG_BRIDGE_WEB_OTA
static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware image has an invalid size");
    }

    char *buf = malloc(4096);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    esp_ota_handle_t ota = 0;
    bool started = false;
    size_t remaining = req->content_len;
    const char *error = NULL;
    ESP_LOGI(TAG, "Receiving firmware (%u bytes) into %s", (unsigned)req->content_len, target->label);

    while (remaining > 0 && error == NULL) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            error = "Upload interrupted";
            break;
        }
        if (!started) {
            /* Reject the merged "full" image (bootloader first) or other files early. */
            const size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + desc_offset);
            if ((size_t)n < desc_offset + sizeof(esp_app_desc_t) || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC ||
                desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                error = "Not an application image. Upload whb04-bridge-ota.bin, not the -full image.";
                break;
            }
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                error = "Could not start update";
                break;
            }
            started = true;
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            error = "Flash write failed";
            break;
        }
        remaining -= (size_t)n;
    }
    free(buf);

    if (error == NULL && esp_ota_end(ota) != ESP_OK) {
        started = false;
        error = "Image verification failed";
    }
    if (error != NULL) {
        if (started) {
            esp_ota_abort(ota);
        }
        ESP_LOGE(TAG, "Firmware update failed: %s", error);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not activate new firmware");
    }
    ESP_LOGI(TAG, "Firmware update complete, rebooting");
    httpd_resp_sendstr(req, "Update complete. Rebooting\xe2\x80\xa6");
    schedule_restart();
    return ESP_OK;
}
#endif

/* Anything else: in setup-portal mode redirect to the setup page (this is
 * what triggers the "sign in to network" prompt on phones), otherwise 404. */
static esp_err_t fallback_get(httpd_req_t *req)
{
    wifi_status_t wifi;
    wifi_manager_get_status(&wifi);
    if (wifi.portal_active) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t web_ui_start(uint16_t tcp_port)
{
    s_tcp_port = tcp_port;
    const esp_timer_create_args_t restart_args = {.callback = restart_cb, .name = "restart"};
    ESP_ERROR_CHECK(esp_timer_create(&restart_args, &s_restart_timer));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
#if CONFIG_BRIDGE_WEB_OTA
        {.uri = "/api/update", .method = HTTP_POST, .handler = update_post},
#endif
        {.uri = "/*", .method = HTTP_GET, .handler = fallback_get},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(server, &handlers[i]);
    }
    return ESP_OK;
}
