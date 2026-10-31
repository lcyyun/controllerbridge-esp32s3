#pragma once

#include <cstdint>

#include "esp_err.h"
#include "manager_protocol.h"

namespace ns2 {

esp_err_t usb_start();
void usb_task();
// Called from the application task; never execute NVS/BLE commands in a USB callback.
size_t usb_execute_manager_command(const char *command, char *out, size_t capacity);
manager::StatusCode usb_manager_request_mode(uint8_t personality);
void usb_uac1_link_driver();
void usb_submit_dualsense_audio_packet(const uint8_t *data, uint16_t len);
void usb_note_uac1_interface(uint8_t interface_number, uint8_t alt_setting);

} // namespace ns2
