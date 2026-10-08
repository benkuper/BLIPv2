idf_component_register(SRCS "main.cpp" "../../../tests/host/blip_script_controls_tests.cpp"
    PRIV_REQUIRES blip_wasm pthread esp_timer esp_system heap freertos INCLUDE_DIRS "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
idf_component_get_property(wasm_lib blip_wasm COMPONENT_LIB)
target_compile_options(${wasm_lib} PRIVATE -fstack-usage)
