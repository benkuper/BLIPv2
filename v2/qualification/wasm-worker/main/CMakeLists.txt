idf_component_register(SRCS "main.cpp" "faults.cpp" PRIV_REQUIRES blip_wasm pthread esp_timer esp_system heap freertos INCLUDE_DIRS "")
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
