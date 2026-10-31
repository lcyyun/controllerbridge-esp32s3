#pragma once

#include <cstddef>
#include <cstdint>

namespace ns2 {

constexpr uint8_t kUsbReportIdInput = 0x05;
constexpr uint8_t kUsbReportIdOutput = 0x02;
constexpr uint8_t kUsbReportIdFeature = 0x7f;
constexpr uint16_t kNintendoVid = 0x057e;
constexpr uint16_t kNintendoPid = 0x2069;

struct AdvMatch {
    bool name = false;
    bool manufacturer = false;
    bool service_uuid = false;

    bool likely_controller() const {
        return name || manufacturer || service_uuid;
    }
};

bool name_looks_like_controller(const uint8_t *name, size_t len);
bool manufacturer_looks_pairable(const uint8_t *data, size_t len);
bool uuid128_matches_be_uuid(const uint8_t uuid_le[16], const uint8_t uuid_be[16]);
void format_ble_addr(const uint8_t addr[6], char *out, size_t out_len);

const uint8_t *fd2_notify_uuid_be();

// Shared, host-testable reconnect decisions. The NimBLE event worker owns the
// mutable state; callback events only update it or schedule the next action.
bool ble_scan_allowed(bool synced, bool stay_disconnected, bool connecting,
                      bool connected, bool scanning, bool auto_connect,
                      bool manual_scan, bool pairing);
bool ble_candidate_allowed(bool likely_controller, bool saved_match,
                           bool has_saved_target, bool pairing, int rssi);
uint64_t ble_retry_delay_us(uint8_t failures);

struct ForgetProgress {
    bool processed = false;
    bool settings_pending = false;
    bool pairing_pending = false;
    bool bond_pending = false;
    bool saved_target = false;
    bool pairing_key = false;
    int storage_error = 0;
    int bond_error = 0; // independent of settings saves; only cleanup clears it
};
// Completion depends on durable cleanup, not whether Bluetooth could start.
bool ble_forget_is_complete(const ForgetProgress &progress);

} // namespace ns2
