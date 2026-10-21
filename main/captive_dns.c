/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "captive_dns.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";

#define AP_IP_ADDR 0xC0A80401 /* 192.168.4.1, the soft-AP default */

static TaskHandle_t s_task;
static volatile bool s_running;

static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        goto done;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno %d", errno);
        close(fd);
        goto done;
    }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) {
            continue;
        }
        /* Only plain queries with exactly one question. */
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if ((buf[2] & 0x80) || qdcount != 1) {
            continue;
        }
        /* Walk the question name. */
        int pos = 12;
        while (pos < n && buf[pos] != 0) {
            pos += buf[pos] + 1;
        }
        pos += 1 + 4; /* terminating zero + QTYPE + QCLASS */
        if (pos > n || pos + 16 > (int)sizeof(buf)) {
            continue;
        }
        uint16_t qtype = (buf[pos - 4] << 8) | buf[pos - 3];

        buf[2] = 0x84 | (buf[2] & 0x01); /* response, authoritative, keep RD */
        buf[3] = 0x80;                   /* RA, no error */
        buf[6] = 0;                      /* ANCOUNT */
        buf[7] = 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NSCOUNT, ARCOUNT */
        int len = pos;
        if (qtype == 1 /* A */) {
            buf[7] = 1;
            const uint8_t answer[16] = {
                0xC0, 0x0C,             /* pointer to the question name */
                0x00, 0x01, 0x00, 0x01, /* type A, class IN */
                0x00, 0x00, 0x00, 0x3C, /* TTL 60 s */
                0x00, 0x04,             /* RDLENGTH */
                (AP_IP_ADDR >> 24) & 0xFF, (AP_IP_ADDR >> 16) & 0xFF, (AP_IP_ADDR >> 8) & 0xFF, AP_IP_ADDR & 0xFF,
            };
            memcpy(buf + pos, answer, sizeof(answer));
            len += sizeof(answer);
        }
        sendto(fd, buf, len, 0, (struct sockaddr *)&from, from_len);
    }
    close(fd);

done:
    s_task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (s_task != NULL) {
        return;
    }
    s_running = true;
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, &s_task);
}

void captive_dns_stop(void)
{
    s_running = false; /* the task exits within its 1 s receive timeout */
}
