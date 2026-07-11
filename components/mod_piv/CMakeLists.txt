# PIV Module - NIST SP 800-73-4 smart-card applet over the shared CCID interface

idf_component_register(
    SRCS
        "src/PivModule.cpp"
        "src/piv_applet.cpp"
        "src/piv_objects.cpp"
        "src/piv_tlv.cpp"
        "src/piv_keys.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        cdc_core
        cdc_scard
        cdc_hal
        cdc_log
    PRIV_REQUIRES
        mbedtls
        nvs_flash
)
