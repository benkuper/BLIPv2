idf_component_register(
    SRCS
        "src/esp_wifi_component.cpp"
        "src/provisioning_form.cpp"
        "src/wifi_config.cpp"
        "src/wifi_state_machine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_storage esp_driver_gpio esp_event esp_http_server esp_netif esp_timer esp_wifi freertos)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})

if(BLIP_BOARD_SEEED_XIAO_ESP32C6)
    if(NOT IDF_TARGET STREQUAL "esp32c6")
        message(FATAL_ERROR "BLIP_BOARD_SEEED_XIAO_ESP32C6 requires IDF_TARGET=esp32c6")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BLIP_WIFI_RF_SWITCH_POWER_GPIO=3
        BLIP_WIFI_RF_SWITCH_SELECT_GPIO=14
        BLIP_WIFI_RF_SWITCH_POWER_ACTIVE_LEVEL=0
        BLIP_WIFI_RF_SWITCH_EXTERNAL_LEVEL=1)
endif()
