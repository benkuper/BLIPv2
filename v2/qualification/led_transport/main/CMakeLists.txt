idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES esp_driver_rmt esp_driver_spi esp_timer freertos heap log soc
    INCLUDE_DIRS "")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)

foreach(name RMT_GPIO SPI_DATA_GPIO SPI_CLOCK_GPIO)
    if(DEFINED BLIP_QUAL_${name})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_QUAL_${name}=${BLIP_QUAL_${name}})
    endif()
endforeach()
