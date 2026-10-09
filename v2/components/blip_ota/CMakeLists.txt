idf_build_get_property(blip_release_tls BLIP_ENABLE_RELEASE_TLS)
set(blip_release_tls_requires)
if(blip_release_tls)
    list(APPEND blip_release_tls_requires mbedtls)
endif()
idf_component_register(
    SRCS
        "src/esp_ota_component.cpp"
        "src/update_service.cpp"
        "src/release_catalog.cpp"
        "src/release_image.cpp"
        "src/release_policy.cpp"
        "src/esp_release_http.cpp"
        "src/esp_release_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_update blip_core blip_network blip_storage esp_app_format esp_http_client esp_timer esp_netif log
    PRIV_REQUIRES ${blip_release_tls_requires})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
