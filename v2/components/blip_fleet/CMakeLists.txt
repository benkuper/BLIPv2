idf_build_get_property(blip_fleet_wasm BLIP_ENABLE_WASM)
set(blip_fleet_optional_requires "")
if(blip_fleet_wasm)
    list(APPEND blip_fleet_optional_requires blip_wasm)
endif()
idf_component_register(SRCS "src/fleet.cpp" "src/esp_fleet_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_network blip_storage blip_transport esp_hw_support esp_timer esp_wifi freertos ${blip_fleet_optional_requires})
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
if(blip_fleet_wasm)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC BLIP_FLEET_WASM=1)
endif()
