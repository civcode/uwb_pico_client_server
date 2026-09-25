add_library(uwb_protocol STATIC
    src/version.cpp
)

target_include_directories(uwb_protocol PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_protocol PUBLIC cxx_std_20)
uwb_apply_strict_warnings(uwb_protocol)
uwb_apply_sanitizers(uwb_protocol)

add_library(uwb::protocol ALIAS uwb_protocol)
