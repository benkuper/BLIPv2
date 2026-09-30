idf_component_register(SRCS "src/ddp.cpp" "src/esp_ddp_component.cpp" INCLUDE_DIRS "include"
    REQUIRES blip_core blip_led esp_timer freertos lwip)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
