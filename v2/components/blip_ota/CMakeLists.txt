idf_component_register(
    SRCS
        "src/esp_ota_component.cpp"
        "src/update_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_update blip_core esp_app_format log)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
