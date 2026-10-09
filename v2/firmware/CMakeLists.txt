cmake_minimum_required(VERSION 3.22)

# Keep generated configuration isolated inside each selected build directory.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(BLIP_RELEASE_SEQUENCE "1" CACHE STRING "Monotonic firmware release code")
set(BLIP_RELEASE_PROFILE "minimal" CACHE STRING "Firmware release profile identity")
option(BLIP_ENABLE_RELEASE_TLS "Include optional HTTPS release transport" OFF)
option(BLIP_FLASH_WEB_UI "Preload the web interface into the filesystem during factory flashing" ON)
string(LENGTH "${BLIP_RELEASE_SEQUENCE}" blip_release_digits)
string(LENGTH "${BLIP_RELEASE_PROFILE}" blip_release_profile_length)
if(NOT BLIP_RELEASE_SEQUENCE MATCHES "^[1-9][0-9]*$" OR blip_release_digits GREATER 10)
    message(FATAL_ERROR "BLIP_RELEASE_SEQUENCE must be 1..4294967295")
endif()
if(BLIP_RELEASE_SEQUENCE GREATER 4294967295)
    message(FATAL_ERROR "BLIP_RELEASE_SEQUENCE must be 1..4294967295")
endif()
if(NOT BLIP_RELEASE_PROFILE MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$" OR blip_release_profile_length GREATER 31)
    message(FATAL_ERROR "BLIP_RELEASE_PROFILE must be a 1..31 character identifier")
endif()
option(BLIP_ENABLE_BLE "Include the NimBLE control transport" OFF)
option(BLIP_ENABLE_CLASSIC_BT "Include Classic Bluetooth SPP control on ESP32" OFF)
option(BLIP_ENABLE_ESPNOW "Include ESP-NOW V2 peer control" OFF)
option(BLIP_ENABLE_FLEET "Include routerless ESP-NOW fleet synchronization and scheduled cues" OFF)
option(BLIP_ENABLE_WASM "Include the supervised WASM script worker" OFF)
option(BLIP_QUALIFY_WASM_PROVIDERS "Run owner lifecycle qualification before enabling external control" OFF)
if(BLIP_QUALIFY_WASM_PROVIDERS AND (NOT BLIP_ENABLE_WASM OR NOT BLIP_ENABLE_FLEET))
    message(FATAL_ERROR "Provider lifecycle qualification requires WASM and fleet")
endif()
set(BLIP_WASM_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../build/wasm-deps" CACHE PATH "Pinned WASM runtime checkouts")
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
if(BLIP_ENABLE_WASM)
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.wasm.defaults")
    if(BLIP_ENABLE_BLE AND IDF_TARGET STREQUAL "esp32c6")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.wasm.ble.c6.defaults")
    endif()
    if(IDF_TARGET STREQUAL "esp32")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.wasm.esp32.defaults")
    endif()
endif()
if(IDF_TARGET STREQUAL "esp32s3")
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.esp32s3.defaults")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
if(BLIP_ENABLE_RELEASE_TLS)
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.updates-tls.defaults")
else()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.updates.defaults")
endif()
if(BLIP_BOARD_CREATORS_BALL_V2 OR BLIP_BOARD_CREATORS_TAB OR BLIP_BOARD_CREATORS_CLUB)
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.external-storage.defaults")
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
if(BLIP_ENABLE_WASM)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/blip_wasm")
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
idf_build_set_property(BLIP_ENABLE_WASM ${BLIP_ENABLE_WASM})
idf_build_set_property(BLIP_ENABLE_RELEASE_TLS ${BLIP_ENABLE_RELEASE_TLS})
# Current external-media boards all use SD SPI. Additional board families can
# opt into their provider without charging every SD board for unused drivers.
option(BLIP_ENABLE_EXTERNAL_MMC "Include the optional SD/MMC storage provider" OFF)
option(BLIP_ENABLE_EXTERNAL_NOR "Include the optional dedicated SPI NOR provider" OFF)
idf_build_set_property(BLIP_ENABLE_EXTERNAL_MMC ${BLIP_ENABLE_EXTERNAL_MMC})
idf_build_set_property(BLIP_ENABLE_EXTERNAL_NOR ${BLIP_ENABLE_EXTERNAL_NOR})
if(BLIP_BOARD_CREATORS_BALL_V2 OR BLIP_BOARD_CREATORS_TAB OR BLIP_BOARD_CREATORS_CLUB)
    idf_build_set_property(BLIP_HAS_EXTERNAL_STORAGE ON)
else()
    idf_build_set_property(BLIP_HAS_EXTERNAL_STORAGE OFF)
endif()
idf_build_set_property(BLIP_QUALIFY_WASM_PROVIDERS ${BLIP_QUALIFY_WASM_PROVIDERS})
idf_build_set_property(BLIP_WASM_DEPS ${BLIP_WASM_DEPS})
idf_build_set_property(BLIP_BOARD_ADAFRUIT_HUZZAH32 ${BLIP_BOARD_ADAFRUIT_HUZZAH32})

# Keep the bootstrap image limited to main and its transitive requirements.
idf_build_set_property(MINIMAL_BUILD ON)

# Conditional dependencies must never make targets rewrite one shared lock.
idf_build_set_property(
    DEPENDENCIES_LOCK
    "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")

project(blip-v2 VERSION 0.1.0)
if(BLIP_ENABLE_RELEASE_TLS AND NOT CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS)
    message(FATAL_ERROR "HTTPS build requires CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y in the existing sdkconfig")
elseif(NOT BLIP_ENABLE_RELEASE_TLS AND CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS)
    message(FATAL_ERROR "Plain HTTP build requires CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=n in the existing sdkconfig")
endif()

if(CONFIG_ESPTOOLPY_FLASHSIZE_8MB)
    if(NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions-8mb.csv")
        message(FATAL_ERROR "Release layout ota-8mb-v1 requires partitions-8mb.csv")
    endif()
elseif(CONFIG_ESPTOOLPY_FLASHSIZE_4MB)
    if(NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions.csv")
        message(FATAL_ERROR "Release layout ota-4mb-v1 requires partitions.csv")
    endif()
else()
    message(FATAL_ERROR "Declare a BLIP release layout for the selected flash size")
endif()

# Defaults do not replace an existing sdkconfig. Refuse a stale generic C6
# configuration before producing a Ball image with the wrong factory layout.
if(BLIP_BOARD_CREATORS_BALL_V2 AND
   (NOT CONFIG_ESPTOOLPY_FLASHSIZE_8MB OR
    NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions-8mb.csv"))
    message(FATAL_ERROR "Creators Ball V2 requires 8 MB flash and partitions-8mb.csv. Use sdkconfig.ball-8mb.defaults in a fresh build directory or update the existing sdkconfig.")
endif()

# Keep the full UI out of both application slots. Factory flashing writes one
# copy into LittleFS; app-only updates preserve it. An empty filesystem uses
# the small firmware-resident first-run downloader instead.
set(blip_factory_tree "${CMAKE_BINARY_DIR}/factory_storage")
set(blip_factory_web "${blip_factory_tree}/web/assets.bundle")
file(MAKE_DIRECTORY "${blip_factory_tree}")
if(BLIP_FLASH_WEB_UI)
    set(blip_web_root "${CMAKE_CURRENT_LIST_DIR}/../web")
    set(blip_web_packer "${CMAKE_CURRENT_LIST_DIR}/../tools/web/pack_web_assets.py")
    file(GLOB_RECURSE blip_web_sources CONFIGURE_DEPENDS "${blip_web_root}/src/*.js")
    add_custom_command(OUTPUT "${blip_factory_web}"
        COMMAND "${PYTHON}" "${blip_web_packer}" --source "${blip_web_root}" --output "${blip_factory_web}"
        DEPENDS ${blip_web_sources} "${blip_web_root}/index.html" "${blip_web_root}/styles.css"
            "${blip_web_root}/package.json" "${blip_web_packer}"
        VERBATIM)
    add_custom_target(blip_factory_web DEPENDS "${blip_factory_web}")
    littlefs_create_partition_image(storage "${blip_factory_tree}" FLASH_IN_PROJECT DEPENDS blip_factory_web)
else()
    # Remove only our generated UI artifact when reusing a build directory.
    file(REMOVE "${blip_factory_web}")
    littlefs_create_partition_image(storage "${blip_factory_tree}" FLASH_IN_PROJECT)
endif()
