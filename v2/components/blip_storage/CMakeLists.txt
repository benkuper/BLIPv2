idf_component_register(
    SRCS
        "src/esp_nvs_blob_store.cpp"
        "src/nvs_settings_component.cpp"
        "src/settings_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core nvs_flash)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
