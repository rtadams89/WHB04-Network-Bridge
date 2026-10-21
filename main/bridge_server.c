/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Two tasks:
 *  - server task: accepts connections and reads/parses frames from the client
 *  - tx task:     sends queued frames (input reports, status) and heartbeats
 *
 * Only one client is served at a time. A new connection replaces the current
 * one: when a tablet drops off WiFi and comes back, its old socket may still
 * look alive here, and the reconnect must not be locked out.
 */
#include "bridge_server.h"

#include <errno.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "protocol.h"
#include "usb_pendant.h"

static const char *TAG = "server";

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[USB_PENDANT_MAX_REPORT];
} frame_t;

static QueueHandle_t s_tx_queue;
static SemaphoreHandle_t s_lock; /* guards s_client and writes to it */
static int s_client = -1;
static volatile bool s_client_broken;
static volatile bool s_dongle_attached;
static bridge_server_stats_t s_stats;
static uint16_t s_port;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Caller must hold s_lock. */
static bool send_all_locked(const uint8_t *data, size_t len)
{
    if (s_client < 0 || s_client_broken) {
        return false;
    }
    while (len > 0) {
        int n = send(s_client, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "send failed: errno %d", errno);
            s_client_broken = true;
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_frame_locked(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[2 + FRAME_MAX_PAYLOAD];
    buf[0] = type;
    buf[1] = len;
    if (len) {
        memcpy(buf + 2, payload, len);
    }
    bool ok = send_all_locked(buf, 2u + len);
    if (ok) {
        s_stats.frames_sent++;
    }
    return ok;
}

static void send_hello_and_status_locked(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t hello[1 + 32];
    size_t ver_len = strnlen(app->version, sizeof(hello) - 1);
    hello[0] = BRIDGE_PROTOCOL_VERSION;
    memcpy(hello + 1, app->version, ver_len);
    send_frame_locked(FRAME_HELLO, hello, (uint8_t)(1 + ver_len));

    uint8_t status = s_dongle_attached ? 1 : 0;
    send_frame_locked(FRAME_STATUS, &status, 1);
}

static void tx_task(void *arg)
{
    int64_t last_send = now_ms();
    frame_t frame;
    while (true) {
        if (xQueueReceive(s_tx_queue, &frame, pdMS_TO_TICKS(BRIDGE_HEARTBEAT_INTERVAL_MS)) == pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (send_frame_locked(frame.type, frame.data, frame.len)) {
                last_send = now_ms();
            }
            xSemaphoreGive(s_lock);
        }
        if (now_ms() - last_send >= BRIDGE_HEARTBEAT_INTERVAL_MS) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_client >= 0) {
                send_frame_locked(FRAME_HEARTBEAT, NULL, 0);
            }
            xSemaphoreGive(s_lock);
            last_send = now_ms();
        }
    }
}

static void close_client(const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_client >= 0) {
        ESP_LOGI(TAG, "Client %s disconnected (%s)", s_stats.client_addr, reason);
        shutdown(s_client, SHUT_RDWR);
        close(s_client);
        s_client = -1;
    }
    s_client_broken = false;
    s_stats.client_connected = false;
    xQueueReset(s_tx_queue);
    xSemaphoreGive(s_lock);
}

static void configure_client_socket(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 5, interval = 2, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    /* Never let a stalled client block the tx task for long. */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void accept_client(int listen_fd)
{
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        ESP_LOGW(TAG, "accept failed: errno %d", errno);
        return;
    }
    configure_client_socket(fd);

    char addr_str[sizeof(s_stats.client_addr)] = "?";
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&addr;
        inet_ntoa_r(in->sin_addr, addr_str, sizeof(addr_str));
        size_t l = strlen(addr_str);
        snprintf(addr_str + l, sizeof(addr_str) - l, ":%u", ntohs(in->sin_port));
    }

    if (s_client >= 0) {
        close_client("replaced by new connection");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    xQueueReset(s_tx_queue); /* stale reports belong to nobody */
    s_client = fd;
    s_client_broken = false;
    strlcpy(s_stats.client_addr, addr_str, sizeof(s_stats.client_addr));
    s_stats.client_connected = true;
    s_stats.clients_accepted++;
    send_hello_and_status_locked();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Client %s connected", addr_str);
}

static void handle_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    switch (type) {
    case FRAME_FEATURE_REPORT:
        usb_pendant_send_feature(payload, len);
        break;
    case FRAME_CLIENT_HEARTBEAT:
        break;
    default:
        ESP_LOGD(TAG, "Ignoring frame type 0x%02X", type);
        break;
    }
}

static void server_task(void *arg)
{
    int listen_fd = -1;
    uint8_t rx_buf[2 * (2 + FRAME_MAX_PAYLOAD)];
    size_t rx_len = 0;
    int64_t last_rx = 0;

    while (true) {
        if (listen_fd < 0) {
            listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (listen_fd < 0) {
                ESP_LOGE(TAG, "socket failed: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int one = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(s_port),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 2) != 0) {
                ESP_LOGE(TAG, "bind/listen on port %u failed: errno %d", s_port, errno);
                close(listen_fd);
                listen_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "Listening on TCP port %u", s_port);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int maxfd = listen_fd;
        int client = s_client;
        if (client >= 0) {
            FD_SET(client, &readfds);
            if (client > maxfd) {
                maxfd = client;
            }
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "select failed: errno %d", errno);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        if (ready > 0 && FD_ISSET(listen_fd, &readfds)) {
            accept_client(listen_fd);
            rx_len = 0;
            last_rx = now_ms();
            continue; /* client fd changed; rebuild the fd set */
        }

        if (client >= 0 && ready > 0 && FD_ISSET(client, &readfds)) {
            int n = recv(client, rx_buf + rx_len, sizeof(rx_buf) - rx_len, 0);
            if (n <= 0) {
                close_client(n == 0 ? "closed by peer" : "receive error");
                rx_len = 0;
                continue;
            }
            last_rx = now_ms();
            rx_len += (size_t)n;

            size_t pos = 0;
            while (rx_len - pos >= 2 && rx_len - pos >= 2u + rx_buf[pos + 1]) {
                handle_frame(rx_buf[pos], rx_buf + pos + 2, rx_buf[pos + 1]);
                pos += 2u + rx_buf[pos + 1];
            }
            memmove(rx_buf, rx_buf + pos, rx_len - pos);
            rx_len -= pos;
        }

        if (s_client >= 0) {
            if (s_client_broken) {
                close_client("send error");
                rx_len = 0;
            } else if (now_ms() - last_rx > BRIDGE_CLIENT_TIMEOUT_MS) {
                close_client("timeout");
                rx_len = 0;
            }
        }
    }
}

esp_err_t bridge_server_start(uint16_t port)
{
    s_port = port;
    s_tx_queue = xQueueCreate(64, sizeof(frame_t));
    s_lock = xSemaphoreCreateMutex();
    if (s_tx_queue == NULL || s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(tx_task, "bridge_tx", 4096, NULL, 7, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(server_task, "bridge_srv", 4096, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void bridge_server_post_input(const uint8_t *data, size_t len)
{
    if (s_tx_queue == NULL || s_client < 0) {
        return;
    }
    frame_t frame = {.type = FRAME_INPUT_REPORT, .len = (uint8_t)(len > sizeof(frame.data) ? sizeof(frame.data) : len)};
    memcpy(frame.data, data, frame.len);
    if (xQueueSend(s_tx_queue, &frame, 0) != pdTRUE) {
        s_stats.frames_dropped++;
    }
}

void bridge_server_post_status(bool attached)
{
    s_dongle_attached = attached;
    if (s_tx_queue == NULL || s_client < 0) {
        return;
    }
    frame_t frame = {.type = FRAME_STATUS, .len = 1, .data = {attached ? 1 : 0}};
    if (xQueueSend(s_tx_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_stats.frames_dropped++;
    }
}

void bridge_server_get_stats(bridge_server_stats_t *out)
{
    *out = s_stats;
}
