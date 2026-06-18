# Browser module (text-stripper web reader)

idf_component_register(
    SRCS
        "src/BrowserModule.cpp"
        "src/BrowserController.cpp"
        "src/BrowserPageView.cpp"
        "src/PageFetcher.cpp"
        "src/Bookmarks.cpp"
        "src/HtmlTokenizer.cpp"
        "src/ContentExtractor.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_ui
        cdc_views
        cdc_hal
        cdc_log
        esp_http_client
        esp-tls
        mbedtls
        nvs_flash
)
