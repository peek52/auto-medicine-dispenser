/**
 * usb_mouse.c — Minimal USB HID Mouse driver for ESP32-P4
 *
 * Uses raw usb_host (ESP-IDF 5.3 "usb" component) directly.
 * No esp_hid dependency — ESP32-P4 has no BT so esp_hid's
 * preprocess/postprocess symbols would be unresolved.
 *
 * Architecture:
 *  - usb_lib_task: processes USB host library events
 *  - usb_class_task: handles device connect/disconnect, claims HID
 *    interface and submits interrupt IN transfers to receive reports
 *  - Interrupt transfer callback: parses boot-protocol mouse report
 *    and updates shared mouse_state_t under a mutex
 */
#include "usb_mouse.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_mouse";

/* ── Screen bounds ─────────────────────────────────────────── */
int16_t usb_mouse_screen_w = 480;
int16_t usb_mouse_screen_h = 320;

/* ── Shared state ────────────────────────────────────────────*/
static mouse_state_t     s_state  = {240, 160, false, false, false};
static SemaphoreHandle_t s_mutex  = NULL;

/* ── USB Host internals ─────────────────────────────────────*/
#define CLIENT_NUM_EVENT_MSG  5
#define USB_HID_INTF_CLASS    0x03
#define USB_HID_INTF_BOOT_SUB 0x01
#define USB_HID_PROTO_MOUSE   0x02

static usb_host_client_handle_t s_client_hdl = NULL;
static usb_device_handle_t      s_dev_hdl    = NULL;
static uint8_t                  s_intf_num   = 0xFF;
static usb_transfer_t          *s_xfer       = NULL;
static volatile bool             s_connected  = false;

/* ── Interrupt transfer callback ────────────────────────────*/
static void intr_transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes >= 3) {
        const uint8_t *d  = transfer->data_buffer;
        int8_t dx         = (int8_t)d[1];
        int8_t dy         = (int8_t)d[2];
        bool btn_l        = (d[0] & 0x01) != 0;
        bool btn_r        = (d[0] & 0x02) != 0;

        if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
            s_state.x += dx;
            s_state.y += dy;
            if (s_state.x < 0) s_state.x = 0;
            if (s_state.y < 0) s_state.y = 0;
            if (s_state.x >= usb_mouse_screen_w) s_state.x = usb_mouse_screen_w - 1;
            if (s_state.y >= usb_mouse_screen_h) s_state.y = usb_mouse_screen_h - 1;
            s_state.btn_left  = btn_l;
            s_state.btn_right = btn_r;
            s_state.changed   = true;
            xSemaphoreGive(s_mutex);
        }
    }
    /* Re-submit to keep polling */
    if (s_xfer) {
        usb_host_transfer_submit(transfer);
    }
}

/* ── Helper: find HID mouse interrupt EP and claim interface ─*/
static bool claim_mouse_interface(void)
{
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(s_dev_hdl, &cfg) != ESP_OK) return false;

    int offset = 0;
    const usb_intf_desc_t *intf     = NULL;
    const usb_ep_desc_t   *ep_intr  = NULL;

    while (offset < cfg->wTotalLength) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)((uint8_t *)cfg + offset);
        if (desc->bLength == 0) break;

        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)desc;
            if (i->bInterfaceClass    == USB_HID_INTF_CLASS &&
                i->bInterfaceSubClass == USB_HID_INTF_BOOT_SUB &&
                i->bInterfaceProtocol == USB_HID_PROTO_MOUSE) {
                intf = i;
                ep_intr = NULL; // reset EP search for this interface
            }
        } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && intf) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
            if ((ep->bEndpointAddress & 0x80) &&           // IN
                (ep->bmAttributes & 0x3) == 0x3) {         // Interrupt
                ep_intr = ep;
                break;
            }
        }
        offset += desc->bLength;
    }

    if (!intf || !ep_intr) return false;
    s_intf_num = intf->bInterfaceNumber;

    if (usb_host_interface_claim(s_client_hdl, s_dev_hdl, s_intf_num, intf->bAlternateSetting) != ESP_OK) {
        ESP_LOGE(TAG, "claim interface failed");
        return false;
    }

    /* Allocate and arm interrupt transfer */
    size_t pkt = USB_EP_DESC_GET_MPS(ep_intr);
    if (pkt < 4) pkt = 4;
    usb_host_transfer_alloc(pkt, 0, &s_xfer);
    if (!s_xfer) { ESP_LOGE(TAG, "transfer alloc failed"); return false; }

    s_xfer->device_handle = s_dev_hdl;
    s_xfer->bEndpointAddress = ep_intr->bEndpointAddress;
    s_xfer->callback = intr_transfer_cb;
    s_xfer->context  = NULL;
    s_xfer->num_bytes = pkt;
    usb_host_transfer_submit(s_xfer);
    ESP_LOGI(TAG, "Mouse claimed, EP=0x%02X MPS=%d", ep_intr->bEndpointAddress, (int)pkt);
    return true;
}

/* ── Client event handler ───────────────────────────────────*/
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "New USB device addr=%d", msg->new_dev.address);
        usb_host_device_open(s_client_hdl, msg->new_dev.address, &s_dev_hdl);
        if (!claim_mouse_interface()) {
            ESP_LOGW(TAG, "Not a boot-mouse or claim failed");
            usb_host_device_close(s_client_hdl, s_dev_hdl);
            s_dev_hdl = NULL;
        } else {
            s_connected = true;
        }
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI(TAG, "USB device removed");
        s_connected = false;
        if (s_xfer) { usb_host_transfer_free(s_xfer); s_xfer = NULL; }
        if (s_dev_hdl) {
            usb_host_interface_release(s_client_hdl, s_dev_hdl, s_intf_num);
            usb_host_device_close(s_client_hdl, s_dev_hdl);
            s_dev_hdl = NULL;
        }
    }
}

/* ── USB host library task ───────────────────────────────────*/
static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;
    }
    usb_host_uninstall();
    vTaskDelete(NULL);
}

/* ── USB client (class driver) task ─────────────────────────*/
static void usb_class_task(void *arg)
{
    usb_host_client_config_t client_cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async.client_event_callback = client_event_cb,
        .async.callback_arg          = NULL,
    };
    usb_host_client_register(&client_cfg, &s_client_hdl);
    while (1) {
        usb_host_client_handle_events(s_client_hdl, portMAX_DELAY);
    }
}

/* ── Public API ──────────────────────────────────────────────*/
void usb_mouse_start(void)
{
    s_mutex = xSemaphoreCreateMutex();

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_lib task");
        return;
    }
    if (xTaskCreate(usb_class_task, "usb_class", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_class task");
        return;
    }
    ESP_LOGI(TAG, "USB Mouse host started (raw usb_host)");
}

mouse_state_t usb_mouse_get(void)
{
    mouse_state_t snap = {240, 160, false, false, false};
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snap = s_state;
        s_state.changed = false;
        xSemaphoreGive(s_mutex);
    }
    return snap;
}

bool usb_mouse_is_connected(void) { return s_connected; }
