cmake_minimum_required(VERSION 3.22)
project(blip_v2_host_tests LANGUAGES CXX)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_library(blip_core_host STATIC
    ../../components/blip_core/src/boot_health.cpp
    ../../components/blip_core/src/descriptor_json.cpp
    ../../components/blip_core/src/diagnostics.cpp
)
target_include_directories(blip_core_host PUBLIC ../../components/blip_core/include)
target_compile_features(blip_core_host PUBLIC cxx_std_20)

add_executable(blip_core_tests blip_core_tests.cpp)
target_link_libraries(blip_core_tests PRIVATE blip_core_host)

if(MSVC)
    target_compile_options(blip_core_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_core_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_core_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_core_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()

add_test(NAME blip_core_tests COMMAND blip_core_tests)

add_executable(blip_diagnostics_tests blip_diagnostics_tests.cpp)
target_link_libraries(blip_diagnostics_tests PRIVATE blip_core_host)
if(MSVC)
    target_compile_options(blip_diagnostics_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_diagnostics_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_diagnostics_tests COMMAND blip_diagnostics_tests)

add_library(blip_ota_host STATIC
    ../../components/blip_ota/src/update_service.cpp
)
target_include_directories(blip_ota_host PUBLIC ../../components/blip_ota/include)
target_link_libraries(blip_ota_host PUBLIC blip_core_host)
target_compile_features(blip_ota_host PUBLIC cxx_std_20)

add_executable(blip_ota_tests blip_ota_tests.cpp)
target_link_libraries(blip_ota_tests PRIVATE blip_ota_host)
if(MSVC)
    target_compile_options(blip_ota_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_ota_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_ota_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_ota_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_ota_tests COMMAND blip_ota_tests)

add_library(blip_transport_host STATIC
    ../../components/blip_transport/src/envelope.cpp
    ../../components/blip_transport/src/legacy_serial_v1.cpp
    ../../components/blip_transport/src/serial_protocol.cpp
)
target_include_directories(blip_transport_host PUBLIC ../../components/blip_transport/include)
target_link_libraries(blip_transport_host PUBLIC blip_core_host)
target_compile_features(blip_transport_host PUBLIC cxx_std_20)

add_executable(blip_transport_tests blip_transport_tests.cpp)
target_link_libraries(blip_transport_tests PRIVATE blip_transport_host)
if(MSVC)
    target_compile_options(blip_transport_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_transport_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_transport_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_transport_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_transport_tests COMMAND blip_transport_tests)
add_test(
    NAME blip_serial_control_self_test
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/control/blip_serial_control.py
        --self-test)

add_executable(blip_resources_tests blip_resources_tests.cpp)
target_include_directories(blip_resources_tests PRIVATE ../../components/blip_resources/include)
target_link_libraries(blip_resources_tests PRIVATE blip_core_host)
if(MSVC)
    target_compile_options(blip_resources_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_resources_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_resources_tests COMMAND blip_resources_tests)

add_executable(blip_scheduler_tests blip_scheduler_tests.cpp)
target_link_libraries(blip_scheduler_tests PRIVATE blip_core_host)
if(MSVC)
    target_compile_options(blip_scheduler_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_scheduler_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_scheduler_tests COMMAND blip_scheduler_tests)

add_library(blip_storage_host STATIC
    ../../components/blip_storage/src/atomic_file_store.cpp
    ../../components/blip_storage/src/imported_settings.cpp
    ../../components/blip_storage/src/legacy_import_coordinator.cpp
    ../../components/blip_storage/src/legacy_settings_importer.cpp
    ../../components/blip_storage/src/migration_registry.cpp
    ../../components/blip_storage/src/settings_store.cpp
    ../../components/blip_storage/src/web_asset_store.cpp
)
target_include_directories(blip_storage_host PUBLIC ../../components/blip_storage/include)
target_link_libraries(blip_storage_host PUBLIC blip_core_host)
target_compile_features(blip_storage_host PUBLIC cxx_std_20)

add_executable(blip_storage_tests blip_storage_tests.cpp)
target_link_libraries(blip_storage_tests PRIVATE blip_storage_host)
if(MSVC)
    target_compile_options(blip_storage_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_storage_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_storage_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_storage_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_storage_tests COMMAND blip_storage_tests)

add_executable(blip_file_storage_tests blip_file_storage_tests.cpp)
target_link_libraries(blip_file_storage_tests PRIVATE blip_storage_host)
if(MSVC)
    target_compile_options(blip_file_storage_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_file_storage_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_file_storage_tests COMMAND blip_file_storage_tests)

add_executable(blip_web_asset_tests blip_web_asset_tests.cpp)
target_link_libraries(blip_web_asset_tests PRIVATE blip_storage_host)
target_compile_definitions(blip_web_asset_tests PRIVATE
    BLIP_FACTORY_WEB_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../components/blip_storage/factory_web.bundle")
if(MSVC)
    target_compile_options(blip_web_asset_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_web_asset_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_web_asset_tests COMMAND blip_web_asset_tests)

add_executable(blip_migration_tests blip_migration_tests.cpp)
target_link_libraries(blip_migration_tests PRIVATE blip_storage_host)
target_compile_definitions(blip_migration_tests PRIVATE
    BLIP_V1_SETTINGS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/v1/settings/representative.msgpack")
if(MSVC)
    target_compile_options(blip_migration_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(blip_migration_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_migration_tests COMMAND blip_migration_tests)

add_executable(blip_legacy_import_dump blip_legacy_import_dump.cpp)
target_link_libraries(blip_legacy_import_dump PRIVATE blip_storage_host)
if(MSVC)
    target_compile_options(blip_legacy_import_dump PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(blip_legacy_import_dump PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(
    NAME blip_legacy_settings_fixture
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/validate_legacy_settings_import.py
        --tool $<TARGET_FILE:blip_legacy_import_dump>
        --source ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/v1/settings/representative.msgpack
        --expected ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/v1/settings/expected-import.json)

add_library(blip_network_host STATIC
    ../../components/blip_network/src/provisioning_form.cpp
    ../../components/blip_network/src/wifi_config.cpp
    ../../components/blip_network/src/wifi_state_machine.cpp
)
target_include_directories(blip_network_host PUBLIC ../../components/blip_network/include)
target_link_libraries(blip_network_host PUBLIC blip_storage_host)
target_compile_features(blip_network_host PUBLIC cxx_std_20)

add_executable(blip_network_tests blip_network_tests.cpp)
target_link_libraries(blip_network_tests PRIVATE blip_network_host)
if(MSVC)
    target_compile_options(blip_network_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_network_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_network_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_network_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_network_tests COMMAND blip_network_tests)

add_library(blip_oscquery_host STATIC
    ../../components/blip_oscquery/src/legacy_osc.cpp
    ../../components/blip_oscquery/src/osc.cpp
    ../../components/blip_oscquery/src/oscquery.cpp
)
target_include_directories(blip_oscquery_host PUBLIC ../../components/blip_oscquery/include)
target_link_libraries(blip_oscquery_host PUBLIC blip_core_host)
target_compile_features(blip_oscquery_host PUBLIC cxx_std_20)

add_executable(blip_oscquery_tests blip_oscquery_tests.cpp)
target_link_libraries(blip_oscquery_tests PRIVATE blip_oscquery_host)
target_compile_definitions(blip_oscquery_tests PRIVATE
    BLIP_V1_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/v1")
if(MSVC)
    target_compile_options(blip_oscquery_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_oscquery_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_oscquery_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_oscquery_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_oscquery_tests COMMAND blip_oscquery_tests)

add_library(blip_led_host STATIC
    ../../components/blip_led/src/backend_selector.cpp
    ../../components/blip_led/src/color.cpp
    ../../components/blip_led/src/compositor.cpp
    ../../components/blip_led/src/current_limiter.cpp
    ../../components/blip_led/src/engine.cpp
    ../../components/blip_led/src/parallel_encoder.cpp
    ../../components/blip_led/src/playback.cpp
    ../../components/blip_led/src/protocol_encoder.cpp
    ../../components/blip_led/src/strip_config.cpp
    ../../components/blip_led/src/stream.cpp
)
target_include_directories(blip_led_host PUBLIC ../../components/blip_led/include)
target_link_libraries(blip_led_host PUBLIC blip_core_host)
target_compile_features(blip_led_host PUBLIC cxx_std_20)

add_executable(blip_led_tests blip_led_tests.cpp)
target_link_libraries(blip_led_tests PRIVATE blip_led_host)
if(MSVC)
    target_compile_options(blip_led_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_led_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_led_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_led_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_led_tests COMMAND blip_led_tests)

add_library(blip_led_compat_host STATIC
    ../../components/blip_artnet/src/artnet.cpp
    ../../components/blip_e131/src/e131.cpp
    ../../components/blip_ddp/src/ddp.cpp
)
target_include_directories(blip_led_compat_host PUBLIC
    ../../components/blip_artnet/include
    ../../components/blip_e131/include
    ../../components/blip_ddp/include
)
target_link_libraries(blip_led_compat_host PUBLIC blip_core_host blip_led_host)
target_compile_features(blip_led_compat_host PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(blip_led_compat_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_led_compat_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()

add_executable(blip_led_m4_tests blip_led_m4_tests.cpp)
target_link_libraries(blip_led_m4_tests PRIVATE blip_led_compat_host)
if(MSVC)
    target_compile_options(blip_led_m4_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_led_m4_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_led_m4_tests COMMAND blip_led_m4_tests)

add_executable(blip_led_power_tests blip_led_power_tests.cpp)
target_link_libraries(blip_led_power_tests PRIVATE blip_led_host)
if(MSVC)
    target_compile_options(blip_led_power_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_led_power_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_led_power_tests COMMAND blip_led_power_tests)

add_library(blip_power_host STATIC
    ../../components/blip_power/src/battery_model.cpp
)
target_include_directories(blip_power_host PUBLIC ../../components/blip_power/include)
target_compile_features(blip_power_host PUBLIC cxx_std_20)
add_executable(blip_power_tests blip_power_tests.cpp)
target_link_libraries(blip_power_tests PRIVATE blip_power_host)
if(MSVC)
    target_compile_options(blip_power_host PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
    target_compile_options(blip_power_tests PRIVATE /W4 /WX /GR- /D_HAS_EXCEPTIONS=0)
else()
    target_compile_options(blip_power_host PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_options(blip_power_tests PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
endif()
add_test(NAME blip_power_tests COMMAND blip_power_tests)
