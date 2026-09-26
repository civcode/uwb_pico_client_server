#include "uwb/simulator/simulator_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace uwb::simulator {
namespace {

bool parseU32(std::string_view text, std::uint32_t &out) {
    if (text.empty()) {
        return false;
    }
    std::string copy(text);
    char *end = nullptr;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 0);
    if (end == copy.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parseU16(std::string_view text, std::uint16_t &out) {
    std::uint32_t wide = 0;
    if (!parseU32(text, wide) || wide > 0xFFFFU) {
        return false;
    }
    out = static_cast<std::uint16_t>(wide);
    return true;
}

// Accepts "1,2,3,4,5,6,7,8" or a 16 digit hex string.
bool parseBoardId(std::string_view text, std::array<std::uint8_t, 8> &out) {
    if (text.size() == 16) {
        std::array<std::uint8_t, 8> values{};
        for (std::size_t i = 0; i < 8; ++i) {
            std::uint32_t value = 0;
            if (!parseU32("0x" + std::string(text.substr(i * 2, 2)), value) || value > 0xFFU) {
                return false;
            }
            values[i] = static_cast<std::uint8_t>(value);
        }
        out = values;
        return true;
    }

    std::array<std::uint8_t, 8> values{};
    std::size_t index = 0;
    std::size_t pos = 0;
    while (index < values.size()) {
        const std::size_t comma = text.find(',', pos);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        std::uint32_t value = 0;
        if (!parseU32(text.substr(pos, end - pos), value) || value > 0xFFU) {
            return false;
        }
        values[index++] = static_cast<std::uint8_t>(value);
        if (end == text.size()) {
            break;
        }
        pos = end + 1;
    }
    if (index != values.size()) {
        return false;
    }
    out = values;
    return true;
}

bool parseMask(std::string_view text, CapabilityMask &out) {
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    auto comma = text.find(',');
    if (comma == std::string_view::npos) {
        std::string copy(text);
        char *end = nullptr;
        const unsigned long long value = std::strtoull(copy.c_str(), &end, 0);
        if (end == copy.c_str() || *end != '\0') {
            return false;
        }
        out = static_cast<CapabilityMask>(value);
        return true;
    }
    if (!parseU32(text.substr(0, comma), high) || !parseU32(text.substr(comma + 1), low)) {
        return false;
    }
    out = (static_cast<CapabilityMask>(high) << 32U) | static_cast<CapabilityMask>(low);
    return true;
}

std::string_view valueAfter(std::string_view arg, const char *const *argv, int index, int argc) {
    // "--name=value" or "--name value"
    const std::size_t eq = arg.find('=');
    if (eq != std::string_view::npos) {
        return arg.substr(eq + 1);
    }
    if (index + 1 < argc) {
        return argv[index + 1];
    }
    return {};
}

bool takesValue(std::string_view name) {
    static constexpr std::string_view kValueOptions[] = {
        "--board-id",      "--device-name",        "--logical-address",     "--udp-port",
        "--tcp-port",      "--bind",               "--known-mask",
        "--detected-mask", "--override-on",        "--override-off",
        "--period-ms",     "--tags",               "--anchors",
        "--range-mm",      "--noise-mm",           "--seed",
        "--backend-latency-ms", "--backend-gap-ms", "--security-key",
        "--p2-server-ms",  "--p2-star-10ms"};
    for (const std::string_view option : kValueOptions) {
        if (option == name) {
            return true;
        }
    }
    return false;
}

} // namespace

bool parseSimulatorOptions(int argc, const char *const *argv, SimulatorOptions &out, std::string &error,
                           bool &usageRequested) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const std::size_t eq = arg.find('=');
        const std::string_view name = eq == std::string_view::npos ? arg : arg.substr(0, eq);
        std::string_view value = valueAfter(arg, argv, i, argc);
        const bool hasInlineValue = eq != std::string_view::npos;

        auto needValue = [&](const char *flag) {
            if (value.empty()) {
                error = std::string(flag) + " needs a value";
                return false;
            }
            return true;
        };

        if (name == "-h" || name == "--help") {
            usageRequested = true;
            continue;
        } else if (name == "--board-id") {
            if (!needValue("--board-id") || !parseBoardId(value, out.boardUniqueId)) {
                if (error.empty()) {
                    error = "--board-id expects 8 bytes as '1,2,3,4,5,6,7,8' or 16 hex digits";
                }
                return false;
            }
        } else if (name == "--device-name") {
            if (!needValue("--device-name")) {
                return false;
            }
            out.deviceName = std::string(value);
        } else if (name == "--logical-address") {
            if (!needValue("--logical-address") || !parseU16(value, out.logicalAddress)) {
                if (error.empty()) {
                    error = "--logical-address expects 0x0000..0xFFFF";
                }
                return false;
            }
        } else if (name == "--udp-port") {
            if (!needValue("--udp-port") || !parseU16(value, out.udpPort)) {
                if (error.empty()) {
                    error = "--udp-port expects 0..65535";
                }
                return false;
            }
        } else if (name == "--tcp-port") {
            if (!needValue("--tcp-port") || !parseU16(value, out.tcpPort)) {
                if (error.empty()) {
                    error = "--tcp-port expects 0..65535";
                }
                return false;
            }
        } else if (name == "--bind") {
            if (!needValue("--bind")) {
                return false;
            }
            out.bindAddress = std::string(value);
        } else if (name == "--known-mask") {
            if (!needValue("--known-mask") || !parseMask(value, out.knownMask)) {
                if (error.empty()) {
                    error = "--known-mask expects a 64-bit number (0x..., 0xhigh,0xlow)";
                }
                return false;
            }
        } else if (name == "--detected-mask") {
            if (!needValue("--detected-mask") || !parseMask(value, out.detectedMask)) {
                if (error.empty()) {
                    error = "--detected-mask expects a 64-bit number (0x..., 0xhigh,0xlow)";
                }
                return false;
            }
        } else if (name == "--override-on") {
            if (!needValue("--override-on") || !parseMask(value, out.overrideForceOn)) {
                if (error.empty()) {
                    error = "--override-on expects a 64-bit number";
                }
                return false;
            }
        } else if (name == "--override-off") {
            if (!needValue("--override-off") || !parseMask(value, out.overrideForceOff)) {
                if (error.empty()) {
                    error = "--override-off expects a 64-bit number";
                }
                return false;
            }
        } else if (name == "--period-ms") {
            if (!needValue("--period-ms") || !parseU32(value, out.measurementPeriodMs) ||
                out.measurementPeriodMs == 0U) {
                if (error.empty()) {
                    error = "--period-ms expects a positive number";
                }
                return false;
            }
        } else if (name == "--tags") {
            if (!needValue("--tags") || !parseU32(value, out.tagCount)) {
                if (error.empty()) {
                    error = "--tags expects a number";
                }
                return false;
            }
        } else if (name == "--anchors") {
            if (!needValue("--anchors") || !parseU32(value, out.anchorCount)) {
                if (error.empty()) {
                    error = "--anchors expects a number";
                }
                return false;
            }
        } else if (name == "--no-pdoa") {
            out.emitPdoa = false;
        } else if (name == "--local-position") {
            out.emitLocalPosition = true;
        } else if (name == "--range-mm") {
            if (!needValue("--range-mm") || !parseU32(value, out.rangeBaseMm)) {
                if (error.empty()) {
                    error = "--range-mm expects a number";
                }
                return false;
            }
        } else if (name == "--noise-mm") {
            if (!needValue("--noise-mm") || !parseU32(value, out.rangeNoiseMm)) {
                if (error.empty()) {
                    error = "--noise-mm expects a number";
                }
                return false;
            }
        } else if (name == "--seed") {
            std::uint32_t seed = 0;
            if (!needValue("--seed") || !parseU32(value, seed)) {
                if (error.empty()) {
                    error = "--seed expects a number";
                }
                return false;
            }
            out.randomSeed = seed;
        } else if (name == "--backend-latency-ms") {
            if (!needValue("--backend-latency-ms") || !parseU32(value, out.backendLatencyMs)) {
                if (error.empty()) {
                    error = "--backend-latency-ms expects a number";
                }
                return false;
            }
        } else if (name == "--backend-gap-ms") {
            if (!needValue("--backend-gap-ms") || !parseU32(value, out.backendInterCommandGapMs)) {
                if (error.empty()) {
                    error = "--backend-gap-ms expects a number";
                }
                return false;
            }
        } else if (name == "--security") {
            out.securityEnabled = true;
        } else if (name == "--security-key") {
            if (!needValue("--security-key") || !parseU32(value, out.securityKey)) {
                if (error.empty()) {
                    error = "--security-key expects a 32-bit number";
                }
                return false;
            }
            out.securityEnabled = true;
        } else if (name == "--no-raw-at") {
            out.rawAtEnabled = false;
        } else if (name == "--p2-server-ms") {
            if (!needValue("--p2-server-ms") || !parseU16(value, out.p2ServerMaxMs)) {
                if (error.empty()) {
                    error = "--p2-server-ms expects 0..65535";
                }
                return false;
            }
        } else if (name == "--p2-star-10ms") {
            if (!needValue("--p2-star-10ms") || !parseU16(value, out.p2StarServerMax10ms)) {
                if (error.empty()) {
                    error = "--p2-star-10ms expects 0..65535 (unit: 10 ms)";
                }
                return false;
            }
        } else if (name == "--fault-timeout") {
            out.backendFaultTimeout = true;
        } else if (name == "--fault-parse-error") {
            out.backendFaultParseError = true;
        } else if (name == "--fault-unsupported") {
            out.backendFaultUnsupported = true;
        } else if (name == "--backend-full") {
            out.backendRejectsSubmissions = true;
        } else if (name == "--deterministic") {
            out.deterministicClock = true;
            out.rangeNoiseMm = 0; // no measurement jitter in deterministic mode
        } else if (name == "--verbose" || name == "-v") {
            out.verbose = true;
        } else {
            error = "unknown option: " + std::string(name);
            return false;
        }

        if (takesValue(name) && !hasInlineValue) {
            ++i; // value given as a separate argument
        }
    }

    if (out.deviceName.size() > uwb::protocol::kMaxDeviceNameLength) {
        error = "device name must be at most 32 bytes";
        return false;
    }
    if (!uwb::protocol::capabilityOverrideValid(out.overrideForceOn, out.overrideForceOff)) {
        error = "capability override cannot force a bit on and off at the same time";
        return false;
    }
    return true;
}

void printSimulatorUsage() {
    std::printf(R"(uwb_simulator - host simulator for the UWB Pico system

Usage: uwb_simulator [options]

Options:
  --board-id <1,2,3,4,5,6,7,8|16hex>  Board unique id used to derive the device UUID
  --device-name <name>                Device name (max 32 bytes)
  --logical-address <hex>             Device logical address (default 0x1001)
  --udp-port <port>                   Discovery port (default 13401, 0 = ephemeral)
  --tcp-port <port>                   Protocol port (default 13401, 0 = ephemeral)
  --bind <address>                    Bind address (default 0.0.0.0)
  --known-mask <mask>                 Known capability mask (64 bit)
  --detected-mask <mask>              Detected capability mask (64 bit)
  --override-on <mask>                Capability override force-on mask
  --override-off <mask>               Capability override force-off mask
  --period-ms <n>                     Measurement period in milliseconds (default 100)
  --tags <n>                          Simulated tag count (default 2)
  --anchors <n>                       Simulated anchor count (default 3)
  --no-pdoa                           Suppress PDOD azimuth/elevation in measurements
  --local-position                    Emit 0x0102 local position events as well
  --range-mm <n>                      Base simulated range in millimetres (default 1200)
  --noise-mm <n>                     Range noise amplitude in millimetres (default 120)
  --seed <n>                          Deterministic RNG seed
  --backend-latency-ms <n>            Simulated UWB command latency (default 5)
  --backend-gap-ms <n>                Simulated inter-command gap (default 0)
  --security                          Enable Security Access (service 0x27)
  --security-key <hex>                Security key accepted by the simulator
  --no-raw-at                         Disable raw AT service (0x40)
  --p2-server-ms <n>                  Reported p2ServerMaxMs
  --p2-star-10ms <n>                  Reported p2*ServerMax (unit 10 ms)
  --deterministic                     Stepped logical clock and no measurement jitter
  --fault-timeout                     Next UWB command never answers (NRC 0x72 at p2*)
  --fault-parse-error                 Next UWB command answers malformed (NRC 0x72)
  --fault-unsupported                 Next UWB command reports "not supported"
  --backend-full                      UWB command queue is full (NRC 0x21)
  -v, --verbose                       Log protocol activity
  -h, --help                          Show this help
)");
}

void printSimulatorOptions(const SimulatorOptions &options) {
    std::printf("simulator identity   : board=%02x%02x%02x%02x%02x%02x%02x%02x name=%s address=0x%04X\n",
                options.boardUniqueId[0], options.boardUniqueId[1], options.boardUniqueId[2], options.boardUniqueId[3],
                options.boardUniqueId[4], options.boardUniqueId[5], options.boardUniqueId[6], options.boardUniqueId[7],
                options.deviceName.c_str(), options.logicalAddress);
    std::printf("simulator ports      : udp=%u tcp=%u bind=%s\n", options.udpPort, options.tcpPort,
                options.bindAddress.c_str());
    std::printf("simulator capabilities: known=0x%016llX detected=0x%016llX effective=0x%016llX\n",
                static_cast<unsigned long long>(options.knownMask), static_cast<unsigned long long>(options.detectedMask),
                static_cast<unsigned long long>(options.effectiveMask()));
    std::printf("simulator measurements: period=%u ms tags=%u anchors=%u pdoa=%s localPosition=%s\n",
                options.measurementPeriodMs, options.tagCount, options.anchorCount, options.emitPdoa ? "yes" : "no",
                options.emitLocalPosition ? "yes" : "no");
    std::printf("simulator backend    : latency=%u ms gap=%u ms queue=%u security=%s rawAt=%s\n",
                options.backendLatencyMs, options.backendInterCommandGapMs, options.backendCommandQueue,
                options.securityEnabled ? "yes" : "no", options.rawAtEnabled ? "yes" : "no");
    std::printf("simulator determinism: seed=%llu steppedClock=%s noise=%u mm\n",
                static_cast<unsigned long long>(options.randomSeed), options.deterministicClock ? "yes" : "no",
                options.rangeNoiseMm);
    std::printf("simulator faults     : timeout=%s parseError=%s unsupported=%s queueFull=%s\n",
                options.backendFaultTimeout ? "armed" : "off", options.backendFaultParseError ? "armed" : "off",
                options.backendFaultUnsupported ? "armed" : "off", options.backendRejectsSubmissions ? "armed" : "off");
}

} // namespace uwb::simulator
