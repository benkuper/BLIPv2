idf_component_register(
    SRCS
        "src/boot_health.cpp"
        "src/descriptor_json.cpp"
        "src/diagnostics.cpp"
        "src/esp_diagnostics_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition esp_system esp_timer espcoredump freertos heap log)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
