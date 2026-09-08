#pragma once

#include <cstdint>

#include "device/usbd_pvt.h"

// Sends one Xbox 360 style input report through the custom XInput interface.
// The custom class driver owns interface 0; TinyUSB HID owns Manager interface 1.
bool ns2_xinput_usb_send(const uint8_t *report, uint16_t length);

// Returned driver remains valid for the complete TinyUSB lifetime.
const usbd_class_driver_t *ns2_xinput_usb_driver();
