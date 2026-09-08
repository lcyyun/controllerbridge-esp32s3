#include "ns2_usb_xinput_driver.h"

#include <algorithm>
#include <cstring>

#include "device/usbd_pvt.h"
#include "tusb.h"

namespace {

constexpr uint8_t kXInputDescriptorType = 0x21;
constexpr uint8_t kMicrosoftVendorCode = 0xcd;
constexpr uint16_t kXInputOutputSize = 32;
constexpr uint16_t kXInputInputMax = 32;

const uint8_t kXusb20CompatibleId[] = {
    0x28, 0x00, 0x00, 0x00, // dwLength
    0x00, 0x01,             // bcdVersion 1.00
    0x04, 0x00,             // wIndex compatible ID
    0x01,                   // one function
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01,             // interface 0
    'X', 'U', 'S', 'B', '2', '0', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static_assert(sizeof(kXusb20CompatibleId) == 0x28);

uint8_t s_endpoint_in = 0;
uint8_t s_endpoint_out = 0;
alignas(4) uint8_t s_input_buffer[kXInputInputMax] = {};
alignas(4) uint8_t s_output_buffer[kXInputOutputSize] = {};

extern "C" void ns2_xinput_output_received(const uint8_t *data, uint16_t length);

void driver_init() {
    s_endpoint_in = 0;
    s_endpoint_out = 0;
    std::memset(s_input_buffer, 0, sizeof(s_input_buffer));
    std::memset(s_output_buffer, 0, sizeof(s_output_buffer));
}

bool driver_deinit() {
    driver_init();
    return true;
}

void driver_reset(uint8_t rhport) {
    (void)rhport;
    driver_init();
}

uint16_t driver_open(uint8_t rhport,
                     const tusb_desc_interface_t *interface_descriptor,
                     uint16_t max_length) {
    if (interface_descriptor == nullptr ||
        interface_descriptor->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC ||
        interface_descriptor->bInterfaceSubClass != 0x5d ||
        interface_descriptor->bInterfaceProtocol != 0x01 ||
        interface_descriptor->bNumEndpoints != 2) {
        return 0;
    }

    auto *cursor = reinterpret_cast<const uint8_t *>(tu_desc_next(interface_descriptor));
    if (max_length < sizeof(tusb_desc_interface_t) + 16 + 2 * sizeof(tusb_desc_endpoint_t) ||
        cursor[0] != 16 || cursor[1] != kXInputDescriptorType) {
        return 0;
    }

    const uint16_t consumed = static_cast<uint16_t>(
        sizeof(tusb_desc_interface_t) + cursor[0] +
        interface_descriptor->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    cursor += cursor[0];
    if (!usbd_open_edpt_pair(rhport,
                             cursor,
                             interface_descriptor->bNumEndpoints,
                             TUSB_XFER_INTERRUPT,
                             &s_endpoint_out,
                             &s_endpoint_in)) {
        driver_init();
        return 0;
    }

    if (s_endpoint_out != 0) {
        usbd_edpt_xfer(rhport, s_endpoint_out, s_output_buffer, sizeof(s_output_buffer));
    }
    return consumed;
}

bool driver_control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request) {
    if (stage != CONTROL_STAGE_SETUP || request == nullptr ||
        request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bRequest != kMicrosoftVendorCode || request->wIndex != 0x0004) {
        return false;
    }
    return tud_control_xfer(rhport,
                            request,
                            const_cast<uint8_t *>(kXusb20CompatibleId),
                            sizeof(kXusb20CompatibleId));
}

bool driver_transfer(uint8_t rhport,
                     uint8_t endpoint,
                     xfer_result_t result,
                     uint32_t transferred) {
    if (endpoint != s_endpoint_out) {
        return endpoint == s_endpoint_in;
    }

    if (result == XFER_RESULT_SUCCESS && transferred > 0) {
        ns2_xinput_output_received(
            s_output_buffer,
            static_cast<uint16_t>(std::min<uint32_t>(transferred, sizeof(s_output_buffer))));
    }
    std::memset(s_output_buffer, 0, sizeof(s_output_buffer));
    return usbd_edpt_xfer(rhport, s_endpoint_out, s_output_buffer, sizeof(s_output_buffer));
}

const usbd_class_driver_t kDriver = {
    .name = "NS2-XINPUT",
    .init = driver_init,
    .deinit = driver_deinit,
    .reset = driver_reset,
    .open = driver_open,
    .control_xfer_cb = driver_control,
    .xfer_cb = driver_transfer,
    .xfer_isr = nullptr,
    .sof = nullptr,
};

} // namespace

const usbd_class_driver_t *ns2_xinput_usb_driver() {
    return &kDriver;
}

bool ns2_xinput_usb_send(const uint8_t *report, uint16_t length) {
    if (report == nullptr || length == 0 || length > sizeof(s_input_buffer) ||
        s_endpoint_in == 0 || usbd_edpt_busy(0, s_endpoint_in)) {
        return false;
    }
    std::memcpy(s_input_buffer, report, length);
    return usbd_edpt_xfer(0, s_endpoint_in, s_input_buffer, length);
}
