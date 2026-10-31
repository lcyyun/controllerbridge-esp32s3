#include "ns2_ble.h"

#include "ns2_config.h"
#include "ns2_gatt.h"
#include "ns2_input.h"
#include "ns2_feedback.h"
#include "ns2_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

extern "C" void ble_store_config_init(void);

namespace ns2 {
namespace {

const char *TAG = "ns2-ble";

constexpr size_t kMaxServices = 24;
constexpr size_t kMaxChars = 72;
constexpr uint32_t kConnectTimeoutMs = 30000;
constexpr uint64_t kConnectTimeoutUs = 30000000ULL;
constexpr uint64_t kInitAckTimeoutUs = 3500000ULL;
constexpr uint64_t kGattStageTimeoutUs = 8000000ULL;
constexpr uint64_t kCommandSpacingUs = 1200ULL;
constexpr uint64_t kConnectRetryBaseDelayUs = 1200000ULL;
constexpr uint64_t kReconnectAfterReadyDelayUs = 900000ULL;
constexpr uint16_t kCccdNotify = 0x0001;
constexpr uint16_t kFastConnIntervalMin = 0x000a;
constexpr uint16_t kFastConnIntervalMax = 0x000c;
constexpr uint16_t kFastConnLatency = 0;
constexpr uint16_t kFastConnSupervisionTimeout = 400;

enum class Stage : uint8_t {
    Idle,
    Scanning,
    Connecting,
    Disconnecting,
    Securing,
    DiscoverServices,
    DiscoverCharacteristics,
    SubscribeAck,
    Initializing,
    Pairing,
    PersistPairing,
    SubscribeInput,
    Ready,
};

struct ServiceSlot {
    uint16_t start = 0;
    uint16_t end = 0;
};

struct CharSlot {
    bool used = false;
    uint16_t value_handle = 0;
    uint16_t end_handle = 0;
    uint8_t properties = 0;
    uint8_t uuid_be[kUuid128Len] = {};
    GattRole role = GattRole::Other;
    bool subscribed = false;
};

struct Runtime {
    bool started = false;
    bool host_synced = false;
    uint8_t own_addr_type = 0;
    uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
    Stage stage = Stage::Idle;
    BleStats stats = {};
    ble_addr_t pending_addr = {};
    ServiceSlot services[kMaxServices] = {};
    CharSlot chars[kMaxChars] = {};
    size_t service_count = 0;
    size_t char_count = 0;
    size_t discover_service_index = 0;
    int subscribe_index = -1;
    uint8_t init_index = 0;
    uint16_t command_handle = 0;
    uint16_t rumble_handle = 0;
    uint16_t legacy_power_handle = 0;
    bool command_no_rsp = false;
    bool rumble_no_rsp = false;
    bool pending_subscribe_ack = false;
    bool pending_subscribe_input = false;
    bool pair_mode = false;
    bool manual_scan = false;
    bool stay_disconnected = false;
    bool target_saved_this_connection = false;
    bool pending_init_command = false;
    bool pending_service_discovery = false;
    bool pending_characteristic_discovery = false;
    bool pending_connect = false;
    bool write_in_flight = false;
    uint64_t write_started_us = 0;
    bool battery_read_in_flight = false;
    uint64_t battery_read_started_us = 0;
    uint64_t next_battery_read_us = 0;
    bool pairing_required = false;
    uint8_t pair_step = 0;
    bool pair_waiting_ack = false;
    uint8_t pair_a1[16] = {};
    uint8_t pair_a2[16] = {};
    uint8_t pair_ltk[16] = {};
    uint32_t pair_generation = 0;
    uint64_t connect_started_us = 0;
    uint64_t next_connect_allowed_us = 0;
    uint64_t scan_restart_due_us = 0;
    uint64_t init_command_sent_us = 0;
    uint64_t stage_started_us = 0;
    uint64_t next_gatt_action_us = 0;
    uint32_t ack_notify_logged = 0;
    uint32_t input_notify_logged = 0;
    uint32_t input_parse_failed_logged = 0;
    uint64_t notify_sample_us = 0;
    uint32_t notify_sample_count = 0;
    uint64_t notify_last_us = 0;
    uint8_t consecutive_connect_failures = 0;
};

Runtime s_ble;
enum class ControlAction : uint8_t { None, Scan, Pair, Reconnect, Disconnect, Forget, AutoOn, AutoOff };
std::atomic<ControlAction> s_control{ControlAction::None};
std::atomic<bool> s_save_pending{false};
std::atomic<uint32_t> s_save_generation{0};
std::atomic<uint32_t> s_saved_generation{0};
std::atomic<int> s_persist_error{0};
struct RumbleRequest { uint32_t source_epoch; uint16_t len; uint8_t data[128]; };
QueueHandle_t s_rumble_queue = nullptr;
ble_npl_event s_work_event;
portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
BleStats s_published_stats;
PairingRecord s_pair_record;
portMUX_TYPE s_pair_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> s_pair_save_pending{false};
std::atomic<uint32_t> s_pair_generation{0};
std::atomic<uint32_t> s_pair_saved_generation{0};
std::atomic<int> s_pair_write_error{0};
std::atomic<bool> s_delete_bond_pending{false};
std::atomic<bool> s_delete_bond_active{false};
std::atomic<bool> s_bond_cleanup_ready{false};
std::atomic<int> s_bond_write_error{0};
std::atomic<bool> s_forget_requested{false};
std::atomic<bool> s_forget_in_progress{false};
std::atomic<bool> s_forget_processed{false};
ble_store_read_fn *s_original_store_read = nullptr;

void start_scan();
void stop_scan();
void schedule_scan(uint64_t delay_us);
void disconnect_current(const char *reason);
int gap_event(ble_gap_event *event, void *arg);
int service_cb(uint16_t conn_handle, const ble_gatt_error *error, const ble_gatt_svc *service, void *arg);
int characteristic_cb(uint16_t conn_handle, const ble_gatt_error *error, const ble_gatt_chr *chr, void *arg);
int write_cb(uint16_t conn_handle, const ble_gatt_error *error, ble_gatt_attr *attr, void *arg);
int mtu_cb(uint16_t conn_handle, const ble_gatt_error *error, uint16_t mtu, void *arg);
int battery_read_cb(uint16_t conn_handle, const ble_gatt_error *error, ble_gatt_attr *attr, void *arg);
void send_current_init_command();
void process_ble_work();
void process_control(ControlAction action);
void publish_stats();
void request_save() { s_save_generation.fetch_add(1); s_save_pending.store(true); }
void start_pairing();
void send_pairing_step();
void handle_pairing_response(const uint8_t *data, size_t len);

PairingRecord pairing_record() {
    portENTER_CRITICAL(&s_pair_mux);
    const PairingRecord record = s_pair_record;
    portEXIT_CRITICAL(&s_pair_mux);
    return record;
}

bool pairing_matches(const PairingRecord &record, const ble_addr_t &addr) {
    return record.valid && (record.addr_type & 1) == (addr.type & 1) &&
           std::memcmp(record.addr, addr.val, 6) == 0;
}

void queue_pairing_record(const PairingRecord &record) {
    portENTER_CRITICAL(&s_pair_mux);
    s_pair_record = record;
    s_pair_generation.fetch_add(1);
    portEXIT_CRITICAL(&s_pair_mux);
    s_pair_save_pending.store(true);
}

void load_pairing_record() {
    nvs_handle_t handle;
    PairingRecord record;
    if (nvs_open("ns2_pair", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(record);
    const esp_err_t rc = nvs_get_blob(handle, "peer", &record, &size);
    nvs_close(handle);
    if (rc == ESP_OK && gatt_pairing_record_valid(record, size)) s_pair_record = record;
}

void persist_pairing_record() {
    if (!s_pair_save_pending.exchange(false)) return;
    portENTER_CRITICAL(&s_pair_mux);
    const PairingRecord record = s_pair_record;
    const uint32_t generation = s_pair_generation.load();
    portEXIT_CRITICAL(&s_pair_mux);
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("ns2_pair", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = record.valid ? nvs_set_blob(handle, "peer", &record, sizeof(record)) : nvs_erase_key(handle, "peer");
        if (rc == ESP_ERR_NVS_NOT_FOUND) rc = ESP_OK;
        if (rc == ESP_OK) rc = nvs_commit(handle);
        nvs_close(handle);
    }
    s_pair_write_error.store(rc);
    s_pair_saved_generation.store(generation);
}

esp_err_t erase_bond_namespace() {
    // ESP-IDF 5.3.3's pinned NimBLE ble_store_nvs.c declares this exact
    // namespace. The running host is disconnected and its store RAM has been
    // cleared before this call; never erase unrelated namespaces or the NVS
    // partition. Erasing the namespace also retries any previous SDK delete
    // that removed a RAM entry before failing to persist it.
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("nimble_bond", NVS_READWRITE, &handle);
    if (rc == ESP_OK) {
        rc = nvs_erase_all(handle);
        if (rc == ESP_OK) rc = nvs_commit(handle);
        nvs_close(handle);
    }
    return rc;
}

void request_offline_forget() {
    const esp_err_t rc = config_init();
    if (rc != ESP_OK) {
        s_persist_error.store(rc);
        return;
    }
    s_ble.stay_disconnected = true;
    s_ble.pending_connect = false;
    s_ble.manual_scan = false;
    s_ble.pair_mode = false;
    input_reset();
    queue_pairing_record(PairingRecord{});
    config_forget_saved_target();
    request_save();
    s_delete_bond_pending.store(true);
    s_bond_cleanup_ready.store(true);
    s_forget_processed.store(true);
}

int pairing_store_read(int obj_type, const ble_store_key *key, ble_store_value *value) {
    const PairingRecord record = pairing_record();
    if (obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC && key && value && key->sec.idx == 0 &&
        pairing_matches(record, key->sec.peer_addr)) {
        std::memset(value, 0, sizeof(*value));
        value->sec.peer_addr = key->sec.peer_addr;
        value->sec.key_size = 16;
        value->sec.ltk_present = 1;
        std::memcpy(value->sec.ltk, record.ltk, 16);
        return 0;
    }
    return s_original_store_read ? s_original_store_read(obj_type, key, value) : BLE_HS_ENOENT;
}

void uuid_to_be(const ble_uuid_t *uuid, uint8_t out[kUuid128Len]) {
    std::memset(out, 0, kUuid128Len);
    if (uuid == nullptr || uuid->type != BLE_UUID_TYPE_128) {
        return;
    }
    const ble_uuid128_t *uuid128 = reinterpret_cast<const ble_uuid128_t *>(uuid);
    for (size_t i = 0; i < kUuid128Len; ++i) {
        out[i] = uuid128->value[kUuid128Len - 1 - i];
    }
}

void update_ready_flags() {
    s_ble.stats.connected = s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE;
    s_ble.stats.gatt_ready = s_ble.stage == Stage::Ready;
    s_ble.stats.rumble_ready = s_ble.rumble_handle != 0;
    s_ble.stats.services = static_cast<uint32_t>(s_ble.service_count);
    s_ble.stats.characteristics = static_cast<uint32_t>(s_ble.char_count);
}

void update_conn_desc() {
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        s_ble.stats.conn_interval_units = 0;
        s_ble.stats.conn_interval_us = 0;
        s_ble.stats.conn_latency = 0;
        s_ble.stats.conn_supervision_timeout = 0;
        return;
    }
    ble_gap_conn_desc desc{};
    const int rc = ble_gap_conn_find(s_ble.conn_handle, &desc);
    if (rc != 0) {
        s_ble.stats.last_error = rc;
        return;
    }
    s_ble.stats.conn_interval_units = desc.conn_itvl;
    s_ble.stats.conn_interval_us = static_cast<uint32_t>(desc.conn_itvl) * 1250UL;
    s_ble.stats.conn_latency = desc.conn_latency;
    s_ble.stats.conn_supervision_timeout = desc.supervision_timeout;
    uint8_t tx_phy = 0;
    uint8_t rx_phy = 0;
    if (ble_gap_read_le_phy(s_ble.conn_handle, &tx_phy, &rx_phy) == 0) {
        s_ble.stats.tx_phy = tx_phy;
        s_ble.stats.rx_phy = rx_phy;
    }
}

void request_fast_link_params(const char *reason) {
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    const int phy_rc = ble_gap_set_prefered_le_phy(s_ble.conn_handle,
                                                   BLE_GAP_LE_PHY_2M_MASK,
                                                   BLE_GAP_LE_PHY_2M_MASK,
                                                   BLE_GAP_LE_PHY_CODED_ANY);
    s_ble.stats.phy_update_rc = phy_rc;
    if (phy_rc != 0) {
        s_ble.stats.last_error = phy_rc;
    }

    const int data_rc = ble_gap_set_data_len(s_ble.conn_handle, 251, 2120);
    s_ble.stats.data_len_update_rc = data_rc;
    if (data_rc != 0) {
        s_ble.stats.last_error = data_rc;
    }

    ESP_LOGW(TAG,
             "fast link params reason=%s phy2m_rc=%d data_len_rc=%d",
             reason ? reason : "unknown",
             phy_rc,
             data_rc);
}

void request_fast_connection_update(const char *reason) {
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ble_gap_upd_params params{};
    params.itvl_min = kFastConnIntervalMin;
    params.itvl_max = kFastConnIntervalMax;
    params.latency = kFastConnLatency;
    params.supervision_timeout = kFastConnSupervisionTimeout;
    params.min_ce_len = 0;
    params.max_ce_len = 0;
    s_ble.stats.conn_update_requests++;
    const int rc = ble_gap_update_params(s_ble.conn_handle, &params);
    s_ble.stats.conn_update_start_rc = rc;
    if (rc != 0) {
        s_ble.stats.last_error = rc;
    }
    ESP_LOGW(TAG,
             "fast conn update reason=%s interval=%u..%u latency=%u supervision=%u rc=%d",
             reason ? reason : "unknown",
             static_cast<unsigned>(params.itvl_min),
             static_cast<unsigned>(params.itvl_max),
             static_cast<unsigned>(params.latency),
             static_cast<unsigned>(params.supervision_timeout),
             rc);
}

void mark_stage(Stage stage) {
    s_ble.stage = stage;
    s_ble.stage_started_us = esp_timer_get_time();
    update_ready_flags();
}

void clear_gatt() {
    std::memset(s_ble.services, 0, sizeof(s_ble.services));
    std::memset(s_ble.chars, 0, sizeof(s_ble.chars));
    s_ble.service_count = 0;
    s_ble.char_count = 0;
    s_ble.discover_service_index = 0;
    s_ble.subscribe_index = -1;
    s_ble.init_index = 0;
    s_ble.command_handle = 0;
    s_ble.rumble_handle = 0;
    s_ble.legacy_power_handle = 0;
    s_ble.command_no_rsp = false;
    s_ble.rumble_no_rsp = false;
    s_ble.pending_subscribe_ack = false;
    s_ble.pending_subscribe_input = false;
    s_ble.target_saved_this_connection = false;
    s_ble.pending_init_command = false;
    s_ble.pending_service_discovery = false;
    s_ble.pending_characteristic_discovery = false;
    s_ble.write_in_flight = false;
    s_ble.write_started_us = 0;
    s_ble.battery_read_in_flight = false;
    s_ble.battery_read_started_us = 0;
    s_ble.next_battery_read_us = 0;
    s_ble.pair_waiting_ack = false;
    s_ble.stats.pairing_active = false;
    s_ble.init_command_sent_us = 0;
    s_ble.next_gatt_action_us = 0;
    s_ble.ack_notify_logged = 0;
    s_ble.input_notify_logged = 0;
    s_ble.input_parse_failed_logged = 0;
    s_ble.notify_sample_us = 0;
    s_ble.notify_sample_count = 0;
    s_ble.notify_last_us = 0;
    s_ble.stats.notify_hz = 0;
    s_ble.stats.notify_last_gap_us = 0;
    s_ble.stats.notify_max_gap_us = 0;
    s_ble.stats.input_subscribed = false;
    update_ready_flags();
}

void note_input_notify_rate() {
    const uint64_t now = esp_timer_get_time();
    s_ble.stats.notify_count++;
    if (s_ble.notify_last_us != 0) {
        const uint32_t gap = static_cast<uint32_t>(now - s_ble.notify_last_us);
        s_ble.stats.notify_last_gap_us = gap;
        if (gap > s_ble.stats.notify_max_gap_us) {
            s_ble.stats.notify_max_gap_us = gap;
        }
    }
    s_ble.notify_last_us = now;
    if (s_ble.notify_sample_us == 0) {
        s_ble.notify_sample_us = now;
        s_ble.notify_sample_count = s_ble.stats.notify_count;
        return;
    }
    const uint64_t elapsed = now - s_ble.notify_sample_us;
    if (elapsed >= 1000000ULL) {
        const uint32_t delta = s_ble.stats.notify_count - s_ble.notify_sample_count;
        s_ble.stats.notify_hz = static_cast<uint32_t>((static_cast<uint64_t>(delta) * 1000000ULL) / elapsed);
        s_ble.notify_sample_us = now;
        s_ble.notify_sample_count = s_ble.stats.notify_count;
        s_ble.stats.notify_max_gap_us = 0;
    }
}

uint64_t connect_retry_delay_us() {
    return ble_retry_delay_us(s_ble.consecutive_connect_failures);
}

CharSlot *find_char_by_handle(uint16_t value_handle) {
    for (size_t i = 0; i < s_ble.char_count; ++i) {
        if (s_ble.chars[i].used && s_ble.chars[i].value_handle == value_handle) {
            return &s_ble.chars[i];
        }
    }
    return nullptr;
}

bool has_role(GattRole role) {
    for (size_t i = 0; i < s_ble.char_count; ++i) {
        if (s_ble.chars[i].used && s_ble.chars[i].role == role) {
            return true;
        }
    }
    return false;
}

bool has_minimum_ns2_gatt() {
    return s_ble.command_handle != 0 &&
           s_ble.rumble_handle != 0 && // NS2Pro-specific UUID, not Joy-Con
           has_role(GattRole::AckNotify) &&
           has_role(GattRole::InputNotify);
}

void start_service_discovery() {
    clear_gatt();
    mark_stage(Stage::DiscoverServices);
    const int rc = ble_gattc_disc_all_svcs(s_ble.conn_handle, service_cb, nullptr);
    if (rc != 0) {
        s_ble.stats.last_error = rc;
        ESP_LOGE(TAG, "service discovery start failed rc=%d", rc);
        disconnect_current("service discovery start failed");
    }
}

void discover_next_characteristics() {
    while (s_ble.discover_service_index < s_ble.service_count) {
        ServiceSlot &svc = s_ble.services[s_ble.discover_service_index];
        if (svc.end <= svc.start) {
            s_ble.discover_service_index++;
            continue;
        }
        mark_stage(Stage::DiscoverCharacteristics);
        const int rc = ble_gattc_disc_all_chrs(s_ble.conn_handle,
                                               static_cast<uint16_t>(svc.start + 1),
                                               svc.end,
                                               characteristic_cb,
                                               nullptr);
        if (rc == 0) {
            return;
        }
        ESP_LOGW(TAG, "characteristic discovery start failed rc=%d svc=%u", rc, static_cast<unsigned>(s_ble.discover_service_index));
        s_ble.discover_service_index++;
    }
    if (!has_minimum_ns2_gatt()) {
        disconnect_current("NS2Pro GATT services missing");
        return;
    }
    mark_stage(Stage::SubscribeAck);
    s_ble.subscribe_index = -1;
    s_ble.pending_subscribe_ack = true;
}

void subscribe_role(GattRole role) {
    for (int i = s_ble.subscribe_index + 1; i < static_cast<int>(s_ble.char_count); ++i) {
        CharSlot &slot = s_ble.chars[i];
        if (!slot.used || slot.role != role || slot.subscribed) {
            continue;
        }
        if (role == GattRole::InputNotify && !gatt_uuid128_equals(slot.uuid_be, gatt_fd2_uuid())) {
            continue;
        }
        s_ble.subscribe_index = i;
        const uint8_t value[2] = {static_cast<uint8_t>(kCccdNotify & 0xff), static_cast<uint8_t>(kCccdNotify >> 8)};
        const uint16_t cccd = static_cast<uint16_t>(slot.value_handle + 1);
        s_ble.write_in_flight = true;
        s_ble.write_started_us = esp_timer_get_time();
        const int rc = ble_gattc_write_flat(s_ble.conn_handle, cccd, value, sizeof(value), write_cb, reinterpret_cast<void *>(static_cast<uintptr_t>(role)));
        ESP_LOGI(TAG, "subscribe role=%s value=0x%04x cccd=0x%04x rc=%d", gatt_role_name(role), slot.value_handle, cccd, rc);
        if (rc == 0) {
            return;
        }
        s_ble.stats.last_error = rc;
        s_ble.write_in_flight = false;
        disconnect_current("CCCD write rejected");
        return;
    }

    if (role == GattRole::AckNotify) {
        mark_stage(Stage::Initializing);
        s_ble.init_index = 0;
        ESP_LOGI(TAG, "ack subscription complete; starting controller init");
        s_ble.pending_init_command = true;
    } else {
        bool subscribed = false;
        for (size_t i = 0; i < s_ble.char_count; ++i) {
            if (s_ble.chars[i].role == GattRole::InputNotify && s_ble.chars[i].subscribed) subscribed = true;
        }
        if (!subscribed) {
            disconnect_current("no subscribed input characteristic");
            return;
        }
        mark_stage(Stage::Ready);
        s_ble.stats.input_subscribed = true;
        ESP_LOGI(TAG, "input subscription complete; FD2 notifications armed");
        request_fast_connection_update("input_subscribed");
    }
}

void send_current_init_command() {
    if (s_ble.init_index >= gatt_init_command_count()) {
        mark_stage(Stage::SubscribeInput);
        s_ble.subscribe_index = -1;
        s_ble.pending_subscribe_input = true;
        return;
    }
    if (s_ble.command_handle == 0) {
        disconnect_current("missing command characteristic");
        return;
    }
    // NS2 uses a proprietary address/key/challenge exchange, not ordinary
    // SMP pairing. On saved-key reconnect do not send the finalize command.
    if (s_ble.init_index == 3) {
        if (s_ble.pairing_required) start_pairing();
        else { s_ble.init_index = 4; s_ble.pending_init_command = true; }
        return;
    }
    const InitCommand &cmd = gatt_init_command(s_ble.init_index);
    s_ble.write_in_flight = !s_ble.command_no_rsp;
    s_ble.write_started_us = esp_timer_get_time();
    const int rc = s_ble.command_no_rsp
        ? ble_gattc_write_no_rsp_flat(s_ble.conn_handle, s_ble.command_handle, cmd.data, cmd.len)
        : ble_gattc_write_flat(s_ble.conn_handle, s_ble.command_handle, cmd.data, cmd.len, write_cb, nullptr);
    ESP_LOGI(TAG, "init %u/%u %s handle=0x%04x rc=%d", s_ble.init_index + 1, gatt_init_command_count(), cmd.name, s_ble.command_handle, rc);
    if (rc == 0) {
        s_ble.init_command_sent_us = esp_timer_get_time();
    }
    if (rc != 0) {
        s_ble.write_in_flight = false;
        s_ble.stats.last_error = rc;
        disconnect_current("initialization command rejected");
    }
}

void start_pairing() {
    esp_fill_random(s_ble.pair_a1, sizeof(s_ble.pair_a1));
    esp_fill_random(s_ble.pair_a2, sizeof(s_ble.pair_a2));
    std::memset(s_ble.pair_ltk, 0, sizeof(s_ble.pair_ltk));
    s_ble.pair_step = kPairAddress;
    s_ble.pair_waiting_ack = false;
    s_ble.stats.pairing_active = true;
    mark_stage(Stage::Pairing);
}

void pairing_failed(const char *reason) {
    s_ble.stats.pairing_failures++;
    s_ble.stats.pairing_active = false;
    s_ble.pair_waiting_ack = false;
    std::memset(s_ble.pair_a1, 0, sizeof(s_ble.pair_a1));
    std::memset(s_ble.pair_a2, 0, sizeof(s_ble.pair_a2));
    std::memset(s_ble.pair_ltk, 0, sizeof(s_ble.pair_ltk));
    disconnect_current(reason);
}

void send_pairing_step() {
    uint8_t command[32] = {};
    uint8_t address[12] = {};
    const uint8_t *value = nullptr;
    size_t value_len = 0;
    if (s_ble.pair_step == kPairAddress) {
        std::memcpy(address, s_ble.stats.local_addr, 6);
        std::memcpy(address + 6, s_ble.stats.local_addr, 6);
        address[6] ^= 1;
        value = address; value_len = sizeof(address);
    } else if (s_ble.pair_step == kPairKey) {
        value = s_ble.pair_a1; value_len = sizeof(s_ble.pair_a1);
    } else if (s_ble.pair_step == kPairChallenge) {
        value = s_ble.pair_a2; value_len = sizeof(s_ble.pair_a2);
    }
    const size_t len = gatt_build_pair_command(command, sizeof(command), s_ble.pair_step, value, value_len);
    if (!len) { pairing_failed("invalid pairing step"); return; }
    s_ble.write_in_flight = !s_ble.command_no_rsp;
    s_ble.write_started_us = esp_timer_get_time();
    const int rc = s_ble.command_no_rsp
        ? ble_gattc_write_no_rsp_flat(s_ble.conn_handle, s_ble.command_handle, command, len)
        : ble_gattc_write_flat(s_ble.conn_handle, s_ble.command_handle, command, len, write_cb, nullptr);
    if (rc) { s_ble.write_in_flight = false; s_ble.stats.last_error = rc; pairing_failed("pairing write failed"); return; }
    s_ble.pair_waiting_ack = true;
    s_ble.init_command_sent_us = esp_timer_get_time();
}

void handle_pairing_response(const uint8_t *data, size_t len) {
    if (!s_ble.pair_waiting_ack || len < 4 || data[0] != 0x15 || data[3] != s_ble.pair_step) return;
    const uint8_t *payload = nullptr;
    if (!gatt_parse_pair_response(data, len, s_ble.pair_step, &payload)) {
        pairing_failed("invalid pairing ACK"); return;
    }
    switch (s_ble.pair_step) {
    case kPairAddress: s_ble.pair_step = kPairKey; break;
    case kPairKey:
        gatt_derive_pair_key(s_ble.pair_a1, payload + 1, s_ble.pair_ltk);
        s_ble.pair_step = kPairChallenge;
        break;
    case kPairChallenge: {
        uint8_t key_be[16], challenge_be[16], expected[16];
        gatt_pair_challenge_inputs(s_ble.pair_ltk, s_ble.pair_a2, key_be, challenge_be);
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        int rc = mbedtls_aes_setkey_enc(&aes, key_be, 128);
        if (!rc) rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, challenge_be, expected);
        mbedtls_aes_free(&aes);
        if (rc || !gatt_pair_challenge_matches(expected, payload + 1)) {
            pairing_failed("pairing challenge rejected"); return;
        }
        s_ble.pair_step = kPairFinalize;
        break;
    }
    case kPairFinalize: {
        PairingRecord record;
        record.valid = 1; record.addr_type = s_ble.pending_addr.type;
        std::memcpy(record.addr, s_ble.pending_addr.val, 6);
        std::memcpy(record.ltk, s_ble.pair_ltk, 16);
        if (!gatt_pairing_record_valid(record, sizeof(record))) {
            pairing_failed("invalid pairing record"); return;
        }
        queue_pairing_record(record);
        s_ble.pair_generation = s_pair_generation.load();
        mark_stage(Stage::PersistPairing);
        break;
    }
    default: pairing_failed("unexpected pairing step"); return;
    }
    s_ble.pair_waiting_ack = false;
    s_ble.init_command_sent_us = 0;
    s_ble.next_gatt_action_us = esp_timer_get_time() + kCommandSpacingUs;
}

int service_cb(uint16_t conn_handle, const ble_gatt_error *error, const ble_gatt_svc *service, void *arg) {
    if (conn_handle != s_ble.conn_handle) return 0;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        s_ble.discover_service_index = 0;
        s_ble.pending_characteristic_discovery = true;
        return 0;
    }
    if (error->status != 0) {
        s_ble.stats.last_error = error->status;
        ESP_LOGW(TAG, "service discovery error=%d", error->status);
        disconnect_current("service discovery failed");
        return 0;
    }
    if (service != nullptr && s_ble.service_count < kMaxServices) {
        s_ble.services[s_ble.service_count++] = ServiceSlot{service->start_handle, service->end_handle};
    }
    return 0;
}

int characteristic_cb(uint16_t conn_handle, const ble_gatt_error *error, const ble_gatt_chr *chr, void *arg) {
    if (conn_handle != s_ble.conn_handle) return 0;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (has_minimum_ns2_gatt()) {
            ESP_LOGI(TAG, "minimum NS2 GATT discovered; skipping remaining services");
            mark_stage(Stage::SubscribeAck);
            s_ble.subscribe_index = -1;
            s_ble.pending_subscribe_ack = true;
            return 0;
        }
        s_ble.discover_service_index++;
        s_ble.pending_characteristic_discovery = true;
        return 0;
    }
    if (error->status != 0) {
        s_ble.stats.last_error = error->status;
        s_ble.discover_service_index++;
        disconnect_current("characteristic discovery failed");
        return 0;
    }
    if (chr == nullptr || s_ble.char_count >= kMaxChars) {
        return 0;
    }
    CharSlot &slot = s_ble.chars[s_ble.char_count++];
    slot.used = true;
    slot.value_handle = chr->val_handle;
    slot.end_handle = chr->val_handle;
    slot.properties = chr->properties;
    uuid_to_be(&chr->uuid.u, slot.uuid_be);
    slot.role = gatt_classify_uuid(slot.uuid_be);
    if (gatt_uuid128_equals(slot.uuid_be, gatt_legacy_uuid()) && (slot.properties & BLE_GATT_CHR_F_READ)) {
        s_ble.legacy_power_handle = slot.value_handle;
    }
    if (slot.role == GattRole::Command) {
        s_ble.command_handle = slot.value_handle;
        s_ble.command_no_rsp = (slot.properties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0;
    } else if (slot.role == GattRole::Rumble) {
        s_ble.rumble_handle = slot.value_handle;
        s_ble.rumble_no_rsp = (slot.properties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0;
    }
    char uuid[48];
    gatt_format_uuid(slot.uuid_be, uuid, sizeof(uuid));
    ESP_LOGI(TAG, "char value=0x%04x props=0x%02x role=%s uuid=%s", slot.value_handle, slot.properties, gatt_role_name(slot.role), uuid);
    return 0;
}

int write_cb(uint16_t conn_handle, const ble_gatt_error *error, ble_gatt_attr *attr, void *arg) {
    if (conn_handle != s_ble.conn_handle) return 0;
    s_ble.write_in_flight = false;
    (void)attr;
    const GattRole role = static_cast<GattRole>(reinterpret_cast<uintptr_t>(arg));
    if (error != nullptr && error->status != 0) {
        s_ble.stats.last_error = error->status;
        ESP_LOGW(TAG, "write complete role=%s status=%d", gatt_role_name(role), error->status);
        if (role == GattRole::AckNotify || role == GattRole::InputNotify ||
            s_ble.stage == Stage::Initializing || s_ble.stage == Stage::Pairing) {
            disconnect_current("GATT write failed");
        }
        return 0;
    }
    if (s_ble.stage == Stage::SubscribeAck && role == GattRole::AckNotify) {
        if (s_ble.subscribe_index >= 0 && s_ble.subscribe_index < static_cast<int>(s_ble.char_count)) {
            s_ble.chars[s_ble.subscribe_index].subscribed = true;
        }
        s_ble.pending_subscribe_ack = true;
    } else if (s_ble.stage == Stage::SubscribeInput && role == GattRole::InputNotify) {
        if (s_ble.subscribe_index >= 0 && s_ble.subscribe_index < static_cast<int>(s_ble.char_count)) {
            s_ble.chars[s_ble.subscribe_index].subscribed = true;
        }
        s_ble.pending_subscribe_input = true;
    }
    update_ready_flags();
    return 0;
}

int mtu_cb(uint16_t conn_handle, const ble_gatt_error *error, uint16_t mtu, void *arg) {
    if (conn_handle != s_ble.conn_handle) return 0;
    (void)arg;
    ESP_LOGI(TAG, "MTU exchange status=%d mtu=%u", error ? error->status : 0, mtu);
    s_ble.pending_service_discovery = true;
    return 0;
}

int battery_read_cb(uint16_t conn_handle, const ble_gatt_error *error, ble_gatt_attr *attr, void *arg) {
    (void)arg;
    if (conn_handle != s_ble.conn_handle) return 0;
    s_ble.battery_read_in_flight = false;
    s_ble.next_battery_read_us = esp_timer_get_time() + 10000000ULL;
    uint8_t data[11] = {};
    if (!error || error->status != 0 || !attr || !attr->om || OS_MBUF_PKTLEN(attr->om) < sizeof(data) ||
        os_mbuf_copydata(attr->om, 0, sizeof(data), data) != 0) {
        input_note_legacy_power(nullptr, 0);
        return 0;
    }
    input_note_legacy_power(data, sizeof(data));
    return 0;
}

void handle_notification(uint16_t attr_handle, os_mbuf *om) {
    uint8_t data[128];
    const uint16_t len = std::min<uint16_t>(OS_MBUF_PKTLEN(om), sizeof(data));
    os_mbuf_copydata(om, 0, len, data);
    CharSlot *slot = find_char_by_handle(attr_handle);
    if (slot == nullptr) {
        ESP_LOGW(TAG, "notify unknown handle=0x%04x len=%u", attr_handle, len);
        return;
    }
    if (slot->role == GattRole::AckNotify && s_ble.stage == Stage::Pairing) {
        handle_pairing_response(data, len);
        return;
    }
    if (slot->role == GattRole::AckNotify && s_ble.stage == Stage::Initializing) {
        if (s_ble.init_command_sent_us == 0 || s_ble.init_index >= gatt_init_command_count()) return;
        const InitCommand &cmd = gatt_init_command(s_ble.init_index);
        if (len < 4 || data[0] != cmd.data[0] || data[1] != 0x01 || data[3] != cmd.data[3]) return;
        if (s_ble.ack_notify_logged < 20) {
            ESP_LOGI(TAG, "ack notify handle=0x%04x len=%u init_index=%u", attr_handle, len, s_ble.init_index);
            s_ble.ack_notify_logged++;
        }
        s_ble.init_index++;
        s_ble.init_command_sent_us = 0;
        s_ble.pending_init_command = true;
        s_ble.next_gatt_action_us = esp_timer_get_time() + kCommandSpacingUs;
        return;
    }
    if (slot->role == GattRole::InputNotify) {
        // A peer can retain CCCDs across reconnects and emit a notification
        // while discovery/security/init is still in progress. Such a packet
        // must not skip pairing or turn an incomplete connection into Ready.
        if (s_ble.stage != Stage::Ready || !slot->subscribed) return;
        const InputReportKind kind = gatt_uuid128_equals(slot->uuid_be, gatt_fd2_uuid())
            ? InputReportKind::Fd2
            : (gatt_uuid128_equals(slot->uuid_be, gatt_legacy_uuid()) ? InputReportKind::Legacy : InputReportKind::Unknown);
        if (s_ble.input_notify_logged < 20) {
            ESP_LOGI(TAG, "input notify handle=0x%04x len=%u kind=%u", attr_handle, len, static_cast<unsigned>(kind));
            s_ble.input_notify_logged++;
        }
        const uint32_t before = input_updates();
        if (input_parse_notify(kind, data, len)) {
            if (before == input_updates()) return;
            note_input_notify_rate();
            if (!s_ble.target_saved_this_connection) {
                config_set_saved_target(s_ble.pending_addr.val, s_ble.pending_addr.type);
                request_save();
                s_ble.target_saved_this_connection = true;
                s_ble.pair_mode = false;
                s_ble.manual_scan = false;
                ESP_LOGI(TAG, "saved live target addr_type=%u", s_ble.pending_addr.type);
            }
        } else {
            s_ble.stats.last_error = -1000;
            if (s_ble.input_parse_failed_logged < 20) {
                ESP_LOGW(TAG,
                         "input parse rejected handle=0x%04x len=%u kind=%u head=%02x %02x %02x %02x %02x %02x %02x %02x",
                         attr_handle,
                         len,
                         static_cast<unsigned>(kind),
                         len > 0 ? data[0] : 0,
                         len > 1 ? data[1] : 0,
                         len > 2 ? data[2] : 0,
                         len > 3 ? data[3] : 0,
                         len > 4 ? data[4] : 0,
                         len > 5 ? data[5] : 0,
                         len > 6 ? data[6] : 0,
                         len > 7 ? data[7] : 0);
                s_ble.input_parse_failed_logged++;
            }
        }
    }
}

void connect_to_candidate(const ble_gap_disc_desc &disc) {
    const uint64_t now = esp_timer_get_time();
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE || s_ble.stage == Stage::Connecting || s_ble.stay_disconnected) return;
    if (s_ble.next_connect_allowed_us != 0 && now < s_ble.next_connect_allowed_us) {
        return;
    }
    s_ble.pending_addr = disc.addr;
    mark_stage(Stage::Connecting);
    stop_scan();
    s_ble.pending_connect = true;
    s_ble.next_gatt_action_us = now + kCommandSpacingUs;
}

void begin_pending_connect() {
    const uint64_t now = esp_timer_get_time();
    s_ble.pending_connect = false;
    s_ble.stats.connect_attempts++;
    s_ble.connect_started_us = now;
    s_ble.next_connect_allowed_us = now + kConnectRetryBaseDelayUs;
    const ble_gap_conn_params params = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = kFastConnIntervalMin,
        .itvl_max = kFastConnIntervalMax,
        .latency = kFastConnLatency,
        .supervision_timeout = kFastConnSupervisionTimeout,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    const int rc = ble_gap_connect(s_ble.own_addr_type, &s_ble.pending_addr, kConnectTimeoutMs, &params, gap_event, nullptr);
    if (rc != 0) {
        s_ble.stats.last_error = rc;
        ESP_LOGE(TAG, "connect start failed rc=%d", rc);
        s_ble.connect_started_us = 0;
        s_ble.consecutive_connect_failures++;
        schedule_scan(connect_retry_delay_us());
    }
}

AdvMatch match_adv_fields(const ble_hs_adv_fields &fields) {
    AdvMatch match{};
    match.name = name_looks_like_controller(fields.name, fields.name_len);
    match.manufacturer = manufacturer_looks_pairable(fields.mfg_data, fields.mfg_data_len);
    for (uint8_t i = 0; i < fields.num_uuids128; ++i) {
        if (uuid128_matches_be_uuid(fields.uuids128[i].value, fd2_notify_uuid_be())) {
            match.service_uuid = true;
            break;
        }
    }
    return match;
}

void log_candidate(const ble_gap_disc_desc &disc, const ble_hs_adv_fields &fields, const AdvMatch &match) {
    char addr[18] = {};
    format_ble_addr(disc.addr.val, addr, sizeof(addr));
    char name[32] = {};
    const size_t name_len = std::min<size_t>(fields.name_len, sizeof(name) - 1);
    if (fields.name != nullptr && name_len > 0) {
        std::memcpy(name, fields.name, name_len);
    }
    s_ble.stats.candidates++;
    s_ble.stats.last_rssi = disc.rssi;
    std::memcpy(s_ble.stats.last_addr, addr, sizeof(s_ble.stats.last_addr));
    std::memcpy(s_ble.stats.last_name, name, sizeof(s_ble.stats.last_name));
    ESP_LOGI(TAG, "candidate addr=%s type=%u rssi=%d name=\"%s\" match(name=%d mfg=%d uuid=%d)",
             addr, disc.addr.type, disc.rssi, name, match.name, match.manufacturer, match.service_uuid);
}

void format_hex(const uint8_t *data, size_t len, char *out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = 0;
    if (data == nullptr) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < len && used + 2 < out_len; ++i) {
        const int written = snprintf(out + used, out_len - used, "%02x", data[i]);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
}

void format_uuid_le_as_be(const uint8_t uuid_le[kUuid128Len], char *out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (uuid_le == nullptr) {
        snprintf(out, out_len, "<none>");
        return;
    }
    uint8_t uuid_be[kUuid128Len] = {};
    for (size_t i = 0; i < kUuid128Len; ++i) {
        uuid_be[i] = uuid_le[kUuid128Len - 1 - i];
    }
    gatt_format_uuid(uuid_be, out, out_len);
}

void log_adv_probe(const ble_gap_disc_desc &disc, const ble_hs_adv_fields &fields, const AdvMatch &match) {
    s_ble.stats.adv_seen++;
    if (s_ble.stats.adv_logged >= 80) {
        return;
    }
    const bool interesting = match.likely_controller() ||
                             fields.name_len > 0 ||
                             fields.mfg_data_len > 0 ||
                             fields.num_uuids128 > 0;
    if (!interesting) {
        return;
    }

    char addr[18] = {};
    char name[32] = {};
    char mfg[96] = {};
    char uuid0[48] = {};
    format_ble_addr(disc.addr.val, addr, sizeof(addr));
    const size_t name_len = std::min<size_t>(fields.name_len, sizeof(name) - 1);
    if (fields.name != nullptr && name_len > 0) {
        std::memcpy(name, fields.name, name_len);
    }
    format_hex(fields.mfg_data, std::min<size_t>(fields.mfg_data_len, 40), mfg, sizeof(mfg));
    if (fields.num_uuids128 > 0) {
        format_uuid_le_as_be(fields.uuids128[0].value, uuid0, sizeof(uuid0));
    }
    ESP_LOGI(TAG,
             "adv probe addr=%s type=%u rssi=%d name=\"%s\" mfg=%s uuid128=%u uuid0=%s match=%d/%d/%d",
             addr,
             disc.addr.type,
             disc.rssi,
             name,
             mfg,
             fields.num_uuids128,
             uuid0,
             match.name,
             match.manufacturer,
             match.service_uuid);
    s_ble.stats.adv_logged++;
}

void start_scan() {
    if (!ble_scan_allowed(s_ble.host_synced, s_ble.stay_disconnected, s_ble.stage == Stage::Connecting,
                          s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE, s_ble.stats.scanning,
                          config_auto_connect(), s_ble.manual_scan, s_ble.pair_mode)) {
        return;
    }
    const uint64_t now = esp_timer_get_time();
    if (s_ble.scan_restart_due_us != 0 && now < s_ble.scan_restart_due_us) {
        return;
    }
    s_ble.scan_restart_due_us = 0;
    ble_gap_disc_params params{};
    params.filter_duplicates = 1;
    params.passive = 0;
    params.itvl = 0x0010;
    params.window = 0x0010;
    // Periodic windows refresh NimBLE's duplicate advertisement cache. The
    // same saved controller must remain discoverable after a failed wake.
    const int rc = ble_gap_disc(s_ble.own_addr_type, 5000, &params, gap_event, nullptr);
    if (rc != 0) {
        s_ble.stats.scanning = false;
        s_ble.stats.scan_errors++;
        s_ble.stats.last_error = rc;
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
        mark_stage(Stage::Idle);
        s_ble.scan_restart_due_us = now + kConnectRetryBaseDelayUs;
    } else {
        mark_stage(Stage::Scanning);
        s_ble.stats.scanning = true;
        s_ble.stats.scan_starts++;
        ESP_LOGI(TAG, "BLE scan started");
    }
}

void schedule_scan(uint64_t delay_us) {
    if (s_ble.stay_disconnected) {
        return;
    }
    stop_scan();
    s_ble.scan_restart_due_us = esp_timer_get_time() + delay_us;
    mark_stage(Stage::Idle);
    ESP_LOGI(TAG, "BLE scan scheduled in %llu ms", delay_us / 1000ULL);
}

void stop_scan() {
    if (s_ble.stats.scanning) {
        s_ble.stats.scanning = false;
        ble_gap_disc_cancel();
    }
}

bool addr_equals(const uint8_t a[6], const uint8_t b[6]) {
    return std::memcmp(a, b, 6) == 0;
}

bool disc_matches_saved(const ble_gap_disc_desc &disc) {
    if (!config_has_saved_target()) {
        return false;
    }
    uint8_t saved[6];
    uint8_t type = 0;
    config_get_saved_target(saved, &type);
    return disc.addr.type == type && addr_equals(disc.addr.val, saved);
}

void disconnect_current(const char *reason) {
    if (s_ble.stage == Stage::Disconnecting) return;
    ESP_LOGW(TAG, "disconnect_current reason=%s", reason ? reason : "manual");
    input_reset();
    if (s_rumble_queue) xQueueReset(s_rumble_queue);
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        const int rc = ble_gap_terminate(s_ble.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == 0) mark_stage(Stage::Disconnecting);
        else {
            s_ble.stats.last_error = rc;
            if (rc == BLE_HS_ENOTCONN) {
                s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
                clear_gatt();
                schedule_scan(kConnectRetryBaseDelayUs);
            }
        }
    } else {
        stop_scan();
        clear_gatt();
        input_reset();
        if (s_rumble_queue) xQueueReset(s_rumble_queue);
        schedule_scan(kConnectRetryBaseDelayUs);
    }
}

int gap_event(ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_ble.stage != Stage::Scanning || s_ble.stay_disconnected || s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) return 0;
        ble_hs_adv_fields fields{};
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        const AdvMatch match = match_adv_fields(fields);
        log_adv_probe(event->disc, fields, match);
        const bool saved_match = disc_matches_saved(event->disc);
        if (ble_candidate_allowed(match.likely_controller(), saved_match, config_has_saved_target(),
                                   s_ble.pair_mode, event->disc.rssi)) {
            log_candidate(event->disc, fields, match);
            connect_to_candidate(event->disc);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_ble.stats.scanning = false;
        if (s_ble.stage == Stage::Scanning) schedule_scan(kCommandSpacingUs);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (s_ble.stay_disconnected || s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                    // Cancellation can race a successful connection. Track
                    // it until disconnection so bond cleanup cannot pass its
                    // barrier while that rejected link still exists.
                    s_ble.conn_handle = event->connect.conn_handle;
                    s_ble.connect_started_us = 0;
                    disconnect_current("connection completed after cancellation");
                } else {
                    ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
                return 0;
            }
            s_ble.conn_handle = event->connect.conn_handle;
            s_ble.connect_started_us = 0;
            s_ble.next_connect_allowed_us = 0;
            s_ble.scan_restart_due_us = 0;
            s_ble.consecutive_connect_failures = 0;
            s_ble.stats.connected = true;
            s_ble.stats.scanning = false;
            s_ble.stats.encrypted = false;
            const PairingRecord record = pairing_record();
            s_ble.pairing_required = s_ble.pair_mode || !pairing_matches(record, s_ble.pending_addr);
            update_conn_desc();
            request_fast_link_params("connect");
            request_fast_connection_update("connect");
            ESP_LOGI(TAG, "connected handle=%u", s_ble.conn_handle);
            if (!s_ble.pairing_required) {
                // Provide the verified proprietary LTK through NimBLE's store
                // API, then use the supported security procedure entry point.
                mark_stage(Stage::Securing);
                const int rc = ble_gap_security_initiate(s_ble.conn_handle);
                if (rc != 0) { s_ble.stats.last_error = rc; disconnect_current("saved-key encryption failed"); }
            } else {
                mark_stage(Stage::DiscoverServices);
                if (ble_gattc_exchange_mtu(s_ble.conn_handle, mtu_cb, nullptr) != 0) s_ble.pending_service_discovery = true;
            }
        } else {
            s_ble.stats.last_error = event->connect.status;
            s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_ble.connect_started_us = 0;
            s_ble.consecutive_connect_failures++;
            s_ble.next_connect_allowed_us = esp_timer_get_time() + connect_retry_delay_us();
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            schedule_scan(connect_retry_delay_us());
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT: {
        if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE && event->disconnect.conn.conn_handle != s_ble.conn_handle) return 0;
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        const bool had_connection = s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE || s_ble.stats.connected;
        s_ble.stats.disconnect_count++;
        s_ble.stats.last_disconnect_reason = event->disconnect.reason;
        s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ble.stats.encrypted = false;
        s_ble.connect_started_us = 0;
        clear_gatt();
        input_reset();
        if (s_ble.stay_disconnected) {
            mark_stage(Stage::Idle);
        } else {
            if (had_connection) {
                s_ble.consecutive_connect_failures = 0;
                schedule_scan(kReconnectAfterReadyDelayUs);
            } else {
                s_ble.consecutive_connect_failures++;
                s_ble.next_connect_allowed_us = esp_timer_get_time() + connect_retry_delay_us();
                schedule_scan(connect_retry_delay_us());
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle != s_ble.conn_handle) return 0;
        if (event->enc_change.status != 0) {
            s_ble.stats.last_error = event->enc_change.status;
            pairing_failed("saved-key encryption rejected");
            return 0;
        }
        {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(s_ble.conn_handle, &desc) != 0 || !desc.sec_state.encrypted) {
                pairing_failed("link is not encrypted"); return 0;
            }
        }
        s_ble.stats.encrypted = true;
        if (s_ble.stage == Stage::Securing) {
            mark_stage(Stage::DiscoverServices);
            if (ble_gattc_exchange_mtu(s_ble.conn_handle, mtu_cb, nullptr) != 0) s_ble.pending_service_discovery = true;
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle != s_ble.conn_handle) return 0;
        handle_notification(event->notify_rx.attr_handle, event->notify_rx.om);
        update_ready_flags();
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        s_ble.stats.conn_update_status = event->conn_update.status;
        update_conn_desc();
        ESP_LOGI(TAG,
                 "conn update status=%d interval=%u (%lu us) latency=%u supervision=%u",
                 event->conn_update.status,
                 static_cast<unsigned>(s_ble.stats.conn_interval_units),
                 static_cast<unsigned long>(s_ble.stats.conn_interval_us),
                 static_cast<unsigned>(s_ble.stats.conn_latency),
                 static_cast<unsigned>(s_ble.stats.conn_supervision_timeout));
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
        ble_gap_upd_params params{};
        params.itvl_min = kFastConnIntervalMin;
        params.itvl_max = kFastConnIntervalMax;
        params.latency = kFastConnLatency;
        params.supervision_timeout = kFastConnSupervisionTimeout;
        params.min_ce_len = 0;
        params.max_ce_len = 0;
        ESP_LOGI(TAG,
                 "conn update req peer=%u..%u latency=%u supervision=%u; reply fast=%u..%u",
                 event->conn_update_req.peer_params ? event->conn_update_req.peer_params->itvl_min : 0,
                 event->conn_update_req.peer_params ? event->conn_update_req.peer_params->itvl_max : 0,
                 event->conn_update_req.peer_params ? event->conn_update_req.peer_params->latency : 0,
                 event->conn_update_req.peer_params ? event->conn_update_req.peer_params->supervision_timeout : 0,
                 static_cast<unsigned>(params.itvl_min),
                 static_cast<unsigned>(params.itvl_max));
        if (event->conn_update_req.self_params != nullptr) {
            *event->conn_update_req.self_params = params;
        }
        return 0;
    }
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        s_ble.stats.phy_update_status = event->phy_updated.status;
        update_conn_desc();
        ESP_LOGI(TAG,
                 "phy update status=%d tx=%u rx=%u",
                 event->phy_updated.status,
                 static_cast<unsigned>(s_ble.stats.tx_phy),
                 static_cast<unsigned>(s_ble.stats.rx_phy));
        return 0;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        s_ble.stats.data_len_tx_octets = event->data_len_chg.max_tx_octets;
        s_ble.stats.data_len_rx_octets = event->data_len_chg.max_rx_octets;
        s_ble.stats.data_len_tx_time = event->data_len_chg.max_tx_time;
        s_ble.stats.data_len_rx_time = event->data_len_chg.max_rx_time;
        ESP_LOGI(TAG,
                 "data len tx=%u/%u rx=%u/%u",
                 static_cast<unsigned>(s_ble.stats.data_len_tx_octets),
                 static_cast<unsigned>(s_ble.stats.data_len_tx_time),
                 static_cast<unsigned>(s_ble.stats.data_len_rx_octets),
                 static_cast<unsigned>(s_ble.stats.data_len_rx_time));
        return 0;
    default:
        return 0;
    }
}

void on_reset(int reason) {
    s_ble.host_synced = false;
    s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_ble.stats.scanning = false;
    s_ble.stats.encrypted = false;
    s_ble.pending_connect = false;
    clear_gatt();
    input_reset();
    mark_stage(Stage::Idle);
    s_ble.stats.last_error = reason;
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

void on_sync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        s_ble.stats.last_error = rc;
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_ble.own_addr_type);
    if (rc != 0) {
        s_ble.stats.last_error = rc;
        return;
    }
    uint8_t local_addr[6] = {};
    if (ble_hs_id_copy_addr(s_ble.own_addr_type, local_addr, nullptr) == 0) {
        std::memcpy(s_ble.stats.local_addr, local_addr, sizeof(s_ble.stats.local_addr));
        s_ble.stats.local_addr_valid = true;
        gatt_set_console_mac(local_addr);
    }
    s_ble.stats.own_addr_type = s_ble.own_addr_type;
    s_ble.host_synced = true;
    if (config_auto_connect()) schedule_scan(kCommandSpacingUs);
}

void host_task(void *param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t init_nvs_for_ble() {
    return config_init();
}

} // namespace

esp_err_t ble_start() {
    if (s_ble.started) {
        return ESP_OK;
    }
    s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    input_reset();

    ESP_RETURN_ON_ERROR(init_nvs_for_ble(), TAG, "NVS init failed");
    load_pairing_record();
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init failed");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("ns2pro-esp32s3");
    ble_store_config_init();
    s_original_store_read = ble_hs_cfg.store_read_cb;
    ble_hs_cfg.store_read_cb = pairing_store_read;
    s_rumble_queue = xQueueCreate(1, sizeof(RumbleRequest));
    if (s_rumble_queue == nullptr) return ESP_ERR_NO_MEM;
    ble_npl_event_init(&s_work_event, [](ble_npl_event *) {
        process_ble_work();
        // Host-owned connection state is the cleanup barrier. Published stats
        // can still describe the previous event and cannot establish this.
        if (s_forget_processed.load() && s_forget_in_progress.load() &&
            s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE &&
            s_ble.stage != Stage::Connecting && !s_ble.pending_connect && !ble_gap_conn_active())
            s_bond_cleanup_ready.store(true);
        publish_stats();
    }, nullptr);
    s_ble.started = true;
    s_ble.stats.started = true;
    publish_stats();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE central connector initialized");
    return ESP_OK;
}

namespace {
void process_ble_work() {
    if (s_forget_requested.exchange(false)) {
        s_control.store(ControlAction::None);
        process_control(ControlAction::Forget);
    }
    if (!s_ble.host_synced) {
        return;
    }
    const ControlAction action = s_control.exchange(ControlAction::None);
    // No new peer or security procedure may race the application-task bond
    // cleanup. A failed cleanup remains blocked until a successful retry.
    if (action != ControlAction::None && !s_forget_in_progress.load()) process_control(action);
    const uint64_t now = esp_timer_get_time();
    if (s_ble.pending_connect && s_ble.stage == Stage::Connecting &&
        now >= s_ble.next_gatt_action_us) {
        begin_pending_connect();
        return;
    }
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE &&
        s_ble.stage == Stage::Connecting &&
        s_ble.connect_started_us != 0 &&
        now - s_ble.connect_started_us > kConnectTimeoutUs) {
        ESP_LOGW(TAG, "connect timeout; restarting scan");
        ble_gap_conn_cancel();
        s_ble.connect_started_us = 0;
        s_ble.consecutive_connect_failures++;
        s_ble.next_connect_allowed_us = now + connect_retry_delay_us();
        clear_gatt();
        schedule_scan(connect_retry_delay_us());
        return;
    }
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE &&
        s_ble.scan_restart_due_us != 0 &&
        now >= s_ble.scan_restart_due_us) {
        s_ble.scan_restart_due_us = 0;
        start_scan();
        return;
    }
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (s_ble.stage == Stage::Disconnecting) {
        if (now - s_ble.stage_started_us > kGattStageTimeoutUs) {
            mark_stage(Stage::Idle);
            disconnect_current("disconnect watchdog");
        }
        return;
    }
    if (s_ble.write_in_flight && now - s_ble.write_started_us > kInitAckTimeoutUs) {
        disconnect_current("ATT write response timeout"); return;
    }
    if (s_ble.battery_read_in_flight && now - s_ble.battery_read_started_us > kInitAckTimeoutUs) {
        input_note_legacy_power(nullptr, 0);
        disconnect_current("battery ATT read timeout"); return;
    }
    if (s_ble.stage == Stage::PersistPairing && s_pair_saved_generation.load() == s_ble.pair_generation) {
        if (s_pair_write_error.load() != ESP_OK) {
            pairing_failed("pairing persistence failed"); return;
        }
        s_ble.stats.pairing_active = false;
        s_ble.pairing_required = false;
        s_ble.init_index = 4;
        s_ble.pending_init_command = true;
        std::memset(s_ble.pair_a1, 0, sizeof(s_ble.pair_a1));
        std::memset(s_ble.pair_a2, 0, sizeof(s_ble.pair_a2));
        std::memset(s_ble.pair_ltk, 0, sizeof(s_ble.pair_ltk));
        mark_stage(Stage::Initializing);
    }
    if (s_ble.stage != Stage::Ready && s_ble.stage != Stage::Initializing &&
        now - s_ble.stage_started_us > kGattStageTimeoutUs) {
        disconnect_current("GATT stage timeout");
        return;
    }
    if (s_ble.stage == Stage::Initializing &&
        s_ble.init_command_sent_us != 0 &&
        now - s_ble.init_command_sent_us > kInitAckTimeoutUs) {
        disconnect_current("init ACK timeout");
        return;
    }
    if (s_ble.stage == Stage::Pairing && s_ble.pair_waiting_ack &&
        now - s_ble.init_command_sent_us > kInitAckTimeoutUs) {
        pairing_failed("pairing ACK timeout"); return;
    }
    if (s_ble.stage == Stage::Ready) {
        const uint32_t age = input_last_age_ms();
        if (age == 0xffffffffu && now - s_ble.stage_started_us > kGattStageTimeoutUs) {
            disconnect_current("first input timeout");
            return;
        }
        if (age != 0xffffffffu && age > kGattStageTimeoutUs / 1000ULL) {
            disconnect_current("physical input stream stalled");
            return;
        }
    }
    if (s_ble.next_gatt_action_us != 0 && now < s_ble.next_gatt_action_us) {
        return;
    }
    if (s_ble.write_in_flight || s_ble.battery_read_in_flight) return;
    if (s_ble.stage == Stage::Pairing && !s_ble.pair_waiting_ack) {
        send_pairing_step(); return;
    }
    if (s_ble.pending_service_discovery) {
        s_ble.pending_service_discovery = false;
        start_service_discovery();
        return;
    }
    if (s_ble.pending_characteristic_discovery) {
        s_ble.pending_characteristic_discovery = false;
        discover_next_characteristics();
        return;
    }
    if (s_ble.pending_init_command && s_ble.stage == Stage::Initializing) {
        s_ble.pending_init_command = false;
        send_current_init_command();
        return;
    }
    if (s_ble.pending_subscribe_ack && s_ble.stage == Stage::SubscribeAck) {
        s_ble.pending_subscribe_ack = false;
        subscribe_role(GattRole::AckNotify);
        s_ble.next_gatt_action_us = now + kCommandSpacingUs;
        return;
    }
    if (s_ble.pending_subscribe_input && s_ble.stage == Stage::SubscribeInput) {
        s_ble.pending_subscribe_input = false;
        subscribe_role(GattRole::InputNotify);
        s_ble.next_gatt_action_us = now + kCommandSpacingUs;
        return;
    }
    RumbleRequest rumble{};
    if (s_ble.stage == Stage::Ready && s_ble.legacy_power_handle != 0 &&
        input_last_age_ms() <= kInputStaleMs && now >= s_ble.next_battery_read_us) {
        s_ble.battery_read_in_flight = true;
        s_ble.battery_read_started_us = now;
        s_ble.next_battery_read_us = now + 10000000ULL;
        const int rc = ble_gattc_read(s_ble.conn_handle, s_ble.legacy_power_handle, battery_read_cb, nullptr);
        if (rc != 0) {
            s_ble.battery_read_in_flight = false;
            input_note_legacy_power(nullptr, 0);
        }
        return;
    }
    if (s_ble.stage == Stage::Ready && xQueueReceive(s_rumble_queue, &rumble, 0) == pdTRUE) {
        InputSnapshot source{};
        input_get_snapshot(&source);
        if (!feedback_ble_allowed(rumble.data,rumble.len,rumble.source_epoch,source)) return;
        s_ble.write_in_flight = !s_ble.rumble_no_rsp;
        s_ble.write_started_us = now;
        const int rc = s_ble.rumble_no_rsp
            ? ble_gattc_write_no_rsp_flat(s_ble.conn_handle, s_ble.rumble_handle, rumble.data, rumble.len)
            : ble_gattc_write_flat(s_ble.conn_handle, s_ble.rumble_handle, rumble.data, rumble.len, write_cb, nullptr);
        if (rc == 0) s_ble.stats.rumble_writes++;
        else { s_ble.write_in_flight = false; s_ble.stats.rumble_errors++; s_ble.stats.last_error = rc; }
    }
}

void publish_stats() {
    update_ready_flags();
    BleStats out = s_ble.stats;
    out.auto_connect = config_auto_connect();
    out.saved_target_valid = config_has_saved_target();
    out.pair_mode = s_ble.pair_mode;
    out.host_synced = s_ble.host_synced;
    out.persist_pending = s_save_pending.load() || s_save_generation.load() != s_saved_generation.load();
    out.persist_error = s_persist_error.load();
    const bool pair_write_pending = s_pair_save_pending.load() || s_pair_saved_generation.load() != s_pair_generation.load();
    out.pairing_key_saved = pairing_record().valid && !pair_write_pending && s_pair_write_error.load() == 0;
    out.persist_pending = out.persist_pending || pair_write_pending || s_delete_bond_pending.load() ||
        s_delete_bond_active.load() || s_forget_requested.load();
    if (s_pair_write_error.load()) out.persist_error = s_pair_write_error.load();
    if (s_bond_write_error.load()) out.persist_error = s_bond_write_error.load();
    if (out.saved_target_valid) {
        config_get_saved_target(out.saved_addr, &out.saved_addr_type);
    }
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        out.scanning = false;
    }
    if (input_last_age_ms() > kInputStaleMs) out.notify_hz = 0;
    portENTER_CRITICAL(&s_stats_mux);
    s_published_stats = out;
    portEXIT_CRITICAL(&s_stats_mux);
}

void process_control(ControlAction action) {
    if (action == ControlAction::AutoOn || action == ControlAction::AutoOff) {
        const bool enabled = action == ControlAction::AutoOn;
        config_set_auto_connect(enabled);
        request_save();
        if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
        action = enabled ? ControlAction::Reconnect : ControlAction::Disconnect;
    }
    s_ble.stay_disconnected = action == ControlAction::Disconnect || action == ControlAction::Forget;
    s_ble.manual_scan = !s_ble.stay_disconnected;
    s_ble.pair_mode = action == ControlAction::Pair;
    s_ble.pending_connect = false;
    s_ble.scan_restart_due_us = 0;
    s_ble.next_connect_allowed_us = 0;
    stop_scan();
    if (s_ble.stage == Stage::Connecting) ble_gap_conn_cancel();
    if (action == ControlAction::Forget) {
        queue_pairing_record(PairingRecord{});
        // This bridge has one active peer. Always clear the whole application
        // bond store, including retries after the saved address was removed.
        s_delete_bond_pending.store(true);
        config_forget_saved_target();
        request_save();
        s_forget_processed.store(true);
    }
    input_reset();
    xQueueReset(s_rumble_queue);
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        disconnect_current("management control");
        return;
    }
    clear_gatt();
    mark_stage(Stage::Idle);
    if (!s_ble.stay_disconnected) schedule_scan(kCommandSpacingUs);
}
} // namespace

void ble_task() {
    if (!s_ble.started) {
        s_control.store(ControlAction::None);
        if (s_forget_requested.exchange(false)) request_offline_forget();
    }
    // Flash writes run in the application task, never a NimBLE notification
    // callback. All BLE operations are serialized on NimBLE's host event queue.
    if (s_save_pending.exchange(false)) {
        const uint32_t generation = s_save_generation.load();
        s_persist_error.store(config_save());
        s_saved_generation.store(generation);
    }
    persist_pairing_record();
    if (s_delete_bond_pending.load() && s_bond_cleanup_ready.load()) {
        s_delete_bond_active.store(true);
        if (s_delete_bond_pending.exchange(false)) {
            // ble_store_clear() serializes each SDK store mutation under the
            // NimBLE host lock. No connection can write a new bond while the
            // cleanup barrier holds. NVS work stays off the BLE host task.
            int rc = s_ble.started ? ble_store_clear() : 0;
            if (rc == 0) rc = erase_bond_namespace();
            s_bond_write_error.store(rc);
        }
        s_delete_bond_active.store(false);
    }
    if (s_forget_in_progress.load() && ble_forget_complete()) s_forget_in_progress.store(false);
    if (s_ble.started) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_work_event);
    else publish_stats();
}

void ble_get_stats(BleStats *out) {
    if (out == nullptr) return;
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_published_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}

bool ble_send_rumble(const uint8_t *data, uint16_t len, uint32_t source_epoch) {
    BleStats stats{};
    ble_get_stats(&stats);
    if (!s_rumble_queue || !stats.connected || !stats.rumble_ready || !data || len == 0 || len > sizeof(RumbleRequest::data)) return false;
    InputSnapshot source{};
    input_get_snapshot(&source);
    if (!feedback_ble_allowed(data,len,source_epoch,source)) return false;
    RumbleRequest request{};
    request.source_epoch = source_epoch;
    request.len = len;
    std::memcpy(request.data, data, len);
    return xQueueOverwrite(s_rumble_queue, &request) == pdTRUE;
}

void ble_start_scan() { s_control.store(ControlAction::Scan); }
void ble_pair() { s_control.store(ControlAction::Pair); }
void ble_reconnect() { s_control.store(ControlAction::Reconnect); }
void ble_disconnect() { s_control.store(ControlAction::Disconnect); }
void ble_forget() {
    s_forget_processed.store(false);
    s_bond_cleanup_ready.store(false);
    s_forget_in_progress.store(true);
    s_forget_requested.store(true);
}
bool ble_forget_complete() {
    ForgetProgress progress;
    progress.processed = s_forget_processed.load();
    progress.settings_pending = s_save_pending.load() || s_save_generation.load() != s_saved_generation.load();
    progress.pairing_pending = s_pair_save_pending.load() || s_pair_saved_generation.load() != s_pair_generation.load();
    progress.bond_pending = s_delete_bond_pending.load() || s_delete_bond_active.load() || s_forget_requested.load();
    progress.saved_target = config_has_saved_target();
    progress.pairing_key = pairing_record().valid;
    progress.storage_error = s_persist_error.load() != ESP_OK ? s_persist_error.load() : s_pair_write_error.load();
    progress.bond_error = s_bond_write_error.load();
    return ble_forget_is_complete(progress);
}
void ble_set_auto_connect(bool enabled) { s_control.store(enabled ? ControlAction::AutoOn : ControlAction::AutoOff); }

} // namespace ns2
