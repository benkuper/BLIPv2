cmake_minimum_required(VERSION 3.22)

# Keep generated configuration isolated inside each selected build directory.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
option(BLIP_ENABLE_BLE "Include the NimBLE control transport" OFF)
option(BLIP_ENABLE_CLASSIC_BT "Include Classic Bluetooth SPP control on ESP32" OFF)
option(BLIP_ENABLE_ESPNOW "Include ESP-NOW V2 peer control" OFF)
option(BLIP_ENABLE_FLEET "Include routerless ESP-NOW fleet synchronization and scheduled cues" OFF)
if(BLIP_BOARD_CREATORS_BALL_V2)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.ball-8mb.defaults")
endif()
if(BLIP_ENABLE_BLE AND BLIP_ENABLE_CLASSIC_BT)
    message(FATAL_ERROR "Select either NimBLE or Classic Bluetooth SPP")
endif()
if(BLIP_ENABLE_CLASSIC_BT AND NOT IDF_TARGET STREQUAL "esp32")
    message(FATAL_ERROR "Classic Bluetooth SPP requires IDF_TARGET=esp32")
endif()
set(blip_network_lighting_default ON)
if(BLIP_ENABLE_BLE)
    # Keep network lighting on the 8 MB Ball; 4 MB BLE profiles need app space.
    if(NOT BLIP_BOARD_CREATORS_BALL_V2)
        set(blip_network_lighting_default OFF)
    endif()
    if(BLIP_BOARD_CREATORS_BALL_V2)
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.ble.defaults")
    elseif(IDF_TARGET STREQUAL "esp32c6")
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.ble.defaults")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.ble-4mb.defaults")
    else()
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.ble.defaults")
    endif()
endif()
if(BLIP_ENABLE_CLASSIC_BT)
    set(blip_network_lighting_default OFF)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.classic.defaults")
endif()
option(BLIP_ENABLE_ARTNET "Include Art-Net discovery and DMX input" ${blip_network_lighting_default})
option(BLIP_ENABLE_DDP "Include DDP pixel input" ${blip_network_lighting_default})
option(BLIP_ENABLE_E131 "Include E1.31/sACN DMX input" OFF)
option(BLIP_BOARD_ADAFRUIT_HUZZAH32 "Use the HUZZAH32 board battery profile" OFF)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_pm"
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
if(BLIP_ENABLE_FLEET)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/blip_fleet")
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
idf_build_set_property(BLIP_ENABLE_BLE ${BLIP_ENABLE_BLE})
idf_build_set_property(BLIP_ENABLE_CLASSIC_BT ${BLIP_ENABLE_CLASSIC_BT})
idf_build_set_property(BLIP_ENABLE_ESPNOW ${BLIP_ENABLE_ESPNOW})
idf_build_set_property(BLIP_ENABLE_FLEET ${BLIP_ENABLE_FLEET})
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
