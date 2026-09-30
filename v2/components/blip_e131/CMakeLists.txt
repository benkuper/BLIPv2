idf_component_register(SRCS "src/e131.cpp" "src/esp_e131_component.cpp"
    INCLUDE_DIRS "include" REQUIRES blip_core blip_led blip_network esp_timer freertos lwip)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
