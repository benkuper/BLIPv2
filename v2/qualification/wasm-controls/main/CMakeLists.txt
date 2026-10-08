# Exercise the exact portable decoder without linking an interpreter or starting
# production services. The host cases allocate fixtures only in this qualifier.
idf_component_register(SRCS "main.cpp"
    "../../../tests/host/blip_wasm_manifest_tests.cpp"
    "../../../components/blip_wasm/src/script_manifest.cpp"
    "../../../components/blip_wasm/src/utf8.cpp"
    INCLUDE_DIRS "../../../components/blip_wasm/include"
    PRIV_REQUIRES blip_core pthread esp_timer esp_system heap freertos)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
