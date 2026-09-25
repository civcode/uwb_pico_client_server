add_executable(uwb_cli
    src/main.cpp
)

target_compile_features(uwb_cli PRIVATE cxx_std_20)
target_link_libraries(uwb_cli PRIVATE uwb::client_core uwb::protocol uwb::domain)

uwb_apply_warnings(uwb_cli)
uwb_apply_sanitizers(uwb_cli)
