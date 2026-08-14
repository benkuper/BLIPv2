cmake_minimum_required(VERSION 3.22)
project(blip_v2_host_tests LANGUAGES CXX)

enable_testing()

add_library(blip_core_host STATIC
    ../../components/blip_core/src/descriptor_json.cpp
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
    ../../components/blip_storage/src/settings_store.cpp
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
