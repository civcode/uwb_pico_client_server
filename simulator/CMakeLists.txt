add_executable(uwb_simulator
    src/main.cpp
)

target_compile_features(uwb_simulator PRIVATE cxx_std_20)
target_link_libraries(uwb_simulator PRIVATE uwb::protocol uwb::domain uwb::server_core)

uwb_apply_warnings(uwb_simulator)
uwb_apply_sanitizers(uwb_simulator)
