idf_component_register(
    SRCS "src/esp_power_manager_component.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core esp_pm
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
