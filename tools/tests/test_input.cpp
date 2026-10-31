#include "ns2_input.h"
#include "ns2_gatt.h"
#include "ns2_protocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static uint64_t clock_us = 1000;
int64_t esp_timer_get_time() { return static_cast<int64_t>(clock_us); }
static void sequence(uint8_t *data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(value >> (8 * i));
}
static void center(uint8_t *data, size_t offset) {
    data[offset] = 0; data[offset + 1] = 8; data[offset + 2] = 128;
}
static ns2::InputSnapshot snapshot() {
    ns2::InputSnapshot value;
    ns2::input_get_snapshot(&value);
    return value;
}
static void parse(ns2::InputReportKind kind, const uint8_t *data, uint16_t len) {
    assert(ns2::input_parse_notify(kind, data, len));
}

int main() {
    using namespace ns2;
    uint8_t fd2[63] = {};
    center(fd2, 10); center(fd2, 13);
    input_reset();
    assert(!snapshot().valid && snapshot().active_input == 0);
    const uint32_t epoch = snapshot().source_epoch;
    sequence(fd2, 0xfffffffeu);
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    auto value = snapshot();
    assert(value.valid && value.unique_input_reports == 1 && value.active_input == 1);
    assert(value.input_rate_supported && !value.battery_valid); // counter byte is NOT battery
    assert(value.lx == 2048 && value.ly == 2048);
    clock_us += 4000;
    sequence(fd2, 0xffffffffu);
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    clock_us += 4000;
    sequence(fd2, 0);
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().unique_input_reports == 3); // stationary counter wrap counts
    clock_us += 4000;
    fd2[4] = 0x08; // changed controls on duplicate counter still must not count
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().unique_input_reports == 3 && snapshot().buttons == 0);
    assert(snapshot().duplicate_input_reports == 1);
    sequence(fd2, 0xffffffffu);
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().unique_input_reports == 3); // old/reordered report rejected
    sequence(fd2, 1);
    fd2[7] = 3; // GR / GL extension buttons
    fd2[31] = 0xd8; fd2[32] = 0x0e; // 3800 mV
    sequence(fd2 + 42, 123456);
    fd2[48] = 0x34; fd2[49] = 0x12;
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    value = snapshot();
    assert(value.unique_input_reports == 4 && value.source_sequence == 1);
    assert(value.buttons & (1u << 1));
    assert(value.buttons & (1u << 18));
    assert(value.buttons & (1u << 19));
    assert(value.battery_voltage_mv == 3800 && !value.battery_valid);
    assert(value.source_timestamp_us == 123456 && value.motion[0] == 0x34);
    clock_us += static_cast<uint64_t>(kInputStaleMs + 1) * 1000;
    parse(InputReportKind::Fd2, fd2, sizeof(fd2)); // stale duplicate cannot keep buttons pressed
    value = snapshot();
    assert(!value.valid && value.active_input == 0 && value.buttons == 0);
    assert(!value.motion_valid && !value.raw_valid && !value.battery_valid);
    assert(value.lx == 2048 && value.battery_voltage_mv == 0);
    assert(value.unique_input_reports == 4);
    sequence(fd2, 2);
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().valid && snapshot().unique_input_reports == 5);
    input_reset();
    assert(snapshot().source_epoch == epoch + 1);
    sequence(fd2, 0); // reconnect may restart its source counter
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().unique_input_reports == 6 && snapshot().valid);
    uint8_t legacy[11] = {};
    center(legacy, 5); center(legacy, 8);
    legacy[0] = 255; legacy[1] = (9 << 2) | 2;
    parse(InputReportKind::Legacy, legacy, sizeof(legacy));
    assert(snapshot().source_epoch == epoch + 2); // input kind switch
    assert(snapshot().battery_valid && snapshot().battery_percent == 100 && snapshot().battery_charging);
    legacy[0] = 0; legacy[1] = 0;
    parse(InputReportKind::Legacy, legacy, sizeof(legacy));
    assert(snapshot().unique_input_reports == 8);
    assert(snapshot().battery_valid && snapshot().battery_percent == 0);
    legacy[0] = 1; legacy[1] = 0xff;
    parse(InputReportKind::Legacy, legacy, sizeof(legacy));
    assert(!snapshot().battery_valid);
    assert(!input_parse_notify(InputReportKind::Unknown, fd2, sizeof(fd2)));
    assert(!input_parse_notify(InputReportKind::Fd2, fd2, 15));
    assert(!input_parse_notify(InputReportKind::Legacy, nullptr, 11));
    assert(snapshot().parse_errors == 3);
    clock_us = (uint64_t{0xffffffffu} + 1) * 1000;
    assert(snapshot().input_rate_time_ms == 0); // manager clock uses modular uint32 time
    input_reset(); sequence(fd2, 10);
    parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    const auto before_power = snapshot();
    legacy[1] = (9 << 2) | 2;
    clock_us += 1000;
    input_note_legacy_power(legacy, sizeof(legacy));
    value = snapshot();
    assert(value.battery_valid && value.battery_percent == 100 && value.battery_from_legacy_read);
    assert(value.battery_age_ms == 0 && value.battery_voltage_mv == 3800);
    assert(value.unique_input_reports == before_power.unique_input_reports);
    assert(value.source_epoch == before_power.source_epoch && value.kind == InputReportKind::Fd2);
    assert(value.input_age_ms == 1); // query does not refresh physical liveness
    clock_us += 251000;
    input_note_legacy_power(legacy, sizeof(legacy));
    assert(!snapshot().valid && !snapshot().battery_valid); // side channel can't revive stale input
    sequence(fd2, 11); parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().battery_valid);
    input_note_legacy_power(nullptr, 0);
    assert(!snapshot().battery_valid); // read errors must make battery unknown
    uint8_t empty_attribute[11] = {};
    input_note_legacy_power(empty_attribute, sizeof(empty_attribute));
    assert(!snapshot().battery_valid); // no fabricated empty battery from an uninitialized characteristic
    input_note_legacy_power(legacy, sizeof(legacy));
    clock_us += 30001000;
    sequence(fd2, 12); parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(snapshot().valid && !snapshot().battery_valid); // independent battery expiry
    input_note_legacy_power(legacy, sizeof(legacy));
    input_reset(); sequence(fd2, 0); parse(InputReportKind::Fd2, fd2, sizeof(fd2));
    assert(!snapshot().battery_valid); // reconnect never carries another peer's battery

    uint8_t command[32] = {}, pair_value[16] = {};
    for (size_t i = 0; i < 16; ++i) pair_value[i] = static_cast<uint8_t>(i + 1);
    assert(gatt_build_pair_command(command, sizeof(command), kPairAddress, pair_value, 12) == 22);
    assert(command[0] == 0x15 && command[3] == 1 && command[5] == 14 && command[9] == 2);
    assert(std::memcmp(command + 10, pair_value, 12) == 0);
    assert(gatt_build_pair_command(command, sizeof(command), kPairKey, pair_value, 16) == 25);
    assert(command[3] == 4 && command[5] == 17 && std::memcmp(command + 9, pair_value, 16) == 0);
    assert(gatt_build_pair_command(command, sizeof(command), kPairChallenge, pair_value, 16) == 25);
    assert(command[3] == 2);
    assert(gatt_build_pair_command(command, sizeof(command), kPairFinalize, nullptr, 0) == 9);
    assert(command[3] == 3 && command[5] == 1 && command[8] == 0);
    assert(gatt_build_pair_command(command, 8, kPairFinalize, nullptr, 0) == 0);
    assert(gatt_build_pair_command(command, sizeof(command), kPairKey, pair_value, 15) == 0);
    assert(gatt_build_pair_command(command, sizeof(command), 0xff, nullptr, 0) == 0);
    uint8_t ack[25] = {0x15, 0x01, 0x01, kPairKey, 0, 0, 0, 0, 0x01};
    const uint8_t *payload = nullptr;
    assert(gatt_parse_pair_response(ack, sizeof(ack), kPairKey, &payload) && payload == ack + 8);
    assert(!gatt_parse_pair_response(ack, 24, kPairKey, &payload) && payload == nullptr);
    assert(!gatt_parse_pair_response(ack, sizeof(ack), kPairAddress, &payload));
    ack[8] = 0;
    assert(!gatt_parse_pair_response(ack, sizeof(ack), kPairKey, &payload));
    ack[8] = 1; ack[3] = kPairChallenge;
    assert(gatt_parse_pair_response(ack, sizeof(ack), kPairChallenge, &payload));
    ack[2] = 0;
    assert(!gatt_parse_pair_response(ack, sizeof(ack), kPairChallenge, &payload));
    ack[2] = 1; ack[3] = kPairAddress;
    assert(!gatt_parse_pair_response(ack, 16, kPairAddress, &payload));
    assert(gatt_parse_pair_response(ack, 17, kPairAddress, &payload));
    PairingRecord record;
    assert(!gatt_pairing_record_valid(record, sizeof(record)));
    record.valid = 1; record.addr[0] = 1; record.ltk[0] = 1;
    assert(gatt_pairing_record_valid(record, sizeof(record)));
    assert(!gatt_pairing_record_valid(record, sizeof(record) - 1));
    record.magic ^= 1;
    assert(!gatt_pairing_record_valid(record, sizeof(record)));
    record.magic ^= 1; record.addr_type = 4;
    assert(!gatt_pairing_record_valid(record, sizeof(record)));
    // Public captured 0x15 pairing vector from switch2_controller_research.
    const uint8_t host_key[16] = {0x35,0x03,0xe9,0x29,0x82,0x87,0x71,0x24,0xbe,0xa8,0x0c,0x66,0x46,0x15,0x83,0x4b};
    const uint8_t peer_key[16] = {0x5c,0xf6,0xee,0x79,0x2c,0xdf,0x05,0xe1,0xba,0x2b,0x63,0x25,0xc4,0x1a,0x5f,0x10};
    const uint8_t challenge[16] = {0x6f,0xc6,0xdf,0x8a,0xd8,0xfe,0xdf,0x15,0xbb,0x8c,0x15,0xe9,0x1f,0x32,0x05,0x44};
    const uint8_t expected_response[16] = {0x13,0x4c,0x97,0xf5,0x11,0xb9,0xb6,0xdd,0x4d,0x86,0xfd,0x40,0xf5,0x36,0xe9,0xed};
    uint8_t ltk[16], key_be[16], challenge_be[16], response[16];
    gatt_derive_pair_key(host_key, peer_key, ltk);
    gatt_pair_challenge_inputs(ltk, challenge, key_be, challenge_be);
    assert(key_be[0] == 0x5b && key_be[15] == 0x69 && challenge_be[0] == 0x44 && challenge_be[15] == 0x6f);
    std::memcpy(response, expected_response, 16);
    assert(gatt_pair_challenge_matches(expected_response, response));
    response[7] ^= 1;
    assert(!gatt_pair_challenge_matches(expected_response, response));
    assert(ble_scan_allowed(true, false, false, false, false, true, false, false));
    assert(!ble_scan_allowed(true, true, false, false, false, true, false, false));
    assert(!ble_scan_allowed(false, false, false, false, false, true, false, false));
    assert(!ble_scan_allowed(true, false, true, false, false, true, false, false));
    assert(!ble_scan_allowed(true, false, false, true, false, true, false, false));
    assert(!ble_scan_allowed(true, false, false, false, true, true, false, false));
    assert(!ble_scan_allowed(true, false, false, false, false, false, false, false));
    assert(ble_scan_allowed(true, false, false, false, false, false, true, false));
    assert(ble_scan_allowed(true, false, false, false, false, false, false, true));
    assert(ble_candidate_allowed(false, true, true, false, -120)); // sleeping saved peer needs no fresh name
    assert(!ble_candidate_allowed(true, false, true, false, -20)); // never steal another paired controller
    assert(ble_candidate_allowed(true, false, true, true, -20));
    assert(!ble_candidate_allowed(false, false, false, true, -20));
    assert(!ble_candidate_allowed(true, false, false, true, -100));
    assert(ble_retry_delay_us(0) == 1200000 && ble_retry_delay_us(1) == 2400000);
    assert(ble_retry_delay_us(3) == 9600000 && ble_retry_delay_us(4) == 10000000);
    assert(ble_retry_delay_us(255) == 10000000);
    ForgetProgress recovery;
    assert(!ble_forget_is_complete(recovery)); // never report success before the request is processed
    recovery.processed = true;
    recovery.settings_pending = true;
    recovery.pairing_pending = true;
    recovery.bond_pending = true;
    assert(!ble_forget_is_complete(recovery));
    recovery.settings_pending = false;
    recovery.pairing_pending = false;
    assert(!ble_forget_is_complete(recovery)); // offline NimBLE namespace erase still running
    recovery.bond_pending = false;
    recovery.storage_error = 0x1101;
    assert(!ble_forget_is_complete(recovery)); // durable erase failure must block reboot
    recovery.storage_error = 0;
    recovery.bond_error = 0x1101;
    assert(!ble_forget_is_complete(recovery)); // settings save cannot mask a failed bond erase
    recovery.processed = false;
    assert(!ble_forget_is_complete(recovery)); // retry has not reached the host yet
    recovery.processed = true;
    recovery.bond_pending = true;
    assert(!ble_forget_is_complete(recovery)); // failed erase remains pending independently
    recovery.bond_pending = false;
    assert(!ble_forget_is_complete(recovery)); // RAM deletion alone cannot establish durability
    recovery.bond_error = 0; // only a successful namespace erase clears this error
    recovery.saved_target = true;
    assert(!ble_forget_is_complete(recovery));
    recovery.saved_target = false;
    recovery.pairing_key = true;
    assert(!ble_forget_is_complete(recovery));
    recovery.pairing_key = false;
    assert(ble_forget_is_complete(recovery)); // succeeds without a BLE-host-started condition
    std::puts("PASS: physical wire-counter dedup/wrap/reconnect/stale/battery/motion/extension and pairing protocol vectors");
}
