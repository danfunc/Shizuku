#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define SERVICE_UUID 0xFFF0
#define VERSION_UUID 0xFFF1
#define THROUGHPUT_UUID 0xFFF2
#define MAX_NOTIFY_VALUE 244

static const char *TAG = "idf_ble_ref";
static const char *DEVICE_NAME = "Shizuku-C6-IDF-Ref";
static volatile uint16_t g_connection = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_throughput_handle;
static volatile bool g_subscribed;
static volatile bool g_notify_enabled;
static volatile uint32_t g_sequence;

static int version_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt,
                          void *) {
    static const char version[] = "idf-ref-v1";
    if (!ctxt || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, version, sizeof(version) - 1u) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int throughput_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt,
                             void *) {
    if (!ctxt) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t command = 0xff;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1u ||
            os_mbuf_copydata(ctxt->om, 0, sizeof(command), &command) != 0 ||
            command > 1u)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        g_notify_enabled = command == 1u;
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || !g_notify_enabled)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t payload[MAX_NOTIFY_VALUE];
    uint16_t length = 4u;
    if (g_connection != BLE_HS_CONN_HANDLE_NONE) {
        const uint16_t mtu = ble_att_mtu(g_connection);
        if (mtu > 3u) {
            length = (uint16_t)(mtu - 3u);
            if (length > sizeof(payload)) length = sizeof(payload);
        }
    }
    if (length < 4u) return BLE_ATT_ERR_UNLIKELY;
    const uint32_t sequence = g_sequence;
    payload[0] = (uint8_t)sequence;
    payload[1] = (uint8_t)(sequence >> 8);
    payload[2] = (uint8_t)(sequence >> 16);
    payload[3] = (uint8_t)(sequence >> 24);
    for (uint16_t i = 4u; i < length; ++i)
        payload[i] = (uint8_t)(sequence + i);
    return os_mbuf_append(ctxt->om, payload, length) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def g_characteristics[] = {
    {.uuid = BLE_UUID16_DECLARE(VERSION_UUID),
     .access_cb = version_access,
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = BLE_UUID16_DECLARE(THROUGHPUT_UUID),
     .access_cb = throughput_access,
     .val_handle = &g_throughput_handle,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY},
    {0},
};

static const struct ble_gatt_svc_def g_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),
     .characteristics = g_characteristics},
    {0},
};

static void start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_connection = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected handle=%u", g_connection);
        } else {
            ESP_LOGI(TAG, "connect failed status=%d", event->connect.status);
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        g_connection = BLE_HS_CONN_HANDLE_NONE;
        g_subscribed = false;
        g_notify_enabled = false;
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == g_throughput_handle)
            g_subscribed = event->subscribe.cur_notify != 0;
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete reason=%d", event->adv_complete.reason);
        start_advertising();
        return 0;
    default:
        return 0;
    }
}

static void start_advertising(void) {
    uint8_t address_type = 0;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = (uint8_t)strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address type rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    ESP_LOGI(TAG, "advertising name=%s rc=%d", DEVICE_NAME, rc);
}

static void on_sync(void) {
    const int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "identity address rc=%d", rc);
        return;
    }
    start_advertising();
}
static void on_reset(int reason) { ESP_LOGE(TAG, "host reset reason=%d", reason); }

static void host_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void notify_task(void *) {
    for (;;) {
        if (g_notify_enabled && g_subscribed &&
            g_connection != BLE_HS_CONN_HANDLE_NONE && g_throughput_handle != 0u) {
            const int rc = ble_gatts_notify_custom(g_connection,
                                                    g_throughput_handle, NULL);
            if (rc == 0) ++g_sequence;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    const gpio_config_t gpio = {
        .pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_14, 1));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_3, 0));

    const esp_err_t nvs_rc = nvs_flash_init();
    if (nvs_rc != ESP_OK) {
        ESP_LOGE(TAG, "nvs init rc=%s", esp_err_to_name(nvs_rc));
        return;
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEVICE_NAME));
    ESP_ERROR_CHECK(ble_gatts_count_cfg(g_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(g_services));
    nimble_port_freertos_init(host_task);
    if (xTaskCreate(notify_task, "ref_notify", 3072, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "notify task creation failed");
}
