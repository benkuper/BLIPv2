idf_component_register(
    SRCS
        "src/esp_oscquery_component.cpp"
        "src/legacy_osc.cpp"
        "src/osc.cpp"
        "src/oscquery.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_network blip_storage esp_http_server esp_netif freertos lwip)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
