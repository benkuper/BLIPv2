idf_build_get_property(blip_ble_enabled BLIP_ENABLE_BLE)
set(blip_transport_sources
    "src/envelope.cpp"
    "src/esp_serial_transport_component.cpp"
    "src/legacy_serial_v1.cpp"
    "src/serial_protocol.cpp")
if(blip_ble_enabled)
    list(APPEND blip_transport_sources "src/ble_fragments.cpp")
endif()

idf_component_register(
    SRCS ${blip_transport_sources}
    INCLUDE_DIRS "include"
    REQUIRES blip_core esp_driver_uart esp_driver_usb_serial_jtag freertos)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
