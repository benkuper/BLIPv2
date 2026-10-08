idf_component_register(SRCS "src/artnet.cpp" "src/esp_artnet_component.cpp"
    INCLUDE_DIRS "include" REQUIRES blip_core blip_led blip_network esp_hw_support esp_timer freertos lwip)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
