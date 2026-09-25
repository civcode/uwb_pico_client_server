# Project-wide warning policy (implementation plan §2.3).
#
# Note: -Wconversion / -Wsign-conversion are intentionally not enabled project-wide;
# they are enabled for the protocol library separately via uwb_apply_strict_warnings().

function(uwb_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
        return()
    endif()

    if(NOT UWB_STRICT_WARNINGS)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-qual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
    )
endfunction()

function(uwb_apply_strict_warnings target)
    uwb_apply_warnings(${target})
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wconversion -Wsign-conversion)
    endif()
endfunction()
