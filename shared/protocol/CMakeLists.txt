add_library(uwb_protocol STATIC
    src/version.cpp
    src/protocol_version.cpp
    src/generic_header.cpp
    src/frame_parser.cpp
    src/payloads.cpp
    src/services.cpp
    src/events.cpp
    src/dids.cpp
    src/capabilities.cpp
    src/routines.cpp
    src/tlv.cpp
    src/did_records.cpp
    src/complete_config.cpp
    src/uuid.cpp
)

target_include_directories(uwb_protocol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(uwb_protocol PUBLIC cxx_std_20)

uwb_apply_strict_warnings(uwb_protocol)

add_library(uwb::protocol ALIAS uwb_protocol)
