#include <asio.hpp>

#include <array>
#include <csignal>
#include <cstdio>
#include <string>

#include "uwb/protocol/uuid.hpp"
#include "uwb/server/version.hpp"
#include "uwb/simulator/asio_server.hpp"
#include "uwb/simulator/simulator_config.hpp"
#include "uwb/simulator/simulator_runtime.hpp"

namespace {

std::string formatUuid(const uwb::protocol::Uuid &uuid) {
    char buffer[37];
    uuid.formatLowerHex(buffer, sizeof buffer);
    return std::string(buffer);
}

} // namespace

int main(int argc, char **argv) {
    uwb::simulator::SimulatorOptions options;
    std::string error;
    bool usageRequested = false;

    if (!uwb::simulator::parseSimulatorOptions(argc, argv, options, error, usageRequested)) {
        std::fprintf(stderr, "uwb_simulator: %s\n\n", error.c_str());
        uwb::simulator::printSimulatorUsage();
        return 2;
    }
    if (usageRequested) {
        uwb::simulator::printSimulatorUsage();
        return 0;
    }

    asio::io_context io;
    uwb::simulator::SimulatorRuntime runtime(options);
    uwb::simulator::SimulatorServer server(io, runtime, options);

    if (!server.start(error)) {
        std::fprintf(stderr, "uwb_simulator: %s\n", error.c_str());
        return 1;
    }

    uwb::simulator::printSimulatorOptions(options);
    std::printf("device uuid        : %s\n", formatUuid(runtime.core().dids().deviceUuid()).c_str());
    const std::string coreVersion(uwb::server::serverCoreVersion());
    std::printf("server-core        : %s\n", coreVersion.c_str());
    std::printf("listening          : udp/%u discovery, tcp/%u protocol (Ctrl-C to stop)\n", server.udpPort(),
                server.tcpPort());
    std::fflush(stdout);

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&server](std::error_code, int) { server.requestStop(); });

    server.run();
    return 0;
}
