# vCard + Beacon Module

idf_component_register(
    SRCS
        "src/VcardModule.cpp"
        "src/VcardWizard.cpp"
        "src/vcard_store.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_log
        cdc_msg
        nvs_flash
        cdc_hal
        serial_cmd
        freertos
)
