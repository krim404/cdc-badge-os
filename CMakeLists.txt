cmake_minimum_required(VERSION 3.16.0)

# Patch CalEPD for CDC Badge (MISO support + minimal build)
set(CALEPD_DIR "${CMAKE_SOURCE_DIR}/components/CalEPD")
set(CALEPD_PATCH_MARKER "${CALEPD_DIR}/.patched")

if(NOT EXISTS ${CALEPD_PATCH_MARKER})
    # Overwrite CMakeLists.txt with minimal version (only GDEY029T94)
    file(WRITE "${CALEPD_DIR}/CMakeLists.txt"
"# Minimal CalEPD for CDC Badge (only GDEY029T94)
set(srcs
    \"models/goodisplay/gdey029T94.cpp\"
    \"epd.cpp\"
    \"epdspi.cpp\"
)

idf_component_register(SRCS \${srcs}
    REQUIRES \"Adafruit-GFX\" esp_timer driver
    INCLUDE_DIRS \"include\"
)
")
    message(STATUS "CalEPD: minimal CMakeLists.txt written")

    # Apply MISO patch for TROPIC01 on shared SPI bus
    set(PATCH_MISO "${CMAKE_SOURCE_DIR}/patches/calepd_spi_miso.patch")
    if(EXISTS ${PATCH_MISO})
        execute_process(
            COMMAND patch -p1 -N -d ${CALEPD_DIR} -i ${PATCH_MISO}
            RESULT_VARIABLE PATCH_RESULT
        )
        if(PATCH_RESULT EQUAL 0)
            message(STATUS "CalEPD: MISO patch applied")
        endif()
    endif()

    file(WRITE ${CALEPD_PATCH_MARKER} "Patched for CDC Badge")
endif()

# Patch CalEPD busy-wait: the SSD1680 full-refresh waveform takes ~2 s, the
# upstream 1 s timeout returns mid-refresh and the next update's panel reset
# aborts the running waveform (visible as partially rendered screen areas)
set(CALEPD_BUSY_PATCH_MARKER "${CALEPD_DIR}/.patched_busy_timeout")

if(NOT EXISTS ${CALEPD_BUSY_PATCH_MARKER})
    set(PATCH_BUSY "${CMAKE_SOURCE_DIR}/patches/calepd_busy_timeout.patch")
    if(EXISTS ${PATCH_BUSY})
        execute_process(
            COMMAND patch -p1 -N -d ${CALEPD_DIR} -i ${PATCH_BUSY}
            RESULT_VARIABLE PATCH_RESULT
        )
        if(PATCH_RESULT EQUAL 0)
            message(STATUS "CalEPD: busy-timeout patch applied")
        endif()
    endif()

    file(WRITE ${CALEPD_BUSY_PATCH_MARKER} "Patched busy timeout")
endif()

# Patch WAMR ESP-IDF platform port: os_self_thread() must work from any
# FreeRTOS task, not only pthreads
set(WAMR_DIR "${CMAKE_SOURCE_DIR}/components/wamr_runtime/wasm-micro-runtime")
set(WAMR_PATCH_MARKER "${WAMR_DIR}/.patched")

if(NOT EXISTS ${WAMR_PATCH_MARKER})
    set(PATCH_WAMR "${CMAKE_SOURCE_DIR}/patches/wamr_espidf_self_thread.patch")
    if(EXISTS ${PATCH_WAMR})
        execute_process(
            COMMAND patch -p1 -N -d ${WAMR_DIR} -i ${PATCH_WAMR}
            RESULT_VARIABLE PATCH_RESULT
        )
        if(PATCH_RESULT EQUAL 0)
            message(STATUS "WAMR: os_self_thread FreeRTOS patch applied")
        endif()
    endif()

    file(WRITE ${WAMR_PATCH_MARKER} "Patched for FreeRTOS task self")
endif()

# Exclude upstream or legacy components
set(EXCLUDE_COMPONENTS libtropic)

# Global include directories (for tusb_config.h)
include_directories(BEFORE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/main
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cdc-badge-os)
