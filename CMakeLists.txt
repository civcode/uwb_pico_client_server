cmake_minimum_required(VERSION 3.21)

project(uwb_system
    VERSION 0.1.0
    DESCRIPTION "UWB Pico W client/server system"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(UWB_BUILD_TESTS "Build host unit/integration tests" ON)
option(UWB_BUILD_SIMULATOR "Build the host Pico/UWB simulator" ON)
option(UWB_BUILD_CLI "Build the uwbctl CLI" ON)
option(UWB_ENABLE_HIL_TESTS "Configure hardware-in-the-loop tests (requires real hardware)" OFF)
option(UWB_ENABLE_SANITIZERS "Enable AddressSanitizer/UndefinedBehaviorSanitizer for host code" OFF)
option(UWB_STRICT_WARNINGS "Treat project warnings as errors" ON)
option(UWB_BUILD_PICO_FIRMWARE "Build the Raspberry Pi Pico W firmware (requires Pico SDK toolchain)" OFF)

set(HIL_SERIAL_DEVICE "" CACHE STRING "Serial device path used by HIL tests, e.g. /dev/ttyUSB0")
set(HIL_PICO_HOST "" CACHE STRING "Host/IP of a Pico device used by HIL tests")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)
include(Dependencies)

if(UWB_BUILD_TESTS)
    include(CTest)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(shared)
add_subdirectory(server-core)

if(UWB_BUILD_SIMULATOR)
    add_subdirectory(simulator)
endif()

add_subdirectory(client-core)
add_subdirectory(client-core/net)

if(UWB_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(UWB_BUILD_PICO_FIRMWARE)
    add_subdirectory(pico)
endif()

if(UWB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

message(STATUS "uwb_system configuration summary:")
message(STATUS "  UWB_BUILD_TESTS          = ${UWB_BUILD_TESTS}")
message(STATUS "  UWB_BUILD_SIMULATOR      = ${UWB_BUILD_SIMULATOR}")
message(STATUS "  UWB_BUILD_CLI            = ${UWB_BUILD_CLI}")
message(STATUS "  UWB_BUILD_PICO_FIRMWARE  = ${UWB_BUILD_PICO_FIRMWARE}")
message(STATUS "  UWB_ENABLE_HIL_TESTS     = ${UWB_ENABLE_HIL_TESTS}")
message(STATUS "  UWB_ENABLE_SANITIZERS    = ${UWB_ENABLE_SANITIZERS}")
