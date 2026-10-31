#pragma once

#include <cstddef>
#include <cstdint>

namespace ns2 {

constexpr uint8_t kUuid128Len = 16;

enum class GattRole : uint8_t {
    Other = 0,
    AckNotify,
    InputNotify,
    Command,
    Rumble,
};

struct InitCommand {
    const char *name;
    const uint8_t *data;
    uint16_t len;
};

GattRole gatt_classify_uuid(const uint8_t uuid128_be[kUuid128Len]);
bool gatt_uuid128_equals(const uint8_t a[kUuid128Len], const uint8_t b[kUuid128Len]);
const char *gatt_role_name(GattRole role);
void gatt_format_uuid(const uint8_t uuid128_be[kUuid128Len], char *out, size_t out_len);
void gatt_set_console_mac(const uint8_t mac[6]);
uint8_t gatt_init_command_count();
const InitCommand &gatt_init_command(uint8_t index);
const uint8_t *gatt_fd2_uuid();
const uint8_t *gatt_legacy_uuid();
constexpr uint8_t kPairAddress = 0x01;
constexpr uint8_t kPairChallenge = 0x02;
constexpr uint8_t kPairFinalize = 0x03;
constexpr uint8_t kPairKey = 0x04;
size_t gatt_build_pair_command(uint8_t *out, size_t capacity, uint8_t subcommand,
                               const uint8_t *value, size_t value_len);
bool gatt_parse_pair_response(const uint8_t *data, size_t len, uint8_t expected,
                              const uint8_t **payload);
// Internal NVS format. Never include this key-bearing record in diagnostics.
struct PairingRecord {
    uint32_t magic = 0x3150324e;
    uint8_t valid = 0;
    uint8_t addr_type = 0;
    uint8_t addr[6] = {};
    uint8_t ltk[16] = {};
};
static_assert(sizeof(PairingRecord) == 28, "Pairing NVS format must remain stable");
bool gatt_pairing_record_valid(const PairingRecord &record, size_t size);
void gatt_derive_pair_key(const uint8_t host[16], const uint8_t peer[16], uint8_t ltk[16]);
void gatt_pair_challenge_inputs(const uint8_t ltk[16], const uint8_t challenge[16],
                                uint8_t key_be[16], uint8_t challenge_be[16]);
bool gatt_pair_challenge_matches(const uint8_t expected[16], const uint8_t actual[16]);

} // namespace ns2
