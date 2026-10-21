/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "usb_pendant.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";

/*
 * All operations that touch a device handle (open, start, close, set_report)
 * run in a single task fed by this queue, so they are naturally serialised.
 * This matters because control transfers complete via the HID driver's own
 * background task: blocking that task on a lock held by a pending
 * set_report would deadlock.
 */
typedef enum {
    EVT_DEVICE_CONNECTED,
    EVT_DEVICE_DISCONNECTED,
    EVT_FEATURE_REPORT,
} usb_evt_kind_t;

typedef struct {
    usb_evt_kind_t kind;
    hid_host_device_handle_t handle;
    uint8_t len;
    uint8_t data[USB_PENDANT_MAX_REPORT];
} usb_evt_t;

static QueueHandle_t s_queue;
static usb_pendant_callbacks_t s_cb;
/* Written only by the app task; read by others (pointer-sized, atomic on Xtensa). */
static volatile hid_host_device_handle_t s_device;
static usb_pendant_stats_t s_stats;

static void set_attached(hid_host_device_handle_t handle)
{
    s_device = handle;
    if (s_cb.on_status) {
        s_cb.on_status(handle != NULL);
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* Runs in the HID driver background task. */
static void interface_callback(hid_host_device_handle_t handle, const hid_host_interface_event_t event, void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[USB_PENDANT_MAX_REPORT];
        size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &len) == ESP_OK && len > 0 &&
            handle == s_device) {
            s_stats.input_reports++;
            if (s_cb.on_input_report) {
                s_cb.on_input_report(data, len);
            }
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
        usb_evt_t evt = {.kind = EVT_DEVICE_DISCONNECTED, .handle = handle};
        xQueueSendToFront(s_queue, &evt, pdMS_TO_TICKS(100));
        break;
    }
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    default:
        break;
    }
}

/* Runs in the HID driver background task. */
static void driver_callback(hid_host_device_handle_t handle, const hid_host_driver_event_t event, void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        usb_evt_t evt = {.kind = EVT_DEVICE_CONNECTED, .handle = handle};
        xQueueSendToFront(s_queue, &evt, pdMS_TO_TICKS(100));
    }
}

static void handle_connected(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        return;
    }

    if (s_device != NULL) {
        ESP_LOGI(TAG, "Ignoring extra HID interface %u (already using one)", params.iface_num);
        return;
    }

    const hid_host_device_config_t dev_config = {
        .callback = interface_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_device_open(handle, &dev_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HID interface: %s", esp_err_to_name(err));
        return;
    }

    hid_host_dev_info_t info = {0};
    if (hid_host_get_device_info(handle, &info) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read device descriptor");
    }
    ESP_LOGI(TAG, "HID device %04X:%04X interface %u (subclass %u, protocol %u)", info.VID, info.PID,
             params.iface_num, params.sub_class, params.proto);

#if !CONFIG_BRIDGE_ACCEPT_ANY_HID
    if (info.VID != CONFIG_BRIDGE_PENDANT_VID || info.PID != CONFIG_BRIDGE_PENDANT_PID) {
        ESP_LOGW(TAG, "Not a WHB04 pendant (expected %04X:%04X), ignoring", CONFIG_BRIDGE_PENDANT_VID,
                 CONFIG_BRIDGE_PENDANT_PID);
        hid_host_device_close(handle);
        return;
    }
#endif

    err = hid_host_device_start(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HID interface: %s", esp_err_to_name(err));
        hid_host_device_close(handle);
        return;
    }

    s_stats.vid = info.VID;
    s_stats.pid = info.PID;
    ESP_LOGI(TAG, "Pendant dongle attached");
    set_attached(handle);
}

static void handle_disconnected(hid_host_device_handle_t handle)
{
    if (handle == s_device) {
        ESP_LOGI(TAG, "Pendant dongle removed");
        set_attached(NULL);
    }
    hid_host_device_close(handle);
}

static void handle_feature(const usb_evt_t *evt)
{
    hid_host_device_handle_t device = s_device;
    if (device == NULL || evt->len == 0) {
        return;
    }
    uint8_t report_id = evt->data[0];
    /* hidapi convention: a leading 0 means the device doesn't use report IDs. */
    uint8_t *payload = (uint8_t *)evt->data + (report_id == 0 ? 1 : 0);
    size_t payload_len = evt->len - (report_id == 0 ? 1 : 0);

    esp_err_t err = hid_class_request_set_report(device, HID_REPORT_TYPE_FEATURE, report_id, payload, payload_len);
    if (err == ESP_OK) {
        s_stats.feature_reports++;
    } else {
        s_stats.feature_errors++;
        ESP_LOGW(TAG, "SET_REPORT failed: %s", esp_err_to_name(err));
    }
}

static void usb_app_task(void *arg)
{
    usb_evt_t evt;
    while (true) {
        if (xQueueReceive(s_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.kind) {
        case EVT_DEVICE_CONNECTED:
            handle_connected(evt.handle);
            break;
        case EVT_DEVICE_DISCONNECTED:
            handle_disconnected(evt.handle);
            break;
        case EVT_FEATURE_REPORT:
            handle_feature(&evt);
            break;
        }
    }
}

esp_err_t usb_pendant_start(const usb_pendant_callbacks_t *callbacks)
{
    s_cb = *callbacks;
    s_queue = xQueueCreate(32, sizeof(usb_evt_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Install the USB host library first, then the HID class driver. */
    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 10, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const hid_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority = 9,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_install(&driver_config);
    if (err != ESP_OK) {
        return err;
    }

    ok = xTaskCreatePinnedToCore(usb_app_task, "usb_app", 4096, NULL, 8, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool usb_pendant_attached(void)
{
    return s_device != NULL;
}

bool usb_pendant_send_feature(const uint8_t *data, size_t len)
{
    if (s_device == NULL || len == 0 || len > USB_PENDANT_MAX_REPORT) {
        return false;
    }
    usb_evt_t evt = {.kind = EVT_FEATURE_REPORT, .len = (uint8_t)len};
    memcpy(evt.data, data, len);
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        s_stats.feature_dropped++;
        return false;
    }
    return true;
}

void usb_pendant_get_stats(usb_pendant_stats_t *out)
{
    *out = s_stats;
}
