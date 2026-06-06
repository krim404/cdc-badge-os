# WAMR (WebAssembly Micro Runtime) - ESP-IDF wrapper for CDC Badge OS.
#
# Wraps the upstream wasm-micro-runtime submodule so it can be used as a normal
# ESP-IDF component. Configured for the Classic Interpreter on ESP32-S3
# (XTENSA) and routes all WAMR allocations through a custom allocator that
# lives in PSRAM. See src/wamr_psram_alloc.c.

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(WAMR_BUILD_PLATFORM "esp-idf")
    set(WAMR_BUILD_TARGET "XTENSA")
    set(CMAKE_BUILD_TYPE Release)

    set(WAMR_BUILD_INTERP 1)
    set(WAMR_BUILD_FAST_INTERP 1)
    set(WAMR_BUILD_AOT 1)
    set(WAMR_BUILD_JIT 0)
    set(WAMR_BUILD_LIBC_BUILTIN 1)
    set(WAMR_BUILD_LIBC_WASI 0)
    set(WAMR_BUILD_MULTI_MODULE 0)
    set(WAMR_BUILD_SHARED_MEMORY 0)
    set(WAMR_BUILD_MINI_LOADER 0)
    set(WAMR_BUILD_REF_TYPES 1)
    set(WAMR_BUILD_BULK_MEMORY 1)
    set(WAMR_BUILD_LIB_PTHREAD 0)
    set(WAMR_BUILD_INSTRUCTION_METERING 1)

    set(WAMR_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/wasm-micro-runtime)
    include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

    # WAMR's shared_platform.cmake sets WASM_MEM_DUAL_BUS_MIRROR=1 whenever
    # PSRAM is on. That branch computes IBus addresses with the original
    # ESP32's PSRAM mapping (SOC_IROM_LOW - SOC_IROM_HIGH) and produces
    # invalid pointers on ESP32-S3 (the .aot ends up jumping to 0x3A...).
    # Force AOT mmap back to internal SRAM (MALLOC_CAP_EXEC).
    remove_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1)
    add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=0)

    list(APPEND srcs
        ${WAMR_RUNTIME_LIB_SOURCE}
        ${PLATFORM_SHARED_SOURCE}
        ${CMAKE_CURRENT_LIST_DIR}/src/wamr_psram_alloc.c
        ${CMAKE_CURRENT_LIST_DIR}/src/Wamr.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/wamr_log.cpp
    )

    set(include_dirs
        ${IWASM_DIR}/include
        ${UTILS_SHARED_DIR}
        ${PLATFORM_SHARED_DIR}
        ${PLATFORM_SHARED_DIR}/../include
        ${IWASM_COMMON_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/include
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES pthread lwip esp_timer cdc_log
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-unused-function)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    -DBUILD_TARGET_XTENSA=1
    -DWASM_ENABLE_INTERP=1
    -DWASM_ENABLE_CUSTOM_NAME_SECTION=1
)
