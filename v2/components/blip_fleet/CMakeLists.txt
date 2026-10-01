idf_component_register(SRCS "src/fleet.cpp" "src/esp_fleet_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_network blip_storage blip_transport esp_hw_support esp_timer esp_wifi freertos)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
