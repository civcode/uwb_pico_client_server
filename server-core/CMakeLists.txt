add_library(uwb_server_core STATIC
    src/version.cpp  # Phase 0 wiring placeholder (removed when the simulator links real APIs)
    src/server_config.cpp
    src/connection_context.cpp
    src/server_errors.cpp
    src/tx_queue.cpp
    src/event_service.cpp
    src/did_service.cpp
    src/routine_service.cpp
    src/service_dispatcher.cpp
    src/server_core.cpp
)

target_include_directories(uwb_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_server_core PUBLIC cxx_std_20)
target_link_libraries(uwb_server_core PUBLIC uwb::protocol)

uwb_apply_strict_warnings(uwb_server_core)
uwb_apply_sanitizers(uwb_server_core)

add_library(uwb::server_core ALIAS uwb_server_core)
