idf_component_register(
    SRCS "src/battery_model.cpp" "src/esp_battery_component.cpp" "src/esp_sleep_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core esp_adc
    PRIV_REQUIRES esp_timer esp_hw_support esp_driver_uart
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
