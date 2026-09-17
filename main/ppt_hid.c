/*
 * BLE HID presentation remote for the AI Passport.
 *
 * The report map and cross-platform slideshow shortcuts are adapted from the
 * MIT-licensed YeatsLiao/ai-passport-ppt project via kidnappe/ai-passport's
 * demo/o-platform branch. NimBLE lifecycle and command dispatch are adapted
 * to this repository's page-owned demo architecture.
 */
#include "ppt_hid.h"

#include "demo_radio.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include <string.h>

static const char *TAG = "ppt_hid";

#define PPT_DEVICE_NAME             "PPT-Remote"
#define PPT_APPEARANCE_KEYBOARD     0x03C1
#define PPT_REPORT_ID_KEYBOARD      1
#define PPT_COMMAND_QUEUE_LENGTH    6
#define PPT_KEY_HOLD_MS             80
#define PPT_COMBO_GAP_MS            120
#define PPT_DISCONNECT_WAIT_MS      500

#define HID_KEY_LEFT_ARROW          0x50
#define HID_KEY_RIGHT_ARROW         0x4F
#define HID_KEY_ESCAPE              0x29
#define HID_KEY_F5                  0x3E
#define HID_KEY_RETURN              0x28
#define HID_KEY_P                   0x13
#define HID_MOD_L_SHIFT             0x02
#define HID_MOD_L_ALT               0x04
#define HID_MOD_L_GUI               0x08

static const uint8_t s_keyboard_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /* Report ID 1 */
    0x05, 0x07,       /* Usage Page (Keyboard) */
    0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,       /* Modifier byte */
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x01,       /* Reserved byte */
    0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02,       /* LED output */
    0x95, 0x01, 0x75, 0x03,
    0x91, 0x01,
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,       /* Six key slots */
    0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_keyboard_report_map, .len = sizeof(s_keyboard_report_map) },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = PPT_DEVICE_NAME,
    .manufacturer_name = "FoloToy",
    .serial_number = "AI-Passport",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static esp_hidd_dev_t *s_hid_dev;
static SemaphoreHandle_t s_host_stopped;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_command_task;
static volatile ppt_hid_state_t s_state;
static volatile int s_error;
static volatile bool s_running;
static volatile bool s_authenticated;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_addr_type;
static bool s_nimble_initialized;
static bool s_host_started;

static int start_advertising(void);

static void set_failed(int error)
{
    s_error = error;
    s_state = PPT_HID_FAILED;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            if (s_running) return start_advertising();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        s_authenticated = false;
        s_state = PPT_HID_PAIRING;
        {
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "security initiate failed: %d", rc);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            s_authenticated = true;
            s_state = PPT_HID_READY;
            ESP_LOGI(TAG, "encrypted HID connection ready");
        } else {
            s_authenticated = false;
            ESP_LOGW(TAG, "HID encryption failed: %d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_authenticated = false;
        if (s_running) {
            int rc = start_advertising();
            if (rc != 0) set_failed(rc);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_running && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            int rc = start_advertising();
            if (rc != 0) set_failed(rc);
        }
        return 0;

    default:
        return 0;
    }
}

static int start_advertising(void)
{
    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)PPT_DEVICE_NAME;
    fields.name_len = strlen(PPT_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.appearance = PPT_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    if (rc == 0) s_state = PPT_HID_ADVERTISING;
    return rc;
}

static void on_reset(int reason)
{
    set_failed(reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0 && s_running) rc = start_advertising();
    if (rc != 0) set_failed(rc);
}

static void hidd_event(void *arg, esp_event_base_t base, int32_t id,
                       void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;
    if (id == ESP_HIDD_DISCONNECT_EVENT) {
        s_authenticated = false;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static esp_err_t send_report(uint8_t modifier, uint8_t keycode)
{
    if (!s_hid_dev || !s_authenticated) return ESP_ERR_INVALID_STATE;

    uint8_t report[8] = { 0 };
    report[0] = modifier;
    report[2] = keycode;
    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0,
                                            PPT_REPORT_ID_KEYBOARD,
                                            report, sizeof(report));
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(PPT_KEY_HOLD_MS));
    memset(report, 0, sizeof(report));
    return esp_hidd_dev_input_set(s_hid_dev, 0, PPT_REPORT_ID_KEYBOARD,
                                  report, sizeof(report));
}

static void send_command(ppt_hid_command_t command)
{
    esp_err_t err = ESP_OK;
    switch (command) {
    case PPT_HID_COMMAND_PREVIOUS:
        err = send_report(0, HID_KEY_LEFT_ARROW);
        break;
    case PPT_HID_COMMAND_NEXT:
        err = send_report(0, HID_KEY_RIGHT_ARROW);
        break;
    case PPT_HID_COMMAND_ESCAPE:
        err = send_report(0, HID_KEY_ESCAPE);
        break;
    case PPT_HID_COMMAND_START:
        err = send_report(0, HID_KEY_F5);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(PPT_COMBO_GAP_MS));
            err = send_report(HID_MOD_L_GUI | HID_MOD_L_SHIFT, HID_KEY_RETURN);
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(PPT_COMBO_GAP_MS));
            err = send_report(HID_MOD_L_GUI | HID_MOD_L_ALT, HID_KEY_P);
        }
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "HID command %d failed: %s", command,
                 esp_err_to_name(err));
    }
}

static void command_task(void *arg)
{
    (void)arg;
    ppt_hid_command_t command;
    while (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
        send_command(command);
    }
}

esp_err_t ppt_hid_start(void)
{
    if (s_nimble_initialized) return ESP_ERR_INVALID_STATE;

    s_error = 0;
    s_state = PPT_HID_STARTING;
    s_running = true;
    s_authenticated = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) goto fail;

    s_command_queue = xQueueCreate(PPT_COMMAND_QUEUE_LENGTH,
                                   sizeof(ppt_hid_command_t));
    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_command_queue || !s_host_stopped) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = nimble_port_init();
    if (err != ESP_OK) goto fail;
    s_nimble_initialized = true;

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    err = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE,
                            hidd_event, &s_hid_dev);
    if (err != ESP_OK) goto fail;

    if (ble_svc_gap_device_name_set(PPT_DEVICE_NAME) != 0 ||
        ble_svc_gap_device_appearance_set(PPT_APPEARANCE_KEYBOARD) != 0) {
        err = ESP_FAIL;
        goto fail;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    if (xTaskCreate(command_task, "ppt_keys", 3072, NULL, 5,
                    &s_command_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_host_started = true;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "PPT HID starting as %s", PPT_DEVICE_NAME);
    return ESP_OK;

fail:
    set_failed(err);
    ppt_hid_stop();
    set_failed(err);
    return err;
}

void ppt_hid_stop(void)
{
    s_running = false;

    if (s_command_task) {
        vTaskDelete(s_command_task);
        s_command_task = NULL;
    }
    if (s_command_queue) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
    }

    if (s_nimble_initialized) {
        ble_gap_adv_stop();
        uint16_t handle = s_conn_handle;
        if (handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
            for (int waited = 0;
                 waited < PPT_DISCONNECT_WAIT_MS &&
                 s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
                 waited += 20) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        if (s_hid_dev) {
            esp_hidd_dev_deinit(s_hid_dev);
            s_hid_dev = NULL;
        }

        int rc = 0;
        if (s_host_started) rc = nimble_port_stop();
        if (rc == 0 && s_host_started && s_host_stopped) {
            if (xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(2500)) != pdTRUE) {
                rc = BLE_HS_ETIMEOUT;
            }
        }
        if (rc == 0) {
            nimble_port_deinit();
            s_nimble_initialized = false;
            s_host_started = false;
        } else if (rc != 0) {
            ESP_LOGE(TAG, "NimBLE stop failed: %d", rc);
        }
    }

    if (s_host_stopped && !s_nimble_initialized) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_hid_dev = NULL;
    s_authenticated = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (!s_nimble_initialized) s_state = PPT_HID_OFF;
}

bool ppt_hid_send(ppt_hid_command_t command)
{
    if (s_state != PPT_HID_READY || !s_authenticated || !s_command_queue) {
        return false;
    }
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE;
}

ppt_hid_state_t ppt_hid_state(void)
{
    return s_state;
}

int ppt_hid_error(void)
{
    return s_error;
}
