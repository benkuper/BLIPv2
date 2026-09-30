idf_component_register(
    SRCS "src/battery_model.cpp" "src/esp_battery_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core esp_adc
    PRIV_REQUIRES esp_timer
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
