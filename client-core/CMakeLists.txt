add_library(uwb_client_core STATIC
    src/version.cpp
    src/client_clock.cpp
    src/client_error.cpp
    src/client_types.cpp
    src/transaction.cpp
    src/client_connection.cpp
    src/discovery_client.cpp
    src/device_registry.cpp
    src/connection_manager.cpp
    src/notification_queue.cpp
    src/application_controller.cpp
)

target_include_directories(uwb_client_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_client_core PUBLIC cxx_std_20)
target_link_libraries(uwb_client_core PUBLIC uwb::protocol uwb::domain)

uwb_apply_strict_warnings(uwb_client_core)
uwb_apply_sanitizers(uwb_client_core)

add_library(uwb::client_core ALIAS uwb_client_core)
