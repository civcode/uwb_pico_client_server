add_library(uwb_domain STATIC
    src/version.cpp
)

target_include_directories(uwb_domain PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_domain PUBLIC cxx_std_20)
uwb_apply_strict_warnings(uwb_domain)
uwb_apply_sanitizers(uwb_domain)

add_library(uwb::domain ALIAS uwb_domain)
