# CDC Image - PNG/JPEG/GIF decode and Floyd-Steinberg dithering to 1-bit.
#
# Leaf component (no module references). JPEG via espressif/esp_jpeg and PNG via
# espressif/libpng, both pulled by the component manager (idf_component.yml) and
# gated by the FEATURE_IMG_* flags. The dither and format-detection logic has no
# third-party dependency.

idf_component_register(
    SRCS
        "src/Dither.cpp"
        "src/ImageFormat.cpp"
        "src/ImageDecoder.cpp"
        "src/JpegEncoder.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cdc_core
        cdc_log
        libjpeg
        libpng
)
