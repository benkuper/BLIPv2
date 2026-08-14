idf_component_register(
    SRCS
        "src/atomic_file_store.cpp"
        "src/esp_nvs_blob_store.cpp"
        "src/esp_nvs_legacy_source.cpp"
        "src/imported_settings.cpp"
        "src/legacy_import_coordinator.cpp"
        "src/legacy_settings_import_component.cpp"
        "src/legacy_settings_importer.cpp"
        "src/littlefs_storage_component.cpp"
        "src/migration_registry.cpp"
        "src/nvs_settings_component.cpp"
        "src/posix_file_backend.cpp"
        "src/sd_file_storage_component.cpp"
        "src/settings_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES blip_core esp_partition joltwallet__littlefs nvs_flash vfs)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/blip_component.cmake)
blip_configure_component(${COMPONENT_LIB})
