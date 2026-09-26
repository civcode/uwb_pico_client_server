add_library(uwb_domain STATIC
    src/version.cpp
    src/device_identity.cpp
    src/measurement.cpp
    src/uwb_configuration.cpp
)

target_include_directories(uwb_domain PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_domain PUBLIC cxx_std_20)
target_link_libraries(uwb_domain PUBLIC uwb::protocol)

uwb_apply_strict_warnings(uwb_domain)
uwb_apply_sanitizers(uwb_domain)

add_library(uwb::domain ALIAS uwb_domain)
