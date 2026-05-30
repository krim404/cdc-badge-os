# vFAT module - file explorer + serial shell for the plugins FAT partition.

idf_component_register(
    SRCS
        "src/VfatModule.cpp"
        "src/VfatFs.cpp"
        "src/VfatExplorerView.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_log
        serial_cmd
        plugin_manager
)
