# Asio adapters for the transport-agnostic client core (plan §24).
# Client core itself never links Asio; this target is the network glue used by
# the CLI and the integration tests.

add_library(uwb_client_net STATIC
    src/asio_transport.cpp
    src/client_session.cpp
)

target_include_directories(uwb_client_net PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(uwb_client_net PUBLIC cxx_std_20)
target_link_libraries(uwb_client_net PUBLIC uwb::client_core uwb_asio)

uwb_apply_strict_warnings(uwb_client_net)
uwb_apply_sanitizers(uwb_client_net)

add_library(uwb::client_net ALIAS uwb_client_net)
