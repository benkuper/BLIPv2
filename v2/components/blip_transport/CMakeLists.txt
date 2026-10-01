idf_build_get_property(blip_ble_enabled BLIP_ENABLE_BLE)
idf_build_get_property(blip_classic_enabled BLIP_ENABLE_CLASSIC_BT)
idf_build_get_property(blip_espnow_enabled BLIP_ENABLE_ESPNOW)
set(blip_transport_sources
    "src/envelope.cpp"
    "src/esp_serial_transport_component.cpp"
    "src/legacy_serial_v1.cpp"
    "src/serial_protocol.cpp")
if(blip_ble_enabled)
    list(APPEND blip_transport_sources
        "src/ble_fragments.cpp"
        "src/esp_ble_transport_component.cpp")
endif()
if(blip_classic_enabled)
    list(APPEND blip_transport_sources "src/esp_classic_transport_component.cpp")
endif()
if(blip_espnow_enabled)
    list(APPEND blip_transport_sources
        "src/espnow_frames.cpp"
        "src/esp_espnow_transport_component.cpp")
endif()

set(blip_transport_requires blip_core esp_driver_uart esp_driver_usb_serial_jtag freertos)
if(blip_ble_enabled)
    list(APPEND blip_transport_requires blip_storage bt log)
endif()
if(blip_classic_enabled)
    list(APPEND blip_transport_requires blip_storage bt log)
endif()
if(blip_espnow_enabled)
    list(APPEND blip_transport_requires blip_storage blip_network esp_wifi esp_timer log)
endif()

idf_component_register(
    SRCS ${blip_transport_sources}
    INCLUDE_DIRS "include"
    REQUIRES ${blip_transport_requires})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
