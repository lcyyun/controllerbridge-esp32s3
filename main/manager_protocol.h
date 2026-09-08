#pragma once

#include <cstddef>
#include <cstdint>

namespace ns2::manager {

constexpr uint8_t kProtocolMajor = 1;
constexpr uint8_t kProtocolMinor = 0;
constexpr uint8_t kFeatureReportId = 0x01;
constexpr uint8_t kInterruptInReportId = 0x02;
constexpr uint8_t kInterruptOutReportId = 0x03;
constexpr size_t kFeaturePayloadLength = 63;
constexpr size_t kInterruptPayloadLength = 7;
constexpr size_t kHeaderLength = 22;
constexpr uint16_t kMaxBodyLength = 512;

enum class Opcode : uint16_t {
    Hello = 0x0001,
    Capabilities = 0x0002,
    Status = 0x0003,
    Echo = 0x0004,
    SetPersonality = 0x0005,
};

enum class StatusCode : uint16_t {
    Ok = 0,
    InvalidHeader = 1,
    UnsupportedVersion = 2,
    UnsupportedOpcode = 3,
    InvalidFragment = 4,
    PayloadTooLarge = 5,
    InternalError = 6,
    InvalidArgument = 7,
};

using PersonalityRequestHandler = StatusCode (*)(uint8_t personality);

struct RuntimeStatus {
    uint8_t personality = 0;
    bool usb_mounted = false;
    bool usb_suspended = false;
    uint32_t reports_sent = 0;
    uint32_t reports_failed = 0;
};

void init();
const uint8_t *device_id();
size_t device_id_size();
const char *device_id_hex();

void receive_feature_request(const uint8_t *payload,
                             uint16_t payload_size,
                             const RuntimeStatus &runtime_status,
                             PersonalityRequestHandler personality_handler = nullptr);
uint16_t build_feature_response(uint8_t *buffer, uint16_t request_length);
void receive_interrupt_out(const uint8_t *payload, uint16_t payload_size);

} // namespace ns2::manager
