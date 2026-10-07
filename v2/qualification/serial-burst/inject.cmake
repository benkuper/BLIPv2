# Explicit opt-in diagnostic hook. Production CMake/component sources are unchanged.
# IDF creates the app ELF after the underlying CMake project() invocation; defer
# attachment until its directory has finished configuring.
if(PROJECT_NAME STREQUAL "blip-v2" AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(BLIP_UART_TRACE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/uart_trace.cpp" CACHE INTERNAL "Qualification trace source")
    function(blip_attach_uart_trace)
        if(NOT TARGET blip-v2.elf)
            message(FATAL_ERROR "UART trace requires the production blip-v2 application")
        endif()
        target_sources(blip-v2.elf PRIVATE "${BLIP_UART_TRACE_SOURCE}")
        target_link_libraries(blip-v2.elf PRIVATE idf::blip_transport)
        target_link_options(blip-v2.elf PRIVATE "-Wl,--wrap=uart_write_bytes")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../cmake/blip_component.cmake")
        blip_configure_component(blip-v2.elf)
    endfunction()
    cmake_language(DEFER CALL blip_attach_uart_trace)
endif()
