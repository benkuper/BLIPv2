idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES blip_core blip_resources esp_system esp_timer log
    INCLUDE_DIRS "")

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
