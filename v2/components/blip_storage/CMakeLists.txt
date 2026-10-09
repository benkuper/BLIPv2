set(blip_external_dependencies "")
idf_build_get_property(blip_has_external_storage BLIP_HAS_EXTERNAL_STORAGE)
idf_build_get_property(blip_external_mmc BLIP_ENABLE_EXTERNAL_MMC)
idf_build_get_property(blip_external_nor BLIP_ENABLE_EXTERNAL_NOR)
if(blip_has_external_storage)
    set(blip_external_dependencies fatfs esp_driver_sdspi esp_driver_sdmmc esp_driver_spi esp_driver_gpio esp_timer)
endif()
idf_component_register(
    SRCS
        "src/atomic_file_store.cpp"
        "src/automatic_file_store.cpp"
        "src/automatic_web_backend.cpp"
        "src/device_identity_component.cpp"
        "src/esp_nvs_blob_store.cpp"
        "src/esp_external_storage.cpp"
        "src/sd_spi_card.cpp"
        "src/imported_settings.cpp"
        "src/littlefs_storage_component.cpp"
        "src/migration_registry.cpp"
        "src/nvs_settings_component.cpp"
        "src/posix_file_backend.cpp"
        "src/sd_file_storage_component.cpp"
        "src/settings_store.cpp"
        "src/web_asset_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core blip_resources esp_partition spi_flash joltwallet__littlefs nvs_flash vfs
        ${blip_external_dependencies})
if(blip_external_dependencies)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_HAS_EXTERNAL_STORAGE=1)
endif()
if(blip_external_mmc)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_EXTERNAL_MMC=1)
endif()
if(blip_external_nor)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BLIP_EXTERNAL_NOR=1)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
