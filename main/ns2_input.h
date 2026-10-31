#pragma once

#include <cstddef>
#include <cstdint>

namespace ns2 {

constexpr size_t kMotionSampleSize = 12;
constexpr size_t kInputRawReportMax = 64;
constexpr uint32_t kInputStaleMs = 250;

enum class InputReportKind : uint8_t {
    Unknown = 0,
    Fd2,
    Legacy,
};

struct InputSnapshot {
    bool valid = false;
    InputReportKind kind = InputReportKind::Unknown;
    uint16_t len = 0;
    uint8_t first_byte = 0;
    uint32_t updates = 0;
    uint32_t parse_errors = 0;
    // Count physical source frames using the controller's wire counter. These
    // counters are cumulative across reconnects; source_epoch identifies them.
    uint32_t unique_input_reports = 0;
    uint32_t duplicate_input_reports = 0;
    uint32_t input_rate_time_ms = 0;
    uint32_t input_age_ms = 0xffffffffu;
    bool input_rate_supported = false;
    uint8_t active_input = 0; // 0 = none/stale, 1 = NS2Pro BLE
    uint32_t source_epoch = 0;
    uint32_t source_sequence = 0;
    uint32_t source_timestamp_us = 0; // FD2 motion timestamp, if available
    uint32_t buttons = 0;
    uint16_t lx = 2048;
    uint16_t ly = 2048;
    uint16_t rx = 2048;
    uint16_t ry = 2048;
    bool battery_valid = false;
    uint8_t battery_raw = 0;
    uint8_t battery_percent = 0;
    bool battery_charging = false;
    uint8_t battery_offset = 0xff;
    uint16_t battery_voltage_mv = 0; // FD2 reports voltage, not percentage
    bool battery_from_legacy_read = false;
    uint32_t battery_age_ms = 0xffffffffu;
    bool raw_valid = false;
    uint16_t raw_len = 0;
    uint8_t raw[kInputRawReportMax] = {};
    bool motion_valid = false;
    uint32_t motion_updates = 0;
    InputReportKind motion_kind = InputReportKind::Unknown;
    uint16_t motion_len = 0;
    uint8_t motion_offset = 0;
    uint8_t motion[kMotionSampleSize] = {};
};

void input_reset();
bool input_parse_notify(InputReportKind kind, const uint8_t *data, uint16_t len);
// Explicit receive time makes expiry deterministic in portable parser tests.
// Deduplication always uses the wire counter, never this receive time.
bool input_parse_notify_at(InputReportKind kind, const uint8_t *data, uint16_t len, uint64_t received_us);
// Side-channel GATT read of the legacy report's real power byte. Does not
// advance the FD2 input counter, refresh input liveness, or change sources.
void input_note_legacy_power(const uint8_t *data, uint16_t len);
bool input_get_snapshot(InputSnapshot *out);
uint32_t input_updates();
uint32_t input_parse_errors();
uint32_t input_last_age_ms();
const char *input_kind_name(InputReportKind kind);
void input_format_motion_json(const InputSnapshot *snapshot, char *out, size_t out_len);

} // namespace ns2
