idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES blip_core blip_network blip_oscquery blip_ota blip_resources blip_storage blip_transport esp_system esp_timer log
    INCLUDE_DIRS "")

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})

if(BLIP_DIAGNOSTICS_HIL_FORCE_BOOT_LOOP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_DIAGNOSTICS_HIL_FORCE_BOOT_LOOP=1)
endif()

if(BLIP_DIAGNOSTICS_HIL_CLEAR_SAFE_MODE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_DIAGNOSTICS_HIL_CLEAR_SAFE_MODE=1)
endif()
