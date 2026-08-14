include_guard(GLOBAL)

# Apply the language and warning contract to BLIP-owned component sources only.
# ESP-IDF and third-party components retain their upstream compiler settings.
function(blip_configure_component target)
    target_compile_options(
        ${target}
        PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
                $<$<COMPILE_LANGUAGE:C>:-Wextra>
                $<$<COMPILE_LANGUAGE:C>:-Werror>
                $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
                $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
                $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
                $<$<COMPILE_LANGUAGE:CXX>:-Wall>
                $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
endfunction()
