cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mibee_cam)

# Embed web UI files into SPIFFS partition
file(GLOB MIBEE_UI_FILES main/web_ui/*)
# 2026-09-03：目录级依赖不随文件内容编辑触发（build/spiffs.bin 比新 app.js 旧），
# 必须显式 DEPENDS 文件列表，否则固件烧的是旧 UI
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT DEPENDS ${MIBEE_UI_FILES})

# Apply local patch to managed component (managed_components/ is gitignored).
# led_strip 2.5.5 uses MALLOC_CAP_* without including esp_heap_caps.h, which
# fails to compile under IDF v6.0.1. Source-only patch: read at compile time,
# so it applies deterministically on fresh clones and in CI.
set(LED_STRIP_PATCH_DIR ${CMAKE_SOURCE_DIR}/patches/espressif__led_strip)
set(LED_STRIP_MANAGED_DIR ${CMAKE_SOURCE_DIR}/managed_components/espressif__led_strip)
if(EXISTS ${LED_STRIP_MANAGED_DIR}/src/led_strip_spi_dev.c AND EXISTS ${LED_STRIP_PATCH_DIR}/src/led_strip_spi_dev.c)
    file(COPY ${LED_STRIP_PATCH_DIR}/src/led_strip_spi_dev.c DESTINATION ${LED_STRIP_MANAGED_DIR}/src)
    message(STATUS "Applied led_strip patch (esp_heap_caps.h include for IDF v6)")
endif()
