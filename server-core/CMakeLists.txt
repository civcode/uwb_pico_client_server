add_library(uwb_server_core STATIC
    src/version.cpp
)

target_include_directories(uwb_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_server_core PUBLIC cxx_std_20)
target_link_libraries(uwb_server_core PUBLIC uwb::protocol uwb::domain)

uwb_apply_warnings(uwb_server_core)
uwb_apply_sanitizers(uwb_server_core)

add_library(uwb::server_core ALIAS uwb_server_core)
