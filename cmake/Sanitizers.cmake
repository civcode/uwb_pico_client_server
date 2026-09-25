# ASan/UBSan support for host builds (implementation plan §2.3, §84).

function(uwb_apply_sanitizers target)
    if(NOT UWB_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
    )
endfunction()
