idf_component_register(
    SRCS
        "src/esp_oscquery_component.cpp"
        "src/legacy_osc.cpp"
        "src/osc.cpp"
        "src/oscquery.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_network blip_ota blip_resources blip_storage esp_http_server esp_netif freertos lwip)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
idf_build_get_property(blip_python PYTHON)
set(blip_first_run "${CMAKE_CURRENT_BINARY_DIR}/first_run.html.gz")
set(blip_first_run_source "${CMAKE_CURRENT_LIST_DIR}/../../firmware/web-bootstrap/index.html")
set(blip_first_run_packer "${CMAKE_CURRENT_LIST_DIR}/../../tools/web/pack_first_run.py")
add_custom_command(OUTPUT "${blip_first_run}"
    COMMAND "${blip_python}" "${blip_first_run_packer}" --source "${blip_first_run_source}" --output "${blip_first_run}"
    DEPENDS "${blip_first_run_source}" "${blip_first_run_packer}"
        "${CMAKE_CURRENT_LIST_DIR}/../../tools/web/pack_web_assets.py"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} "${blip_first_run}" BINARY RENAME_TO "first_run_html")
