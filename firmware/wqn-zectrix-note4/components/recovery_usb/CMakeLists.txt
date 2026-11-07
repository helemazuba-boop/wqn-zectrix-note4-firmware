idf_component_register(SRCS "recovery_usb.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_driver_usb_serial_jtag esp_timer esp_rom log vfs)
