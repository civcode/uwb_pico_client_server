#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>

#include "uwb/protocol/version.hpp"

#ifndef UWB_HIL_SERIAL_DEVICE
#define UWB_HIL_SERIAL_DEVICE ""
#endif

#ifndef UWB_HIL_PICO_HOST
#define UWB_HIL_PICO_HOST ""
#endif

TEST_CASE("HIL environment is configured", "[hil][env]") {
    // Placeholder HIL assertion; real HIL coverage arrives in implementation plan §3, §7, §13.
    static constexpr std::string_view serial_device = UWB_HIL_SERIAL_DEVICE;
    static constexpr std::string_view pico_host = UWB_HIL_PICO_HOST;

    INFO("Set -DHIL_SERIAL_DEVICE=/dev/ttyUSB0 and -DHIL_PICO_HOST=<board ip> for HIL runs.");
    CHECK(!serial_device.empty());
    CHECK(!pico_host.empty());
}
