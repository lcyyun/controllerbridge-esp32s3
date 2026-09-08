idf_component_register(
    SRCS
        "app_main.cpp"
        "ns2_board.cpp"
        "ns2_ble.cpp"
        "ns2_config.cpp"
        "ns2_gatt.cpp"
        "ns2_input.cpp"
        "ns2_protocol.cpp"
        "ns2_usb.cpp"
        "ns2_usb_xinput_driver.cpp"
        "ns2_usb_uac1.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt driver esp_tinyusb led_strip nvs_flash
)

# esp_tinyusb v1.x rejects string descriptor index 0xEE before application
# code can answer it.  Interpose the callback so Microsoft OS 1.0 compatible
# IDs (WINUSB/XUSB20) remain usable without patching the managed component.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=tud_descriptor_string_cb")
