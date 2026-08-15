cmake_minimum_required(VERSION 3.22)

# Keep generated configuration isolated inside each selected build directory.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_resources"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_storage"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_transport"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_network"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_led"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_ota"
    "${CMAKE_CURRENT_LIST_DIR}/../components/blip_oscquery")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

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
