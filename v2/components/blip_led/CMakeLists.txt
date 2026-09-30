idf_component_register(
    SRCS
        "src/backend_selector.cpp"
        "src/color.cpp"
        "src/compositor.cpp"
        "src/engine.cpp"
        "src/esp_rmt_strip_component.cpp"
        "src/esp_rmt_output_driver.cpp"
        "src/esp_spi_dma_output_driver.cpp"
        "src/parallel_encoder.cpp"
        "src/playback.cpp"
        "src/protocol_encoder.cpp"
        "src/strip_config.cpp"
        "src/stream.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_resources blip_storage esp_driver_gpio esp_driver_rmt esp_driver_spi esp_timer freertos)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})

if(DEFINED BLIP_LED_DEFAULT_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_LED_DEFAULT_GPIO=${BLIP_LED_DEFAULT_GPIO})
elseif(BLIP_BOARD_CREATORS_BALL_V2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_LED_DEFAULT_GPIO=3)
elseif(BLIP_BOARD_CREATORS_TAB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_LED_DEFAULT_GPIO=25)
elseif(BLIP_BOARD_M5STICKC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_LED_DEFAULT_GPIO=26)
endif()
if(BLIP_BOARD_CREATORS_BALL_V2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_BOARD_CREATORS_BALL_V2=1)
endif()
if(BLIP_BOARD_CREATORS_TAB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_BOARD_CREATORS_TAB=1)
endif()
if(BLIP_BOARD_M5STICKC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_BOARD_M5STICKC=1)
endif()
