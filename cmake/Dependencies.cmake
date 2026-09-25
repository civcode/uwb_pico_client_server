# Third-party dependency wiring.
#
# Policy:
#   - Catch2: use the system package when available, otherwise fetch a pinned version.
#   - Asio:   standalone Asio (design_decisions.md). System package preferred.
#   - libcrc: vendored pinned snapshot in external/libcrc (design_decisions.md).
#             Vendored because libcrc has no CMake build and generates its CRC-32
#             lookup table with its own 'prc' tool; the generated table is checked in
#             so both host and ARM cross builds are hermetic.

include(FetchContent)

# ---------------------------------------------------------------------------
# Catch2 (host tests only)
# ---------------------------------------------------------------------------
if(UWB_BUILD_TESTS)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
        message(STATUS "uwb_system: using system Catch2 ${Catch2_VERSION}")
        list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
    else()
        message(STATUS "uwb_system: fetching Catch2 v3.16.0")
        set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.16.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
endif()

# ---------------------------------------------------------------------------
# Standalone Asio
# ---------------------------------------------------------------------------
find_package(Asio CONFIG QUIET)
add_library(uwb_asio INTERFACE)
if(Asio_FOUND)
    message(STATUS "uwb_system: using system standalone Asio (CMake Asio::asio package)")
    target_link_libraries(uwb_asio INTERFACE Asio::asio)
else()
    find_path(UWB_ASIO_INCLUDE_DIR asio.hpp REQUIRED)
    target_include_directories(uwb_asio INTERFACE "${UWB_ASIO_INCLUDE_DIR}")
    target_compile_definitions(uwb_asio INTERFACE ASIO_STANDALONE)
    message(STATUS "uwb_system: using standalone Asio from ${UWB_ASIO_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------
# libcrc (vendored, pinned)
# ---------------------------------------------------------------------------
add_library(uwb_libcrc STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/external/libcrc/src/crc32.c"
)
target_include_directories(uwb_libcrc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/libcrc/include")
set_target_properties(uwb_libcrc PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
add_library(uwb::libcrc ALIAS uwb_libcrc)

# ---------------------------------------------------------------------------
# Threading (client I/O thread)
# ---------------------------------------------------------------------------
find_package(Threads QUIET)
