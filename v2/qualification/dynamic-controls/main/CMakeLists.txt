# Compile the production portable dispatcher/OSC sources and the shared host
# cases. This surrogate owner does not load a guest or exercise concurrency.
idf_component_register(SRCS "main.cpp"
    "../../../tests/host/blip_dynamic_controls_tests.cpp"
    "../../../components/blip_oscquery/src/oscquery.cpp"
    "../../../components/blip_oscquery/src/legacy_osc.cpp"
    "../../../components/blip_oscquery/src/osc.cpp"
    INCLUDE_DIRS "../../../components/blip_oscquery/include"
    PRIV_REQUIRES blip_core pthread esp_timer esp_system heap freertos)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
