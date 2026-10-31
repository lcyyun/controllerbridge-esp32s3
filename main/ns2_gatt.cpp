#include "ns2_gatt.h"

#include <cstdio>
#include <cstring>

namespace ns2 {
void gatt_derive_pair_key(const uint8_t host[16], const uint8_t peer[16], uint8_t ltk[16]) {
    for (size_t i = 0; i < 16; ++i) ltk[i] = host[i] ^ peer[i];
}

void gatt_pair_challenge_inputs(const uint8_t ltk[16], const uint8_t challenge[16],
                                uint8_t key_be[16], uint8_t challenge_be[16]) {
    for (size_t i = 0; i < 16; ++i) {
        key_be[i] = ltk[15 - i];
        challenge_be[i] = challenge[15 - i];
    }
}

bool gatt_pair_challenge_matches(const uint8_t expected[16], const uint8_t actual[16]) {
    uint8_t different = 0;
    for (size_t i = 0; i < 16; ++i) different |= expected[i] ^ actual[i];
    return different == 0;
}

bool gatt_pairing_record_valid(const PairingRecord &record, size_t size) {
    if (size != sizeof(record) || record.magic != 0x3150324e || record.valid != 1 || record.addr_type > 3) return false;
    uint8_t address_bits = 0, key_bits = 0;
    for (const uint8_t value : record.addr) address_bits |= value;
    for (const uint8_t value : record.ltk) key_bits |= value;
    return address_bits != 0 && key_bits != 0;
}

size_t gatt_build_pair_command(uint8_t *out, size_t capacity, uint8_t subcommand,
                               const uint8_t *value, size_t value_len) {
    const size_t required = subcommand == kPairAddress ? 12u :
        (subcommand == kPairKey || subcommand == kPairChallenge) ? 16u : 0u;
    if (subcommand != kPairAddress && subcommand != kPairKey &&
        subcommand != kPairChallenge && subcommand != kPairFinalize) return 0;
    const size_t payload_len = required + (subcommand == kPairAddress ? 2u : 1u);
    if (!out || capacity < 8 + payload_len || value_len != required || (required && !value)) return 0;
    std::memset(out, 0, 8 + payload_len);
    out[0] = 0x15; out[1] = 0x91; out[2] = 0x01; out[3] = subcommand;
    out[5] = static_cast<uint8_t>(payload_len);
    if (subcommand == kPairAddress) out[9] = 0x02;
    if (required) std::memcpy(out + (subcommand == kPairAddress ? 10 : 9), value, required);
    return 8 + payload_len;
}

bool gatt_parse_pair_response(const uint8_t *data, size_t len, uint8_t expected,
                              const uint8_t **payload) {
    if (payload) *payload = nullptr;
    if (expected != kPairAddress && expected != kPairKey && expected != kPairChallenge && expected != kPairFinalize) return false;
    const size_t required = expected == kPairAddress ? 9u :
        (expected == kPairKey || expected == kPairChallenge) ? 17u : 1u;
    if (!data || len < 8 + required || data[0] != 0x15 || data[1] != 0x01 ||
        data[2] != 0x01 || data[3] != expected || data[8] != 0x01) return false;
    if (payload) *payload = data + 8;
    return true;
}

namespace {

const uint8_t kUuidNotifyFd2[kUuid128Len] = {
    0xab, 0x7d, 0xe9, 0xbe, 0x89, 0xfe, 0x49, 0xad,
    0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2,
};

const uint8_t kUuidNotifyLegacy[kUuid128Len] = {
    0x74, 0x92, 0x86, 0x6c, 0xec, 0x3e, 0x46, 0x19,
    0x82, 0x58, 0x32, 0x75, 0x5f, 0xfc, 0xc0, 0xf8,
};

const uint8_t kUuidAck[kUuid128Len] = {
    0xc7, 0x65, 0xa9, 0x61, 0xd9, 0xd8, 0x4d, 0x36,
    0xa2, 0x0a, 0x53, 0x15, 0xb1, 0x11, 0x83, 0x6a,
};

const uint8_t kUuidCommand[kUuid128Len] = {
    0x64, 0x9d, 0x4a, 0xc9, 0x8e, 0xb7, 0x4e, 0x6c,
    0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05,
};

const uint8_t kUuidRumble[kUuid128Len] = {
    0xcc, 0x48, 0x3f, 0x51, 0x92, 0x58, 0x42, 0x7d,
    0xa9, 0x39, 0x63, 0x0c, 0x31, 0xf7, 0x2b, 0x05,
};

uint8_t kInitCmd0[] = {0x03, 0x91, 0x01, 0x0d, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t kInitCmd1[] = {0x07, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd2[] = {0x16, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd3[] = {0x15, 0x91, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00};
const uint8_t kInitCmd4[] = {0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00};
const uint8_t kInitCmd5[] = {0x11, 0x91, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd6[] = {0x0a, 0x91, 0x01, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd7[] = {0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00};
const uint8_t kInitCmd8[] = {0x03, 0x91, 0x01, 0x0a, 0x00, 0x04, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00};
const uint8_t kInitCmd9[] = {0x10, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd10[] = {0x01, 0x91, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd11[] = {0x01, 0x91, 0x01, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd12[] = {0x09, 0x91, 0x01, 0x07, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t kInitCmd13[] = {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x09, 0x7e, 0x00, 0x00, 0xa8, 0x30, 0x01, 0x00};
const uint8_t kInitCmd14[] = {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x09, 0x7e, 0x00, 0x00, 0xe8, 0x30, 0x01, 0x00};

const InitCommand kInitCommands[] = {
    {"INIT", kInitCmd0, sizeof(kInitCmd0)},
    {"CMD_07", kInitCmd1, sizeof(kInitCmd1)},
    {"CMD_16", kInitCmd2, sizeof(kInitCmd2)},
    {"CMD_15_03", kInitCmd3, sizeof(kInitCmd3)},
    {"FEATSEL_SET_MASK", kInitCmd4, sizeof(kInitCmd4)},
    {"CMD_11", kInitCmd5, sizeof(kInitCmd5)},
    {"VIBRATE_CFG", kInitCmd6, sizeof(kInitCmd6)},
    {"FEATSEL_ENABLE", kInitCmd7, sizeof(kInitCmd7)},
    {"SELECT_REPORT", kInitCmd8, sizeof(kInitCmd8)},
    {"FW_INFO_GET", kInitCmd9, sizeof(kInitCmd9)},
    {"CMD_01_0C", kInitCmd10, sizeof(kInitCmd10)},
    {"RUMBLE_ENABLE", kInitCmd11, sizeof(kInitCmd11)},
    {"SET_PLAYER_LED", kInitCmd12, sizeof(kInitCmd12)},
    {"CALIB_LEFT", kInitCmd13, sizeof(kInitCmd13)},
    {"CALIB_RIGHT", kInitCmd14, sizeof(kInitCmd14)},
};

} // namespace

bool gatt_uuid128_equals(const uint8_t a[kUuid128Len], const uint8_t b[kUuid128Len]) {
    return a != nullptr && b != nullptr && std::memcmp(a, b, kUuid128Len) == 0;
}

GattRole gatt_classify_uuid(const uint8_t uuid128_be[kUuid128Len]) {
    if (uuid128_be == nullptr) {
        return GattRole::Other;
    }
    if (gatt_uuid128_equals(uuid128_be, kUuidAck)) {
        return GattRole::AckNotify;
    }
    if (gatt_uuid128_equals(uuid128_be, kUuidNotifyFd2) ||
        gatt_uuid128_equals(uuid128_be, kUuidNotifyLegacy)) {
        return GattRole::InputNotify;
    }
    if (gatt_uuid128_equals(uuid128_be, kUuidCommand)) {
        return GattRole::Command;
    }
    if (gatt_uuid128_equals(uuid128_be, kUuidRumble)) {
        return GattRole::Rumble;
    }
    return GattRole::Other;
}

const char *gatt_role_name(GattRole role) {
    switch (role) {
    case GattRole::AckNotify:
        return "ack";
    case GattRole::InputNotify:
        return "input";
    case GattRole::Command:
        return "command";
    case GattRole::Rumble:
        return "rumble";
    case GattRole::Other:
    default:
        return "other";
    }
}

void gatt_format_uuid(const uint8_t uuid128_be[kUuid128Len], char *out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (uuid128_be == nullptr) {
        std::snprintf(out, out_len, "<none>");
        return;
    }
    std::snprintf(out,
                  out_len,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  uuid128_be[0], uuid128_be[1], uuid128_be[2], uuid128_be[3],
                  uuid128_be[4], uuid128_be[5], uuid128_be[6], uuid128_be[7],
                  uuid128_be[8], uuid128_be[9], uuid128_be[10], uuid128_be[11],
                  uuid128_be[12], uuid128_be[13], uuid128_be[14], uuid128_be[15]);
}

void gatt_set_console_mac(const uint8_t mac[6]) {
    if (mac != nullptr) {
        std::memcpy(&kInitCmd0[10], mac, 6);
    }
}

uint8_t gatt_init_command_count() {
    return static_cast<uint8_t>(sizeof(kInitCommands) / sizeof(kInitCommands[0]));
}

const InitCommand &gatt_init_command(uint8_t index) {
    if (index >= gatt_init_command_count()) {
        index = static_cast<uint8_t>(gatt_init_command_count() - 1);
    }
    return kInitCommands[index];
}

const uint8_t *gatt_fd2_uuid() {
    return kUuidNotifyFd2;
}

const uint8_t *gatt_legacy_uuid() {
    return kUuidNotifyLegacy;
}

} // namespace ns2
