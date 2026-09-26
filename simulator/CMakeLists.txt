# Host simulator (implementation plan §20-§23).
#
# uwb_simulator_core is a library so the integration tests drive exactly the same
# runtime the CLI executable uses. Only asio_server.cpp touches sockets; the
# runtime, clock, storage, security and fake backend are transport-free and mirror
# the wiring the Pico firmware will use.

add_library(uwb_simulator_core STATIC
    src/simulator_config.cpp
    src/simulated_backend.cpp
    src/simulator_runtime.cpp
    src/asio_server.cpp
)

target_include_directories(uwb_simulator_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(uwb_simulator_core PUBLIC cxx_std_20)
target_link_libraries(uwb_simulator_core
    PUBLIC
        uwb::protocol
        uwb::server_core
        uwb_asio
)

uwb_apply_strict_warnings(uwb_simulator_core)
uwb_apply_sanitizers(uwb_simulator_core)

add_library(uwb::simulator_core ALIAS uwb_simulator_core)

add_executable(uwb_simulator src/main.cpp)
target_compile_features(uwb_simulator PRIVATE cxx_std_20)
target_link_libraries(uwb_simulator PRIVATE uwb_simulator_core)

uwb_apply_warnings(uwb_simulator)
uwb_apply_sanitizers(uwb_simulator)
