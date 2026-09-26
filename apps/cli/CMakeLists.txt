add_executable(uwb_cli
    src/main.cpp
)

target_compile_features(uwb_cli PRIVATE cxx_std_20)
target_link_libraries(uwb_cli PRIVATE uwb::client_core uwb::client_net uwb::protocol uwb::domain)

if(TARGET Threads::Threads)
    target_link_libraries(uwb_cli PRIVATE Threads::Threads)
endif()

uwb_apply_warnings(uwb_cli)
uwb_apply_sanitizers(uwb_cli)
