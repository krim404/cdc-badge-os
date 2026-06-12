# USB Badge Component - TinyUSB CDC Serial

idf_component_register(
    SRCS "usb_cdc.cpp" "usb_hid.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES cdc_core cdc_log freertos driver espressif__tinyusb
    PRIV_REQUIRES usb
)
