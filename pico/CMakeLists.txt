# Pico firmware layer (implementation plan §7).
#
# This subdirectory is only configured with the Pico SDK toolchain preset:
#
#   cmake --preset pico-w
#   cmake --build --preset pico-w
#
# which requires PICO_SDK_PATH to be set and uses
# ${PICO_SDK_PATH}/pico_sdk_init.cmake as the toolchain file.

if(NOT DEFINED PICO_SDK_VERSION_STRING)
    message(FATAL_ERROR
        "UWB_BUILD_PICO_FIRMWARE is ON but the Pico SDK was not initialised. "
        "Configure with 'cmake --preset pico-w' (requires PICO_SDK_PATH).")
endif()

# Firmware targets are added in implementation plan §7 (Phase 5).
message(STATUS "uwb_system: Pico SDK ${PICO_SDK_VERSION_STRING} detected for board '${PICO_BOARD}'")
