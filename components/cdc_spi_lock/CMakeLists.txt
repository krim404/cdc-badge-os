# Leaf primitive: a recursive mutex serializing the shared SPI bus (display +
# TROPIC01). Depends only on freertos so both cdc_hal and CalEPD can use it
# without a dependency cycle.

idf_component_register(
    SRCS
        "cdc_spi_lock.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
)
