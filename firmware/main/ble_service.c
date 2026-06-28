#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "ble_service.h"
#include "board_config.h"

static const char *TAG = "ble_svc";

// ---- 128-bit UUIDs (little-endian) ----
// Base: 8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
    0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_chr_audio_tx = BLE_UUID128_INIT(
    0x01, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
    0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_chr_state_tx = BLE_UUID128_INIT(
    0x02, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
    0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);
static const ble_uuid128_t s_chr_control_rx = BLE_UUID128_INIT(
    0x03, 0x51, 0xfc, 0xea, 0x3c, 0x3a, 0xf7, 0x88,
    0x23, 0x4b, 0x6f, 0x6e, 0x84, 0x0b, 0x2f, 0x8f);

// ---- State ----
static bool s_connected = false;
static uint16_t s_conn_handle = 0xffff;
static uint16_t s_audio_attr_handle = 0;
static uint16_t s_state_attr_handle = 0;
static char s_device_name[20] = {0};
static ble_control_cb_t s_control_cb = NULL;
static ble_connect_cb_t s_connect_cb = NULL;

// ---- Forward declarations ----
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static int ble_svc_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

// ---- GATT Service Definition ----
// Forward declaration
static void nimble_host_task(void *param);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {{
            // audio_tx (notify)
            .uuid = &s_chr_audio_tx.u,
            .access_cb = ble_svc_access_cb,
            .flags = BLE_GATT_CHR_F_NOTIFY,
            .arg = (void *)1,
        }, {
            // state_tx (notify)
            .uuid = &s_chr_state_tx.u,
            .access_cb = ble_svc_access_cb,
            .flags = BLE_GATT_CHR_F_NOTIFY,
            .arg = (void *)2,
        }, {
            // control_rx (write without response)
            .uuid = &s_chr_control_rx.u,
            .access_cb = ble_svc_access_cb,
            .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            .arg = (void *)3,
        }, {
            0, // terminator
        }},
    }, {
        0, // no more services
    },
};

// ---- GATT Access Callback ----
static int ble_svc_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;
    uintptr_t chr_id = (uintptr_t)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        // All three characteristics return success with no data
        rc = 0;
        break;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (chr_id == 3 && s_control_cb) {
            // control_rx: data from Windows app
            const uint8_t *data = ctxt->om->om_data;
            size_t len = ctxt->om->om_len;
            char *json = malloc(len + 1);
            if (json) {
                memcpy(json, data, len);
                json[len] = '\0';
                s_control_cb(json);
                free(json);
            }
        }
        rc = 0;
        break;

    default:
        rc = BLE_ATT_ERR_UNLIKELY;
        break;
    }
    return rc;
}

// ---- Start advertising ----
static void start_advertising(void)
{
    int rc;

    // Stop any existing advertising first (prevents BLE_HS_EBUSY on reconnect)
    ble_gap_adv_stop();

    // Set advertising fields (include device name + service UUID)
    struct ble_hs_adv_fields adv_fields;
    memset(&adv_fields, 0, sizeof(adv_fields));

    // Include device name in advertising packet
    adv_fields.name = (uint8_t *)ble_svc_gap_device_name();
    adv_fields.name_len = strlen(ble_svc_gap_device_name());
    adv_fields.name_is_complete = 1;

    // Include our 128-bit service UUID in advertising packet
    adv_fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    // Set TX power level
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = -3; // +3 dBm

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed: %d", rc);
        return;
    }

    // Start advertising
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising started as %s", s_device_name);
    } else {
        ESP_LOGE(TAG, "advertising start failed: %d", rc);
    }
}

// ---- GAP Event Callback ----
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "connected, handle=%d", s_conn_handle);
            if (s_connect_cb) s_connect_cb(true);

            // Request connection parameters with 20s supervision timeout
            // (default is ~2s, too short for audio streaming under load).
            // Units: interval=1.25ms, supervision_timeout=10ms
            struct ble_gap_upd_params params = {
                .itvl_min = 12,
                .itvl_max = 24,
                .latency = 0,
                .supervision_timeout = 2000,  // 2000 * 10ms = 20s
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn_handle, &params);
        } else {
            // Connection failed; restart advertising
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        int r = event->disconnect.reason;
        const char *reason_str = "";
        if (r == BLE_HS_ETIMEOUT) reason_str = "supervision_timeout";
        else if (r == 0x16) reason_str = "local_host_term";
        else if (r == BLE_HS_EREJECT) reason_str = "remote_reject";
        ESP_LOGI(TAG, "disconnected, reason=%d %s", r, reason_str);
        s_connected = false;
        s_conn_handle = 0xffff;
        if (s_connect_cb) s_connect_cb(false);
        start_advertising();
        break;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete, restarting...");
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe handle=%d cur=%d prev=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify, event->subscribe.prev_notify);
        break;

    default:
        break;
    }
    return 0;
}

// ---- NimBLE sync callback (called when host syncs with controller) ----
static void ble_sync_cb(void)
{
    // GATT handles are now assigned; look up notification handles
    int rc;
    rc = ble_gatts_find_chr(&s_svc_uuid.u, &s_chr_audio_tx.u, NULL, &s_audio_attr_handle);
    if (rc != 0) ESP_LOGW(TAG, "audio_tx handle lookup: %d", rc);
    rc = ble_gatts_find_chr(&s_svc_uuid.u, &s_chr_state_tx.u, NULL, &s_state_attr_handle);
    if (rc != 0) ESP_LOGW(TAG, "state_tx handle lookup: %d", rc);
    ESP_LOGI(TAG, "handles: audio=%d state=%d", s_audio_attr_handle, s_state_attr_handle);
    ESP_LOGI(TAG, "BLE host synced with controller");
    start_advertising();
}

// ---- NimBLE reset callback ----
static void ble_reset_cb(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

// ---- Build device name VS-XXXX from MAC ----
static void build_device_name(const char *prefix)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_device_name, sizeof(s_device_name), "%s%02X%02X",
             prefix, mac[4], mac[5]);
}

// ---- NimBLE host task (runs the NimBLE stack event loop) ----
static void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---- Public API ----

esp_err_t ble_service_init(const char *device_name_prefix)
{
    ESP_RETURN_ON_FALSE(device_name_prefix, ESP_ERR_INVALID_ARG, TAG, "prefix required");

    build_device_name(device_name_prefix);

    // Initialize NimBLE stack
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return ESP_FAIL;
    }

    // Set up host callbacks
    ble_hs_cfg.reset_cb = ble_reset_cb;
    ble_hs_cfg.sync_cb = ble_sync_cb;

    // Register GATT services (defined as static table above)
    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    // Set device name
    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "set device name failed: %d", rc);
    }

    // Start NimBLE host task (runs event loop, never returns)
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "NimBLE service initialized: %s", s_device_name);
    return ESP_OK;
}

void ble_service_set_control_callback(ble_control_cb_t cb)
{
    s_control_cb = cb;
}

void ble_service_set_connect_callback(ble_connect_cb_t cb)
{
    s_connect_cb = cb;
}

esp_err_t ble_service_send_audio(const uint8_t *data, uint16_t len,
                                 uint32_t session_id, uint32_t seq, uint8_t flags)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "send audio: session=%u seq=%u flags=0x%x len=%u",
             session_id, seq, flags, len);

    // Build audio frame
    uint16_t frame_size = AUDIO_FRAME_HEADER_SIZE + len;
    uint8_t *frame = malloc(frame_size);
    if (!frame) return ESP_ERR_NO_MEM;

    audio_ble_frame_t *af = (audio_ble_frame_t *)frame;
    af->version = 1;
    af->type = AUDIO_FRAME_TYPE;
    af->header_len = AUDIO_FRAME_HEADER_SIZE;
    af->session_id = session_id;
    af->seq = seq;
    af->flags = flags;
    af->reserved = 0;
    af->payload_len = len;
    if (len > 0) memcpy(af->payload, data, len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_size);
    free(frame);

    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_audio_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify audio failed: %d", rc);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_service_send_state(const char *json)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_state_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify state failed: %d", rc);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool ble_service_is_connected(void)
{
    return s_connected;
}

const char *ble_service_device_name(void)
{
    return s_device_name;
}
