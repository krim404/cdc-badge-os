# FIDO2 Module (WebAuthn)

idf_component_register(
    SRCS
        "src/cbor_helpers.cpp"
        "src/ctap2.cpp"
        "src/ctaphid.cpp"
        "src/fido2.cpp"
        "src/fido2_storage.cpp"
        "src/LargeBlobStore.cpp"
        "src/Fido2Module.cpp"
        "src/Fido2Ui.cpp"
        "src/u2f.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_log
        freertos
        usb_badge
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        serial_cmd
        mbedtls
)
