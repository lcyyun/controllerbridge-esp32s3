#include "manager_protocol.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ns2_usb.h"

namespace ns2::manager {
namespace {

constexpr char kTag[] = "ns2-manager";
constexpr uint8_t kMagic[] = {'N', 'S', 'M', '1'};
constexpr uint8_t kMessageRequest = 0;
constexpr uint8_t kMessageResponse = 1;
constexpr uint8_t kFlagMore = 0x01;
constexpr uint8_t kFlagError = 0x02;
constexpr char kProduct[] = "ns2bridge-esp32s3";
constexpr std::array<uint8_t, 10> kDeviceIdPrefix = {
    'n', 's', '2', 'b', 'r', 'i', 'd', 'g', 'e', 0x01,
};

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
struct Guard { Guard() { portENTER_CRITICAL(&s_lock); } ~Guard() { portEXIT_CRITICAL(&s_lock); } };

struct ProtocolState {
    std::array<uint8_t, kMaxRequestLength + 1> request{};
    uint16_t request_length = 0;
    uint16_t request_offset = 0;
    uint32_t request_transaction = 0;
    uint16_t request_opcode = 0;
    int64_t request_time_us = 0;
    bool request_ready = false;
    bool processing = false;
    RuntimeStatus runtime{};
    PersonalityRequestHandler personality_handler = nullptr;
    std::array<uint8_t, 16> device_id{};
    std::array<char, 33> device_id_hex{};
    std::array<uint8_t, kMaxBodyLength> reply{};
    uint16_t reply_length = 0;
    uint16_t reply_offset = 0;
    uint32_t transaction_id = 0;
    uint16_t opcode = 0;
    StatusCode status = StatusCode::Ok;
    bool reply_ready = false;
};

ProtocolState s_protocol;

uint16_t read_le16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t read_le32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void write_le16(uint8_t *data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value & 0xff);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void write_le32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value & 0xff);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

class CborWriter {
public:
    CborWriter(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    bool map(size_t count) { return type_value(5, count); }
    bool array(size_t count) { return type_value(4, count); }
    bool unsigned_value(uint64_t value) { return type_value(0, value); }

    bool boolean(bool value) {
        return byte(value ? 0xf5 : 0xf4);
    }

    bool text(const char *value) {
        if (value == nullptr) {
            return false;
        }
        const size_t length = std::strlen(value);
        return type_value(3, length) && bytes(reinterpret_cast<const uint8_t *>(value), length);
    }

    bool byte_string(const uint8_t *value, size_t length) {
        return value != nullptr && type_value(2, length) && bytes(value, length);
    }

    size_t size() const { return size_; }
    bool ok() const { return ok_; }

private:
    bool byte(uint8_t value) {
        if (!ok_ || size_ >= capacity_) {
            ok_ = false;
            return false;
        }
        buffer_[size_++] = value;
        return true;
    }

    bool bytes(const uint8_t *value, size_t length) {
        if (!ok_ || value == nullptr || length > capacity_ - size_) {
            ok_ = false;
            return false;
        }
        std::memcpy(buffer_ + size_, value, length);
        size_ += length;
        return true;
    }

    bool type_value(uint8_t major, uint64_t value) {
        if (value <= 23) {
            return byte(static_cast<uint8_t>((major << 5) | value));
        }
        if (value <= 0xff) {
            return byte(static_cast<uint8_t>((major << 5) | 24)) &&
                   byte(static_cast<uint8_t>(value));
        }
        if (value <= 0xffff) {
            return byte(static_cast<uint8_t>((major << 5) | 25)) &&
                   byte(static_cast<uint8_t>((value >> 8) & 0xff)) &&
                   byte(static_cast<uint8_t>(value & 0xff));
        }
        if (value <= 0xffffffffULL) {
            return byte(static_cast<uint8_t>((major << 5) | 26)) &&
                   byte(static_cast<uint8_t>((value >> 24) & 0xff)) &&
                   byte(static_cast<uint8_t>((value >> 16) & 0xff)) &&
                   byte(static_cast<uint8_t>((value >> 8) & 0xff)) &&
                   byte(static_cast<uint8_t>(value & 0xff));
        }
        return false;
    }

    uint8_t *buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t size_ = 0;
    bool ok_ = true;
};

void queue_response(uint32_t transaction_id,
                    uint16_t opcode,
                    StatusCode status,
                    const uint8_t *body,
                    size_t body_length) {
    Guard guard;
    s_protocol.transaction_id = transaction_id;
    s_protocol.opcode = opcode;
    s_protocol.status = status;
    s_protocol.reply_offset = 0;
    s_protocol.reply_length = static_cast<uint16_t>(std::min<size_t>(body_length, s_protocol.reply.size()));
    if (s_protocol.reply_length > 0 && body != nullptr) {
        std::memcpy(s_protocol.reply.data(), body, s_protocol.reply_length);
    }
    s_protocol.reply_ready = true;
}

void queue_error(uint32_t transaction_id,
                 uint16_t opcode,
                 StatusCode status,
                 const char *message) {
    std::array<uint8_t, 128> body{};
    CborWriter writer(body.data(), body.size());
    writer.map(2);
    writer.unsigned_value(0);
    writer.unsigned_value(static_cast<uint16_t>(status));
    writer.unsigned_value(1);
    writer.text(message);
    queue_response(transaction_id,
                   opcode,
                   status,
                   writer.ok() ? body.data() : nullptr,
                   writer.ok() ? writer.size() : 0);
}

void queue_hello(uint32_t transaction_id, uint16_t opcode, const RuntimeStatus &runtime_status) {
    std::array<uint8_t, 256> body{};
    CborWriter writer(body.data(), body.size());
    const esp_app_desc_t *app = esp_app_get_description();
    const char *version = app != nullptr && app->version[0] != 0 ? app->version : "unknown";

    writer.map(7);
    writer.unsigned_value(0);
    writer.text(kProduct);
    writer.unsigned_value(1);
    writer.byte_string(s_protocol.device_id.data(), s_protocol.device_id.size());
    writer.unsigned_value(2);
    writer.unsigned_value(runtime_status.personality);
    writer.unsigned_value(3);
    writer.text(version);
    writer.unsigned_value(4);
    writer.unsigned_value(kProtocolMajor);
    writer.unsigned_value(5);
    writer.unsigned_value(kProtocolMinor);
    writer.unsigned_value(6);
    writer.array(6);
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Hello));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Capabilities));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Status));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Echo));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::SetPersonality));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::ExecuteCommand));

    if (!writer.ok()) {
        queue_error(transaction_id, opcode, StatusCode::InternalError, "hello_encode_failed");
        return;
    }
    queue_response(transaction_id, opcode, StatusCode::Ok, body.data(), writer.size());
}

void queue_capabilities(uint32_t transaction_id, uint16_t opcode) {
    std::array<uint8_t, 64> body{};
    CborWriter writer(body.data(), body.size());
    writer.map(3);
    writer.unsigned_value(0);
    writer.array(6);
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Hello));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Capabilities));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Status));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::Echo));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::SetPersonality));
    writer.unsigned_value(static_cast<uint16_t>(Opcode::ExecuteCommand));
    writer.unsigned_value(1);
    writer.unsigned_value(kFeaturePayloadLength - kHeaderLength);
    writer.unsigned_value(2);
    writer.unsigned_value(kInterruptPayloadLength);
    queue_response(transaction_id,
                   opcode,
                   writer.ok() ? StatusCode::Ok : StatusCode::InternalError,
                   writer.ok() ? body.data() : nullptr,
                   writer.ok() ? writer.size() : 0);
}

void queue_status(uint32_t transaction_id, uint16_t opcode, const RuntimeStatus &runtime_status) {
    std::array<uint8_t, 96> body{};
    CborWriter writer(body.data(), body.size());
    writer.map(5);
    writer.unsigned_value(0);
    writer.unsigned_value(runtime_status.personality);
    writer.unsigned_value(1);
    writer.boolean(runtime_status.usb_mounted);
    writer.unsigned_value(2);
    writer.boolean(runtime_status.usb_suspended);
    writer.unsigned_value(3);
    writer.unsigned_value(runtime_status.reports_sent);
    writer.unsigned_value(4);
    writer.unsigned_value(runtime_status.reports_failed);
    queue_response(transaction_id,
                   opcode,
                   writer.ok() ? StatusCode::Ok : StatusCode::InternalError,
                   writer.ok() ? body.data() : nullptr,
                   writer.ok() ? writer.size() : 0);
}

} // namespace

void init() {
    std::array<uint8_t, 6> mac{};
    const esp_err_t result = esp_efuse_mac_get_default(mac.data());
    std::copy(kDeviceIdPrefix.begin(), kDeviceIdPrefix.end(), s_protocol.device_id.begin());
    if (result == ESP_OK) {
        std::copy(mac.begin(), mac.end(), s_protocol.device_id.begin() + kDeviceIdPrefix.size());
    } else {
        ESP_LOGE(kTag, "Unable to read eFuse MAC: %d", static_cast<int>(result));
    }

    constexpr char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < s_protocol.device_id.size(); ++index) {
        s_protocol.device_id_hex[index * 2] = hex[s_protocol.device_id[index] >> 4];
        s_protocol.device_id_hex[index * 2 + 1] = hex[s_protocol.device_id[index] & 0x0f];
    }
    s_protocol.device_id_hex.back() = 0;
    s_protocol.reply_ready = false;
    s_protocol.request_ready = false;
    s_protocol.request_offset = 0;
    s_protocol.processing = false;
    ESP_LOGI(kTag, "Manager v1 device_id=%s", s_protocol.device_id_hex.data());
}

const uint8_t *device_id() {
    return s_protocol.device_id.data();
}

size_t device_id_size() {
    return s_protocol.device_id.size();
}

const char *device_id_hex() {
    return s_protocol.device_id_hex.data();
}

void receive_feature_request(const uint8_t *payload,
                             uint16_t payload_size,
                             const RuntimeStatus &runtime_status,
                             PersonalityRequestHandler personality_handler) {
    {
        Guard guard;
        // Preserve the active transaction even if a second client sends a
        // malformed request during execution or before its reply is consumed.
        if (s_protocol.processing || s_protocol.request_ready || s_protocol.reply_ready) return;
    }
    uint32_t transaction_id = 0;
    uint16_t opcode = 0;
    if (payload != nullptr && payload_size >= 14) {
        transaction_id = read_le32(payload + 8);
        opcode = read_le16(payload + 12);
    }
    const auto reject = [transaction_id, opcode](StatusCode status, const char *message) {
        Guard guard;
        s_protocol.request_ready = false;
        s_protocol.request_length = 0;
        s_protocol.request_offset = 0;
        s_protocol.request_transaction = transaction_id;
        s_protocol.request_opcode = opcode;
        queue_error(transaction_id, opcode, status, message);
    };

    if (payload == nullptr || payload_size < kHeaderLength ||
        std::memcmp(payload, kMagic, sizeof(kMagic)) != 0 ||
        payload[21] != kHeaderLength || payload[6] != kMessageRequest) {
        reject(StatusCode::InvalidHeader, "invalid_header");
        return;
    }
    if (payload[4] != kProtocolMajor) {
        reject(StatusCode::UnsupportedVersion, "unsupported_version");
        return;
    }

    const uint16_t total_length = read_le16(payload + 16);
    const uint16_t offset = read_le16(payload + 18);
    const uint8_t chunk_length = payload[20];
    if (total_length > kMaxRequestLength) {
        reject(StatusCode::PayloadTooLarge, "request_too_large");
        return;
    }
    if (payload_size > kFeaturePayloadLength || chunk_length > kFeaturePayloadLength - kHeaderLength ||
        offset > total_length || chunk_length > total_length - offset ||
        static_cast<size_t>(kHeaderLength + chunk_length) > payload_size ||
        ((payload[7] & kFlagMore) != 0) != (offset + chunk_length < total_length) ||
        (payload[7] & ~kFlagMore) != 0 || read_le16(payload + 14) != 0 ||
        (chunk_length == 0 && total_length != 0)) {
        reject(StatusCode::InvalidFragment, "invalid_fragment");
        return;
    }
    bool invalid = false;
    {
        Guard guard;
        // One request at a time. A client must consume the reply before issuing
        // the next command; GET reports Busy until the application loop runs.
        if (s_protocol.processing || s_protocol.request_ready) return;
        const int64_t now = esp_timer_get_time();
        if (offset == 0) {
            s_protocol.request_offset = 0;
            s_protocol.request_length = total_length;
            s_protocol.request_transaction = transaction_id;
            s_protocol.request_opcode = opcode;
            s_protocol.reply_ready = false;
        }
        invalid = offset != s_protocol.request_offset || total_length != s_protocol.request_length ||
                  transaction_id != s_protocol.request_transaction || opcode != s_protocol.request_opcode ||
                  (offset != 0 && now - s_protocol.request_time_us > 2000000);
        if (!invalid) {
            std::memcpy(s_protocol.request.data() + offset, payload + kHeaderLength, chunk_length);
            s_protocol.request_offset += chunk_length;
            s_protocol.request[s_protocol.request_offset] = 0;
            s_protocol.request_time_us = now;
            s_protocol.runtime = runtime_status;
            s_protocol.personality_handler = personality_handler;
            s_protocol.request_ready = s_protocol.request_offset == total_length;
        } else {
            s_protocol.request_offset = 0;
            s_protocol.request_length = 0;
        }
    }
    if (invalid) reject(StatusCode::InvalidFragment, "fragment_order");
}

void task() {
    uint32_t transaction_id;
    uint16_t opcode;
    uint16_t chunk_length;
    RuntimeStatus runtime_status;
    PersonalityRequestHandler personality_handler;
    {
        Guard guard;
        if (!s_protocol.request_ready || s_protocol.processing) return;
        s_protocol.request_ready = false;
        s_protocol.processing = true;
        transaction_id = s_protocol.request_transaction;
        opcode = s_protocol.request_opcode;
        chunk_length = s_protocol.request_length;
        runtime_status = s_protocol.runtime;
        personality_handler = s_protocol.personality_handler;
    }
    const uint8_t *body = s_protocol.request.data();
    if ((opcode == static_cast<uint16_t>(Opcode::Hello) ||
         opcode == static_cast<uint16_t>(Opcode::Capabilities) ||
         opcode == static_cast<uint16_t>(Opcode::Status)) && chunk_length != 0) {
        queue_error(transaction_id, opcode, StatusCode::InvalidArgument, "unexpected_body");
        Guard guard;
        s_protocol.processing = false;
        return;
    }
    switch (static_cast<Opcode>(opcode)) {
    case Opcode::Hello:
        queue_hello(transaction_id, opcode, runtime_status);
        break;
    case Opcode::Capabilities:
        queue_capabilities(transaction_id, opcode);
        break;
    case Opcode::Status:
        queue_status(transaction_id, opcode, runtime_status);
        break;
    case Opcode::Echo:
        queue_response(transaction_id, opcode, StatusCode::Ok, body, chunk_length);
        break;
    case Opcode::ExecuteCommand: {
        // Embedded NUL is rejected so transport validation and execution see
        // exactly the same command. Commands are bounded before dispatch.
        static std::array<char, kMaxBodyLength> response{};
        if (chunk_length == 0 || std::memchr(body, 0, chunk_length) != nullptr) {
            queue_error(transaction_id, opcode, StatusCode::InvalidArgument, "invalid_command");
            break;
        }
        response.fill(0);
        ns2::usb_execute_manager_command(reinterpret_cast<const char *>(body), response.data(), response.size());
        const size_t length = std::strlen(response.data());
        if (length == 0 || length >= response.size() - 1) {
            queue_error(transaction_id, opcode, StatusCode::InternalError, "command_response_overflow");
        } else {
            queue_response(transaction_id, opcode, StatusCode::Ok,
                           reinterpret_cast<const uint8_t *>(response.data()), length);
        }
        break;
    }
    case Opcode::SetPersonality: {
        if (chunk_length != 3 || body[0] != 0xa1 || body[1] != 0x00 || body[2] > 3 ||
            personality_handler == nullptr) {
            queue_error(transaction_id, opcode, StatusCode::InvalidArgument, "invalid_personality");
            break;
        }
        const StatusCode result = personality_handler(body[2]);
        if (result != StatusCode::Ok) {
            queue_error(transaction_id, opcode, result, "personality_change_failed");
            break;
        }
        std::array<uint8_t, 8> response{};
        CborWriter writer(response.data(), response.size());
        writer.map(2);
        writer.unsigned_value(0);
        writer.unsigned_value(body[2]);
        writer.unsigned_value(1);
        writer.boolean(true);
        queue_response(transaction_id,
                       opcode,
                       writer.ok() ? StatusCode::Ok : StatusCode::InternalError,
                       writer.ok() ? response.data() : nullptr,
                       writer.ok() ? writer.size() : 0);
        break;
    }
    default:
        queue_error(transaction_id, opcode, StatusCode::UnsupportedOpcode, "unsupported_opcode");
        break;
    }
    { Guard guard; s_protocol.processing = false; }
}

uint16_t build_feature_response(uint8_t *buffer, uint16_t request_length) {
    if (buffer == nullptr || request_length == 0) {
        return 0;
    }
    std::memset(buffer, 0, request_length);
    if (request_length <= kHeaderLength) {
        return 0; // cannot make progress without at least one body byte
    }
    Guard guard;
    if (!s_protocol.reply_ready) {
        // A short, non-consuming Busy response avoids blocking the USB task.
        std::memcpy(buffer, kMagic, sizeof(kMagic));
        buffer[4] = kProtocolMajor;
        buffer[5] = kProtocolMinor;
        buffer[6] = kMessageResponse;
        buffer[7] = kFlagError;
        write_le32(buffer + 8, s_protocol.request_transaction);
        write_le16(buffer + 12, s_protocol.request_opcode);
        write_le16(buffer + 14, static_cast<uint16_t>(StatusCode::Busy));
        buffer[21] = kHeaderLength;
        return request_length;
    }

    const uint16_t remaining = static_cast<uint16_t>(s_protocol.reply_length - s_protocol.reply_offset);
    const uint16_t chunk_capacity = static_cast<uint16_t>(std::min<size_t>(request_length - kHeaderLength, kFeaturePayloadLength - kHeaderLength));
    const uint8_t chunk_length = static_cast<uint8_t>(std::min<uint16_t>(remaining, chunk_capacity));
    const bool more = static_cast<uint16_t>(s_protocol.reply_offset + chunk_length) < s_protocol.reply_length;

    std::memcpy(buffer, kMagic, sizeof(kMagic));
    buffer[4] = kProtocolMajor;
    buffer[5] = kProtocolMinor;
    buffer[6] = kMessageResponse;
    buffer[7] = static_cast<uint8_t>((more ? kFlagMore : 0) |
                                     (s_protocol.status == StatusCode::Ok ? 0 : kFlagError));
    write_le32(buffer + 8, s_protocol.transaction_id);
    write_le16(buffer + 12, s_protocol.opcode);
    write_le16(buffer + 14, static_cast<uint16_t>(s_protocol.status));
    write_le16(buffer + 16, s_protocol.reply_length);
    write_le16(buffer + 18, s_protocol.reply_offset);
    buffer[20] = chunk_length;
    buffer[21] = static_cast<uint8_t>(kHeaderLength);
    if (chunk_length > 0) {
        std::memcpy(buffer + kHeaderLength,
                    s_protocol.reply.data() + s_protocol.reply_offset,
                    chunk_length);
    }

    s_protocol.reply_offset = static_cast<uint16_t>(s_protocol.reply_offset + chunk_length);
    if (!more) {
        s_protocol.reply_ready = false;
        s_protocol.reply_length = 0;
        s_protocol.reply_offset = 0;
    }
    return request_length;
}

void receive_interrupt_out(const uint8_t *payload, uint16_t payload_size) {
    if (payload != nullptr && payload_size > 0) {
        ESP_LOGD(kTag, "Interrupt OUT received: %u bytes", static_cast<unsigned>(payload_size));
    }
}

} // namespace ns2::manager
