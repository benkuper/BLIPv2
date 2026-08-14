idf_component_register(
    SRCS
        "src/esp_wifi_component.cpp"
        "src/provisioning_form.cpp"
        "src/wifi_config.cpp"
        "src/wifi_state_machine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_storage esp_event esp_http_server esp_netif esp_timer esp_wifi freertos)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
