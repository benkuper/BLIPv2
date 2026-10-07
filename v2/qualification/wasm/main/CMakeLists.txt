idf_component_register(SRCS "main.cpp" PRIV_REQUIRES bench_runtime esp_timer esp_system heap freertos INCLUDE_DIRS "")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
