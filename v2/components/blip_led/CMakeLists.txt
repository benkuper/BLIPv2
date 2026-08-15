idf_component_register(
    SRCS
        "src/esp_rmt_strip_component.cpp"
        "src/strip_config.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_resources blip_storage esp_driver_rmt esp_timer freertos)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})

if(DEFINED BLIP_LED_DEFAULT_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_LED_DEFAULT_GPIO=${BLIP_LED_DEFAULT_GPIO})
endif()
