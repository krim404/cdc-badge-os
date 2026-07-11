# 2FA Module

idf_component_register(
    SRCS
        "src/TwoFaModule.cpp"
        "src/OathStore.cpp"
        "src/OathApplet.cpp"
        "src/OathBackend.cpp"
        "src/ble_chalresp.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/components/serial_cmd/include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        cdc_scard
        serial_cmd
        cdc_log
        mbedtls
        nvs_flash
        freertos
)
