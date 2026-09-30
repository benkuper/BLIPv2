cmake_minimum_required(VERSION 3.22)

# Keep generated configuration isolated inside each selected build directory.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
option(BLIP_ENABLE_ARTNET "Include Art-Net discovery and DMX input" ON)
option(BLIP_ENABLE_DDP "Include DDP pixel input" ON)
option(BLIP_ENABLE_E131 "Include E1.31/sACN DMX input" OFF)
option(BLIP_BOARD_ADAFRUIT_HUZZAH32 "Use the HUZZAH32 board battery profile" OFF)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_resources"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_storage"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_transport"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_network"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_led"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_ota"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_oscquery")
if(BLIP_ENABLE_ARTNET)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../components/blip_artnet")
endif()
if(BLIP_ENABLE_DDP)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../components/blip_ddp")
endif()
if(BLIP_ENABLE_E131)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../components/blip_e131")
endif()
if(BLIP_BOARD_ADAFRUIT_HUZZAH32)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../components/blip_power")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF expands component requirements in a separate CMake script process.
# Mirror these options into build properties so both passes see the same values.
idf_build_set_property(BLIP_ENABLE_ARTNET ${BLIP_ENABLE_ARTNET})
idf_build_set_property(BLIP_ENABLE_DDP ${BLIP_ENABLE_DDP})
idf_build_set_property(BLIP_ENABLE_E131 ${BLIP_ENABLE_E131})
idf_build_set_property(BLIP_BOARD_ADAFRUIT_HUZZAH32 ${BLIP_BOARD_ADAFRUIT_HUZZAH32})

# Keep the bootstrap image limited to main and its transitive requirements.
idf_build_set_property(MINIMAL_BUILD ON)

# Conditional dependencies must never make targets rewrite one shared lock.
idf_build_set_property(
    DEPENDENCIES_LOCK
    "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")

project(blip-v2 VERSION 0.1.0)

# A full factory install must carry a valid empty LittleFS image. The storage
# component then installs its embedded, integrity-checked browser bundle on the
# first boot. App-only OTA builds continue to leave this partition untouched.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/factory_storage")
littlefs_create_partition_image(storage "${CMAKE_BINARY_DIR}/factory_storage" FLASH_IN_PROJECT)
