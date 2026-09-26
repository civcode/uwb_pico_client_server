// uwbctl - development CLI for the UWB client core (implementation plan §27.8).
//
// Threading model: ClientSession owns the single Asio I/O thread that runs every
// connection (specification §55).  The CLI never calls ApplicationController from
// this thread - reads go through ClientSession::invoke(), commands through
// ClientSession::post() - and it consumes measurements through the bounded
// notification queue, so slow console output cannot stall the network (§56, §27.7).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "uwb/client/version.hpp"
#include "uwb/client_net/client_session.hpp"

#include "uwb/domain/device_identity.hpp"
#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/complete_config.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/did_records.hpp"
#include "uwb/protocol/dids.hpp"
#include "uwb/protocol/events.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/routines.hpp"
#include "uwb/protocol/services.hpp"

namespace client = uwb::client;
namespace client_net = uwb::client_net;
namespace domain = uwb::domain;
namespace protocol = uwb::protocol;

using namespace std::chrono_literals;

namespace {

std::atomic<bool> interrupted{false};

void handleSignal(int) { interrupted.store(true); }

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

const std::vector<std::string> kBooleanFlags = {"broadcast", "follow", "verbose", "help"};

struct CommandLine {
    std::vector<std::string> words;              // positional words
    std::map<std::string, std::string> options;  // --name value
    std::vector<std::string> targets;            // --target may repeat
    std::string sub;                             // subcommand of a group command

    [[nodiscard]] bool has(std::string_view name) const { return options.count(std::string(name)) > 0; }
    [[nodiscard]] std::string value(std::string_view name, std::string fallback = {}) const {
        auto it = options.find(std::string(name));
        return it == options.end() ? fallback : it->second;
    }
    [[nodiscard]] std::uint64_t number(std::string_view name, std::uint64_t fallback) const {
        auto it = options.find(std::string(name));
        if (it == options.end()) {
            return fallback;
        }
        return parseUnsigned(it->second).value_or(fallback);
    }
    [[nodiscard]] std::chrono::milliseconds duration(std::string_view name, std::chrono::milliseconds fallback) const {
        auto it = options.find(std::string(name));
        if (it == options.end()) {
            return fallback;
        }
        return std::chrono::milliseconds(parseUnsigned(it->second).value_or(static_cast<std::uint64_t>(fallback.count())));
    }
    [[nodiscard]] static std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
        if (text.empty()) {
            return std::nullopt;
        }
        const bool hexadecimal = text.rfind("0x", 0) == 0;
        if (hexadecimal) {
            text.remove_prefix(2);
        }
        if (text.empty()) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (char c : text) {
            const unsigned char digit = static_cast<unsigned char>(c);
            std::uint64_t nibble = 0;
            if (digit >= '0' && digit <= '9') {
                nibble = static_cast<std::uint64_t>(digit - '0');
            } else if (hexadecimal && digit >= 'a' && digit <= 'f') {
                nibble = static_cast<std::uint64_t>(digit - 'a') + 10U;
            } else if (hexadecimal && digit >= 'A' && digit <= 'F') {
                nibble = static_cast<std::uint64_t>(digit - 'A') + 10U;
            } else {
                return std::nullopt;
            }
            value = value * (hexadecimal ? 16U : 10U) + nibble;
        }
        return value;
    }
};

[[nodiscard]] CommandLine parseArguments(int argc, char **argv) {
    CommandLine parsed;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument.rfind("--", 0) != 0) {
            parsed.words.push_back(argument);
            continue;
        }
        const std::string name = argument.substr(2);
        if (std::find(kBooleanFlags.begin(), kBooleanFlags.end(), name) != kBooleanFlags.end()) {
            parsed.options[name] = "1";
            continue;
        }
        std::string value;
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            value = argv[++i];
        }
        if (name == "target") {
            parsed.targets.push_back(value);
        }
        parsed.options[name] = value;
    }
    return parsed;
}

[[nodiscard]] std::optional<client::Endpoint> parseTarget(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string host = std::string(text.substr(0, colon));
    const auto port = CommandLine::parseUnsigned(text.substr(colon + 1));
    if (host.empty() || !port.has_value() || *port > 65535U) {
        return std::nullopt;
    }
    return client::Endpoint{host, static_cast<std::uint16_t>(*port)};
}

[[nodiscard]] std::string hex(const protocol::ConstBytes &bytes) {
    std::string out;
    for (std::uint8_t byte : bytes) {
        char buffer[3];
        std::snprintf(buffer, sizeof(buffer), "%02x", static_cast<unsigned>(byte));
        out += buffer;
    }
    return out;
}

[[nodiscard]] protocol::ByteBuffer parseHexBytes(std::string_view text) {
    protocol::ByteBuffer data;
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        text.remove_prefix(2); // accept both 0xNNNN and NNNN
    }
    if (text.empty() || text.size() % 2 != 0) {
        return {};
    }
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        auto byte = CommandLine::parseUnsigned(std::string("0x") + std::string(text.substr(i, 2)));
        if (!byte.has_value()) {
            return {};
        }
        data.push_back(static_cast<std::uint8_t>(*byte));
    }
    return data;
}

// ---------------------------------------------------------------------------
// Completion latch: the callback fires on the I/O thread, the CLI waits here.
// ---------------------------------------------------------------------------

template <typename T>
class Latch {
public:
    void complete(client::ClientResult<T> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
    }
    [[nodiscard]] bool ready() {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_.has_value();
    }
    [[nodiscard]] client::ClientResult<T> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(*result_);
    }

private:
    std::mutex mutex_;
    std::optional<client::ClientResult<T>> result_;
};

// ---------------------------------------------------------------------------
// CLI session: ClientSession + the CLI-side waiting helpers
// ---------------------------------------------------------------------------

class Cli {
public:
    explicit Cli(client::ControllerConfig config) : session(std::move(config)) { session.start(); }

    client_net::ClientSession session;
    domain::DeviceUuid found_;
    protocol::ByteBuffer key; // --key: Security Access key for privileged commands
    std::chrono::milliseconds connectTimeout = 3000ms;
    std::chrono::milliseconds commandTimeout = 8000ms;

    template <typename Predicate>
    [[nodiscard]] bool waitFor(Predicate predicate, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline && !interrupted.load()) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return predicate();
    }

    [[nodiscard]] bool waitLatch(Latch<std::string> &latch, std::chrono::milliseconds timeout) {
        return waitFor([&] { return latch.ready(); }, timeout);
    }

    [[nodiscard]] bool waitReady(const domain::DeviceUuid &uuid, std::chrono::milliseconds timeout) {
        return waitFor([&] {
            auto summary = session.deviceSummary(uuid);
            return summary.has_value() && summary->state == client::ConnectionState::Ready;
        },
                       timeout);
    }

    // Resolves a device by name, UUID, or host:port and connects it.
    [[nodiscard]] client::ClientResult<domain::DeviceUuid> openDevice(const std::string &argument) {
        if (auto endpoint = parseTarget(argument); endpoint.has_value()) {
            auto started = session.connectEndpoint(*endpoint, argument);
            if (!started.ok()) {
                return client::ClientResult<domain::DeviceUuid>::error(started.error());
            }
            const client::Endpoint target = *endpoint;
            if (!waitFor([&] {
                    for (const client::DeviceSummary &summary : session.listDevices()) {
                        if (summary.endpoint == target && summary.state == client::ConnectionState::Ready) {
                            found_ = summary.uuid;
                            return true;
                        }
                    }
                    return false;
                },
                connectTimeout)) {
                return client::ClientResult<domain::DeviceUuid>::error(client::ErrorDomain::Network,
                                                                      client::ClientErrorCode::ConnectTimeout,
                                                                      "no device answered on " + argument);
            }
            return client::ClientResult<domain::DeviceUuid>::ok(found_);
        }

        auto uuid = session.resolveDevice(argument);
        if (!uuid.has_value()) {
            // The name may belong to a device that is not in the registry yet.
            session.discoverNow();
            static_cast<void>(waitFor([&] { return session.resolveDevice(argument).has_value(); }, connectTimeout));
            uuid = session.resolveDevice(argument);
        }
        if (!uuid.has_value()) {
            return client::ClientResult<domain::DeviceUuid>::error(client::ErrorDomain::Application,
                                                                  client::ClientErrorCode::DeviceNotFound,
                                                                  "no device named " + argument);
        }

        auto started = session.connectDevice(*uuid);
        if (!started.ok()) {
            return client::ClientResult<domain::DeviceUuid>::error(started.error());
        }
        if (!waitReady(*uuid, connectTimeout)) {
            auto summary = session.deviceSummary(*uuid);
            const std::string detail = summary.has_value() ? client::connectionStateName(summary->state) : "unknown";
            return client::ClientResult<domain::DeviceUuid>::error(client::ErrorDomain::Network,
                                                                  client::ClientErrorCode::ConnectTimeout,
                                                                  "device did not reach Ready (" + detail + ")");
        }
        return client::ClientResult<domain::DeviceUuid>::ok(*uuid);
    }

    // Waits for a command callback and prints the outcome.  0 means success.
    int finish(Latch<std::string> &latch, bool verbose, std::chrono::milliseconds timeout) {
        if (!waitLatch(latch, timeout)) {
            std::cerr << "command timed out\n";
            return 1;
        }
        auto result = latch.take();
        if (!result.ok()) {
            std::cerr << "failed: " << result.error().toString() << "\n";
            return 1;
        }
        std::cout << (verbose ? "ok: " + result.value() : result.value()) << "\n";
        return 0;
    }

    // Reads one DID and hands back the record bytes.
    [[nodiscard]] client::ClientResult<protocol::ByteBuffer> readRecord(const domain::DeviceUuid &uuid,
                                                                        protocol::Did did,
                                                                        std::chrono::milliseconds timeout) {
        Latch<client::ServiceResponse> latch;
        session.post([this, uuid, did, &latch] {
            session.controller().readDid(uuid, did,
                                         [&latch](client::ClientResult<client::ServiceResponse> result) {
                                             latch.complete(std::move(result));
                                         });
        });
        if (!waitFor([&] { return latch.ready(); }, timeout)) {
            return client::ClientResult<protocol::ByteBuffer>::error(client::ErrorDomain::Network,
                                                                    client::ClientErrorCode::Timeout, "read timed out");
        }
        auto response = latch.take();
        if (!response.ok()) {
            return client::ClientResult<protocol::ByteBuffer>::error(response.error());
        }
        auto decoded = protocol::decodeReadDidResponse(response.value().pduBytes());
        if (!decoded.ok()) {
            return client::ClientResult<protocol::ByteBuffer>::error(client::ErrorDomain::Protocol,
                                                                    client::ClientErrorCode::DecodeError,
                                                                    "malformed ReadDid response");
        }
        return client::ClientResult<protocol::ByteBuffer>::ok(std::move(decoded.value().data));
    }

    [[nodiscard]] client::ClientResult<protocol::ByteBuffer> readConfigurationRecord(const domain::DeviceUuid &uuid) {
        return readRecord(uuid, protocol::Did::UwbCompleteConfiguration, commandTimeout);
    }

    // Extended Session is a precondition of every write (§22.3).
    // Security Access (§25) before a privileged command, when --key was given.
    [[nodiscard]] bool authenticate(const domain::DeviceUuid &uuid) {
        if (key.empty()) {
            return true;
        }
        Latch<std::string> latch;
        session.post([this, uuid, &latch] {
            session.controller().securityUnlock(uuid, key, [&latch](client::ClientResult<std::string> result) {
                latch.complete(std::move(result));
            });
        });
        if (!waitLatch(latch, commandTimeout)) {
            std::cerr << "security access timed out\n";
            return false;
        }
        auto result = latch.take();
        if (!result.ok()) {
            std::cerr << "security access failed: " << result.error().toString() << "\n";
            return false;
        }
        return true;
    }

    // Privileged commands run in the Extended Session (§22.3), which a secured
    // device only grants after Security Access succeeded.
    [[nodiscard]] bool enterExtendedSession(const domain::DeviceUuid &uuid) {
        if (!authenticate(uuid)) {
            return false;
        }
        Latch<std::string> latch;
        session.post([this, uuid, &latch] {
            session.controller().requestSession(uuid, protocol::SessionId::Extended,
                                                [&latch](client::ClientResult<std::string> result) {
                                                    latch.complete(std::move(result));
                                                });
        });
        if (!waitLatch(latch, commandTimeout)) {
            std::cerr << "session request timed out\n";
            return false;
        }
        auto result = latch.take();
        if (!result.ok()) {
            std::cerr << "session request failed: " << result.error().toString() << "\n";
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void printSummary(const client::DeviceSummary &summary) {
    std::cout << "  uuid           " << domain::uuidToString(summary.uuid) << "\n";
    std::cout << "  name           " << summary.name << "\n";
    std::cout << "  endpoint       " << summary.endpoint.toString() << "\n";
    std::cout << "  logical addr   0x" << std::hex << summary.logicalAddress << std::dec << "\n";
    std::cout << "  state          " << client::connectionStateName(summary.state) << "\n";
    std::cout << "  role           " << client::roleName(summary.role) << "\n";
    std::cout << "  session        " << client::sessionName(summary.session) << "\n";
    std::cout << "  capabilities   " << client::capabilityList(summary.capabilities) << "\n";
    std::cout << "  control        " << (summary.controlOccupied ? "occupied" : "free") << " (observers "
              << static_cast<unsigned>(summary.activeObservers) << "/" << static_cast<unsigned>(summary.maxObservers)
              << ")\n";
    std::cout << "  subscriptions  " << summary.subscriptions << "\n";
    if (summary.hasError) {
        std::cout << "  last error     " << summary.lastError << "\n";
    }
}

void printDeviceTable(const std::vector<client::DeviceSummary> &devices) {
    if (devices.empty()) {
        std::cout << "no devices discovered\n";
        return;
    }
    std::cout << std::left << "UUID" << std::setw(38) << " NAME" << std::setw(14) << "ENDPOINT" << std::setw(20)
              << "ADDR" << std::setw(9) << "STATE" << std::setw(14) << "ROLE" << "\n";
    for (const client::DeviceSummary &summary : devices) {
        std::ostringstream address;
        address << "0x" << std::hex << summary.logicalAddress << std::dec;
        std::cout << std::setw(36) << domain::uuidToString(summary.uuid) << " " << std::setw(13) << summary.name << " "
                  << std::setw(19) << summary.endpoint.toString() << " " << std::setw(8) << address.str() << " "
                  << std::setw(13) << client::connectionStateName(summary.state) << " " << client::roleName(summary.role)
                  << "\n";
    }
}

void printCompleteConfig(const protocol::CompleteUwbConfig &config) {
    std::cout << "schema " << config.schemaVersion << ", " << config.entries.size() << " entries\n";
    for (const protocol::DidRecordEntry &entry : config.entries) {
        const char *name = protocol::didName(entry.didId());
        std::cout << "  " << (name != nullptr ? name : "?") << " (0x" << std::hex << entry.did << std::dec << ") "
                  << hex(entry.dataBytes()) << "\n";
    }
}

void printDidRecord(protocol::Did did, const protocol::ConstBytes &data) {
    const char *name = protocol::didName(did);
    std::cout << (name != nullptr ? name : "did") << " (" << data.size() << " bytes)\n";

    auto text = protocol::decodeTextRecord8(data, protocol::kMaxDeviceNameLength);
    if (text.ok() && !text.value().empty()) {
        bool printable = true;
        for (std::uint8_t byte : text.value()) {
            if (byte < 0x20 || byte > 0x7E) {
                printable = false;
                break;
            }
        }
        if (printable) {
            std::cout << "  text  " << std::string(text.value().begin(), text.value().end()) << "\n";
        }
    }
    if (auto wide = protocol::decodeU64Record(data); wide.ok()) {
        std::cout << "  u64   0x" << std::hex << wide.value() << std::dec << "\n";
    }
    if (auto narrow = protocol::decodeU16Record(data); narrow.ok()) {
        std::cout << "  u16   0x" << std::hex << narrow.value() << std::dec << "\n";
    }
    std::cout << "  raw   " << hex(data) << "\n";
}

void printNotification(const client::ClientNotification &note) {
    const std::string device =
        note.device.has_value() ? note.device->name : (note.measurement.has_value() ? "device" : "client");
    switch (note.kind) {
    case client::NotificationKind::Measurement: {
        if (!note.measurement.has_value()) {
            return;
        }
        const auto &sample = *note.measurement;
        std::cout << "[" << device << "] anchor " << sample.anchorId << " tag " << sample.tagId;
        if (sample.rawRangeMm.has_value()) {
            std::cout << " range " << *sample.rawRangeMm << " mm";
        }
        if (sample.correctedRangeMm.has_value()) {
            std::cout << " corrected " << *sample.correctedRangeMm << " mm";
        }
        if (sample.rawAzimuthMilliDeg.has_value()) {
            std::cout << " az " << *sample.rawAzimuthMilliDeg << " mdeg";
        }
        if (sample.quality.has_value()) {
            std::cout << " quality " << *sample.quality;
        }
        std::cout << "\n";
        return;
    }
    case client::NotificationKind::LocalPosition: {
        std::cout << "[" << device << "] local position update\n";
        return;
    }
    case client::NotificationKind::DeviceSeen: {
        std::cout << "[" << device << "] discovered on "
                  << (note.device.has_value() ? note.device->endpoint.toString() : "?") << "\n";
        return;
    }
    case client::NotificationKind::DeviceRemoved: {
        std::cout << "[" << device << "] removed from the registry\n";
        return;
    }
    case client::NotificationKind::DeviceStateChanged: {
        std::cout << "[" << device << "] state "
                  << (note.device.has_value() ? client::connectionStateName(note.device->state) : "?") << " role "
                  << (note.device.has_value() ? client::roleName(note.device->role) : "?") << "\n";
        return;
    }
    case client::NotificationKind::DeviceError: {
        std::cout << "[" << device << "] error "
                  << (note.error.has_value() ? note.error->toString() : "unknown") << "\n";
        return;
    }
    case client::NotificationKind::StreamStateChanged: {
        if (!note.stream.has_value()) {
            return;
        }
        std::cout << "[" << device << "] stream " << note.stream->streamId << " state "
                  << static_cast<unsigned>(note.stream->state) << " dropped " << note.stream->droppedCount << "\n";
        return;
    }
    case client::NotificationKind::CommandFinished: {
        std::cout << "[" << device << "] " << (note.text.has_value() ? *note.text : "command finished") << "\n";
        return;
    }
    }
}

void printUsage() {
    std::cout <<
        R"(uwbctl - UWB Pico client

Usage: uwbctl <command> [options]

Commands:
  version                        Client core version
  discover                       Probe the network and list devices
  list                           List the devices already in the registry
  connect <dev>... [--follow]    Connect one or more devices
  disconnect <dev>               Close the connection to a device
  info <dev>                     Full device view
  diagnostics                    Client diagnostics snapshot
                                 (--diagnostics also prints one at the end of
                                  connect --follow, stream, and monitor)
  did get <dev> <did>            Read a DID (name or 0x value)
  config get <dev>               Complete UWB configuration (DID 0xF01F)
  config set <dev> [entries]     Write configuration entries and save them
  config save <dev>              Run the SaveUwbConfiguration routine
  session <dev> <extended|default>
  stream range <dev> [--mode live|recording] [--duration ms] [--keep]
  stream stop <dev>
  routine <dev> <name> [--options hex]
  routine stop <dev>
  at <dev> "AT+CMD"              Raw AT passthrough (Extended Session)
  reset <dev> <soft|hard>
  security <dev> <hex-key>
  monitor [dev...] [--duration ms]

A device is addressed by name, UUID, or host:port (probe that endpoint first).
Each invocation is its own client process: 'list', 'info', and 'diagnostics'
report only what this process saw, and a connection ends when the process exits.

Common options:
  --target host:port        Discovery target (repeatable)
  --broadcast               Also probe the broadcast addresses
  --no-broadcast            Never fall back to broadcast probing
  --port <n>                Discovery port (default 13401)
  --role <control|observer> Requested activation role
  --key <hex>               Security Access key (service 0x27) used before
                            privileged commands on a secured device
  --duration <ms>           Stream or monitor duration (default 5000)
  --connect-timeout <ms>    Default 3000
  --request-timeout <ms>    Default 3000
  --routine-timeout <ms>    Default 15000
  --capacity <n>            Notification queue capacity (default 512)
  --probe-interval <ms>     Discovery re-probe interval (default 1000)
  --reconnect <on|off>
  --verbose
)";
}

client::ControllerConfig buildConfig(const CommandLine &args) {
    client::ControllerConfig config;
    const auto port = static_cast<std::uint16_t>(args.number("port", protocol::kDefaultProtocolPort));
    config.discovery.port = port;
    for (const std::string &text : args.targets) {
        auto endpoint = parseTarget(text);
        if (endpoint.has_value()) {
            config.discovery.targets.push_back(*endpoint);
        } else {
            std::cerr << "warning: ignoring malformed --target " << text << "\n";
        }
    }
    if (args.has("broadcast")) {
        // --broadcast adds the LAN broadcast addresses to the explicit targets.
        config.discovery.targets.push_back(client::Endpoint{"255.255.255.255", port});
        config.discovery.targets.push_back(client::Endpoint{"127.255.255.255", port});
    }
    config.discovery.useBroadcastTargets = args.targets.empty() && !args.has("no-broadcast");
    config.discovery.requestIntervalMs = static_cast<std::uint32_t>(args.number("probe-interval", 1000));
    config.options.desiredRole =
        args.value("role", "control") == "observer" ? protocol::ConnectionRole::Observer : protocol::ConnectionRole::Control;
    config.options.autoReconnect = args.value("reconnect", "on") != "off";
    config.options.timeouts.connectTimeoutMs = static_cast<std::uint32_t>(args.number("connect-timeout", 3000));
    config.options.timeouts.requestTimeoutMs = static_cast<std::uint32_t>(args.number("request-timeout", 3000));
    config.options.timeouts.longRoutineTimeoutMs = static_cast<std::uint32_t>(args.number("routine-timeout", 15000));
    config.options.timeouts.reconnectInitialDelayMs = static_cast<std::uint32_t>(args.number("retry-delay", 500));
    config.options.timeouts.reconnectMaxDelayMs = static_cast<std::uint32_t>(args.number("retry-max", 5000));
    config.notificationCapacity = static_cast<std::uint32_t>(args.number("capacity", 512));
    return config;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int commandDiscover(Cli &cli, const CommandLine &args) {
    cli.session.discoverNow();
    std::this_thread::sleep_for(args.duration("timeout", 1500ms));
    printDeviceTable(cli.session.listDevices());
    const auto stats = cli.session.diagnostics();
    std::cout << stats.discoveryRequestsSent << " probes sent, " << stats.discovered << " devices known, "
              << stats.malformedDatagrams << " malformed datagrams\n";
    return 0;
}

// A CLI process starts with an empty registry, so a read-only command probes
// first when it does not know the device yet.
void ensureRegistry(Cli &cli, const CommandLine &args) {
    if (!cli.session.listDevices().empty()) {
        return;
    }
    cli.session.discoverNow();
    std::this_thread::sleep_for(args.duration("timeout", 1200ms));
}

int commandInfo(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl info <dev>\n";
        return 2;
    }
    ensureRegistry(cli, args);
    auto uuid = cli.session.resolveDevice(args.words[1]);
    if (!uuid.has_value()) {
        std::cerr << "unknown device " << args.words[1] << "\n";
        return 1;
    }
    auto summary = cli.session.deviceSummary(*uuid);
    if (!summary.has_value()) {
        std::cerr << "unknown device " << args.words[1] << "\n";
        return 1;
    }
    printSummary(*summary);
    return 0;
}

void printDiagnostics(Cli &cli) {
    const auto stats = cli.session.diagnostics();
    std::cout << "discovered devices        " << stats.discovered << "\n";
    std::cout << "connections               " << stats.connections << " (active " << stats.activeConnections << ")\n";
    std::cout << "discovery probes          " << stats.discoveryRequestsSent << " sent, " << stats.discoveryResponses
              << " answers\n";
    std::cout << "frames                    " << stats.framesSent << " sent, " << stats.framesReceived << " received\n";
    std::cout << "requests                  " << stats.requestsSent << " sent, " << stats.responsesMatched
              << " matched\n";
    std::cout << "negative responses        " << stats.negativeResponses << "\n";
    std::cout << "timeouts                  " << stats.requestTimeouts << "\n";
    std::cout << "late/unmatched responses  " << stats.lateResponses << " / " << stats.unmatchedResponses << "\n";
    std::cout << "protocol errors           " << stats.protocolErrors << "\n";
    std::cout << "tx queue drops            " << stats.txQueueDrops << "\n";
    std::cout << "notification drops        " << stats.notificationsDropped << "\n";
    std::cout << "reconnect attempts        " << stats.reconnectAttempts << "\n";
}

int commandDiagnostics(Cli &cli, const CommandLine &) {
    printDiagnostics(cli);
    return 0;
}

[[nodiscard]] std::optional<protocol::Did> parseDid(const std::string &text) {
    if (auto numeric = CommandLine::parseUnsigned(text); numeric.has_value() && *numeric <= 0xFFFFU) {
        return static_cast<protocol::Did>(*numeric);
    }
    static const std::vector<std::pair<std::string, protocol::Did>> names = {
        {"uuid", protocol::Did::DeviceUuid},
        {"board", protocol::Did::PicoBoardUniqueId},
        {"name", protocol::Did::DeviceName},
        {"address", protocol::Did::LogicalAddress},
        {"firmware", protocol::Did::PicoFirmwareVersion},
        {"module", protocol::Did::UwbModuleVersion},
        {"capabilities", protocol::Did::EffectiveCapabilities},
        {"capability-detection", protocol::Did::CapabilityDetection},
        {"capability-override", protocol::Did::CapabilityOverride},
        {"timeouts", protocol::Did::TimeoutConfiguration},
        {"connection", protocol::Did::ConnectionStatus},
        {"uptime", protocol::Did::Uptime},
        {"rssi", protocol::Did::WifiRssi},
        {"parameters", protocol::Did::UwbDeviceParameters},
        {"twr", protocol::Did::UwbTwrParameters},
        {"pdoa", protocol::Did::UwbPdoaParameters},
        {"work-mode", protocol::Did::UwbWorkMode},
        {"mode", protocol::Did::UwbMode},
        {"sensor", protocol::Did::UwbLatestSensorData},
        {"distance", protocol::Did::UwbLatestDistance},
        {"metadata", protocol::Did::UwbMiscellaneousMetadata},
        {"config", protocol::Did::UwbCompleteConfiguration},
        {"calibration", protocol::Did::DeviceCalibration},
    };
    for (const auto &entry : names) {
        if (entry.first == text) {
            return entry.second;
        }
    }
    return std::nullopt;
}

int commandDidGet(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 3) {
        std::cerr << "usage: uwbctl did get <dev> <did>\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    auto did = parseDid(args.words[2]);
    if (!did.has_value()) {
        std::cerr << "unknown DID " << args.words[2] << "\n";
        return 2;
    }
    auto record = cli.readRecord(opened.value(), *did, cli.commandTimeout);
    if (!record.ok()) {
        std::cerr << "failed: " << record.error().toString() << "\n";
        return 1;
    }
    printDidRecord(*did, protocol::bytesOf(record.value()));
    return 0;
}

int commandConfigGet(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl config get <dev>\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    auto record = cli.readConfigurationRecord(opened.value());
    if (!record.ok()) {
        std::cerr << "failed: " << record.error().toString() << "\n";
        return 1;
    }
    auto config = protocol::decodeCompleteUwbConfig(protocol::bytesOf(record.value()));
    if (!config.ok()) {
        std::cerr << "malformed configuration record (protocol error " << static_cast<unsigned>(config.code()) << ")\n";
        return 1;
    }
    printCompleteConfig(config.value());
    return 0;
}

// config set <dev> [--id N] [--role N] [--channel N] [--rate N]
//                  [--tag-capacity N] [--antenna-delay N] [--twr-flags N]
//                  [--dlist N] [--klist N] [--network N] [--anchor N] [--filter N]
//                  [--pdoa-offset N] [--range-offset N]
//                  [--did 0xF010=<hex>]
int commandConfigSet(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl config set <dev> [entry options]\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const auto uuid = opened.value();
    if (!cli.enterExtendedSession(uuid)) {
        return 1;
    }

    // Start from the stored configuration so unspecified entries keep their values.
    protocol::CompleteUwbConfig config;
    auto current = cli.readConfigurationRecord(uuid);
    if (current.ok()) {
        auto decoded = protocol::decodeCompleteUwbConfig(protocol::bytesOf(current.value()));
        if (decoded.ok()) {
            config = std::move(decoded.value());
        }
    }

    auto replace = [&](protocol::Did did, protocol::ByteBuffer data) {
        const std::uint16_t id = static_cast<std::uint16_t>(did);
        for (protocol::DidRecordEntry &entry : config.entries) {
            if (entry.did == id) {
                entry.data = std::move(data);
                return;
            }
        }
        config.entries.push_back(protocol::DidRecordEntry{id, std::move(data)});
    };

    if (args.has("id") || args.has("role") || args.has("channel") || args.has("rate")) {
        protocol::UwbDeviceParametersRecord record;
        if (const auto *existing = protocol::findCompleteConfigEntry(config, protocol::Did::UwbDeviceParameters)) {
            auto decoded = protocol::decodeUwbDeviceParametersRecord(existing->dataBytes());
            if (decoded.ok()) {
                record = decoded.value();
            }
        }
        record.id = static_cast<std::uint16_t>(args.number("id", record.id));
        record.role = static_cast<std::uint8_t>(args.number("role", record.role));
        record.channel = static_cast<std::uint8_t>(args.number("channel", record.channel));
        record.rate = static_cast<std::uint16_t>(args.number("rate", record.rate));
        replace(protocol::Did::UwbDeviceParameters, protocol::encodeRecord(record));
    }

    if (args.has("tag-capacity") || args.has("antenna-delay") || args.has("twr-flags")) {
        protocol::UwbTwrParametersRecord record;
        if (const auto *existing = protocol::findCompleteConfigEntry(config, protocol::Did::UwbTwrParameters)) {
            auto decoded = protocol::decodeUwbTwrParametersRecord(existing->dataBytes());
            if (decoded.ok()) {
                record = decoded.value();
            }
        }
        record.tagCapacity = static_cast<std::uint16_t>(args.number("tag-capacity", record.tagCapacity));
        record.antennaDelay = static_cast<std::uint16_t>(args.number("antenna-delay", record.antennaDelay));
        record.flags = static_cast<std::uint16_t>(args.number("twr-flags", record.flags));
        replace(protocol::Did::UwbTwrParameters, protocol::encodeRecord(record));
    }

    if (args.has("dlist") || args.has("klist") || args.has("network") || args.has("anchor") || args.has("filter") ||
        args.has("pdoa-offset") || args.has("range-offset")) {
        protocol::UwbPdoaParametersRecord record;
        if (const auto *existing = protocol::findCompleteConfigEntry(config, protocol::Did::UwbPdoaParameters)) {
            auto decoded = protocol::decodeUwbPdoaParametersRecord(existing->dataBytes());
            if (decoded.ok()) {
                record = decoded.value();
            }
        }
        record.dlist = static_cast<std::uint16_t>(args.number("dlist", record.dlist));
        record.klist = static_cast<std::uint16_t>(args.number("klist", record.klist));
        record.network = static_cast<std::uint32_t>(args.number("network", record.network));
        record.anchorId = static_cast<std::uint16_t>(args.number("anchor", record.anchorId));
        record.filterEnabled = static_cast<std::uint8_t>(args.number("filter", record.filterEnabled));
        record.pdoaOffsetRaw = static_cast<std::int32_t>(args.number("pdoa-offset", static_cast<std::uint64_t>(record.pdoaOffsetRaw)));
        record.rangeOffsetMm = static_cast<std::int32_t>(args.number("range-offset", static_cast<std::uint64_t>(record.rangeOffsetMm)));
        replace(protocol::Did::UwbPdoaParameters, protocol::encodeRecord(record));
    }

    if (args.has("did")) {
        const std::string spec = args.value("did");
        const auto equals = spec.find('=');
        if (equals == std::string::npos) {
            std::cerr << "malformed --did " << spec << " (expected DID=HEX)\n";
            return 2;
        }
        auto did = parseDid(spec.substr(0, equals));
        if (!did.has_value()) {
            std::cerr << "unknown DID in --did " << spec << "\n";
            return 2;
        }
        auto data = parseHexBytes(spec.substr(equals + 1));
        if (data.empty()) {
            std::cerr << "malformed --did data\n";
            return 2;
        }
        replace(*did, std::move(data));
    }

    if (config.entries.empty()) {
        std::cerr << "nothing to write; pass --id/--role/--channel/--rate/--tag-capacity/--did\n";
        return 2;
    }

    Latch<std::string> writeLatch;
    cli.session.post([&cli, uuid, &config, &writeLatch] {
        cli.session.controller().writeConfiguration(uuid, config, [&writeLatch](client::ClientResult<std::string> result) {
            writeLatch.complete(std::move(result));
        });
    });
    const int status = cli.finish(writeLatch, args.has("verbose"), cli.commandTimeout);
    if (status != 0) {
        return status;
    }

    Latch<std::string> saveLatch;
    cli.session.post([&cli, uuid, &saveLatch] {
        cli.session.controller().saveConfiguration(uuid, [&saveLatch](client::ClientResult<std::string> result) {
            saveLatch.complete(std::move(result));
        });
    });
    return cli.finish(saveLatch, args.has("verbose"), cli.commandTimeout);
}

int commandConfigSave(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl config save <dev>\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    Latch<std::string> latch;
    cli.session.post([&cli, uuid = opened.value(), &latch] {
        cli.session.controller().saveConfiguration(uuid, [&latch](client::ClientResult<std::string> result) {
            latch.complete(std::move(result));
        });
    });
    return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
}

int commandSession(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 3) {
        std::cerr << "usage: uwbctl session <dev> <extended|default>\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const bool extended = args.words[2] == "extended";
    Latch<std::string> latch;
    cli.session.post([&cli, uuid = opened.value(), extended, &latch] {
        cli.session.controller().requestSession(uuid, extended ? protocol::SessionId::Extended : protocol::SessionId::Default,
                                                [&latch](client::ClientResult<std::string> result) {
                                                    latch.complete(std::move(result));
                                                });
    });
    return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
}

// Streams measurements to the notification queue and prints them here (§56).
int commandStream(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl stream <range|stop> <dev>\n";
        return 2;
    }
    const bool stop = args.sub == "stop";
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const auto uuid = opened.value();

    Latch<std::string> latch;
    if (stop) {
        cli.session.post([&cli, uuid, &latch] {
            cli.session.controller().stopMeasurementStream(uuid, [&latch](client::ClientResult<std::string> result) {
                latch.complete(std::move(result));
            });
        });
        return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
    }

    const bool recording = args.value("mode", "live") == "recording";
    cli.session.post([&cli, uuid, recording, &latch] {
        cli.session.controller().startMeasurementStream(
            uuid, recording ? protocol::StreamMode::Recording : protocol::StreamMode::Live,
            [&latch](client::ClientResult<std::string> result) { latch.complete(std::move(result)); });
    });
    const int status = cli.finish(latch, args.has("verbose"), cli.commandTimeout);
    if (status != 0) {
        return status;
    }

    const auto deadline = std::chrono::steady_clock::now() + args.duration("duration", 5000ms);
    std::size_t measurements = 0;
    while (std::chrono::steady_clock::now() < deadline && !interrupted.load()) {
        client::ClientNotification note;
        if (cli.session.notifications().waitPop(note, 100ms)) {
            if (note.kind == client::NotificationKind::Measurement) {
                measurements++;
            }
            printNotification(note);
        }
    }
    std::cout << measurements << " measurements\n";
    if (args.has("diagnostics")) {
        printDiagnostics(cli);
    }

    if (args.has("keep")) {
        return 0;
    }
    Latch<std::string> stopLatch;
    cli.session.post([&cli, uuid, &stopLatch] {
        cli.session.controller().stopMeasurementStream(uuid, [&stopLatch](client::ClientResult<std::string> result) {
            stopLatch.complete(std::move(result));
        });
    });
    static_cast<void>(cli.finish(stopLatch, args.has("verbose"), cli.commandTimeout));
    return 0;
}

[[nodiscard]] std::optional<protocol::RoutineId> parseRoutine(const std::string &text) {
    static const std::vector<std::pair<std::string, protocol::RoutineId>> names = {
        {"initialize", protocol::RoutineId::InitializeUwbBackend},
        {"acquire", protocol::RoutineId::UwbMeasurementAcquisition},
        {"calibrate", protocol::RoutineId::DeviceCalibration},
        {"test-led", protocol::RoutineId::TestLed},
        {"test-oled", protocol::RoutineId::TestOled},
        {"restart-uwb", protocol::RoutineId::RestartUwbModule},
        {"restore-defaults", protocol::RoutineId::RestoreUwbDefaults},
        {"save-config", protocol::RoutineId::SaveUwbConfiguration},
        {"add-tag", protocol::RoutineId::AddTag},
        {"delete-tag", protocol::RoutineId::DeleteTag},
    };
    for (const auto &entry : names) {
        if (entry.first == text) {
            return entry.second;
        }
    }
    if (auto numeric = CommandLine::parseUnsigned(text);
        numeric.has_value() && protocol::isKnownRoutine(static_cast<std::uint16_t>(*numeric))) {
        return static_cast<protocol::RoutineId>(*numeric);
    }
    return std::nullopt;
}

int commandRoutine(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl routine <dev> <name> | uwbctl routine stop <dev>\n";
        return 2;
    }
    const bool stop = args.sub == "stop";
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl routine <dev> <name> | uwbctl routine stop <dev>\n";
        return 2;
    }

    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const auto uuid = opened.value();

    if (stop) {
        Latch<std::string> latch;
        cli.session.post([&cli, uuid, &latch] {
            cli.session.controller().stopRoutine(uuid, [&latch](client::ClientResult<std::string> result) {
                latch.complete(std::move(result));
            });
        });
        return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
    }

    if (args.words.size() < 3) {
        std::cerr << "usage: uwbctl routine <dev> <name>\n";
        return 2;
    }
    auto routine = parseRoutine(args.words[2]);
    if (!routine.has_value()) {
        std::cerr << "unknown routine " << args.words[2] << "\n";
        return 2;
    }
    const bool longRunning = args.words[2] == "calibrate" || args.words[2] == "initialize";
    const auto timeout = longRunning ? std::chrono::milliseconds(20000) : cli.commandTimeout;

    Latch<std::string> latch;
    cli.session.post([&cli, uuid, routine = *routine, options = parseHexBytes(args.value("options")), &latch] {
        cli.session.controller().startRoutine(uuid, routine, options, [&latch](client::ClientResult<std::string> result) {
            latch.complete(std::move(result));
        });
    });
    return cli.finish(latch, args.has("verbose"), timeout);
}

int commandAt(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 3) {
        std::cerr << "usage: uwbctl at <dev> \"AT+CMD\"\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const auto uuid = opened.value();

    // Raw AT passthrough is only open in the Extended Session (§22.3, §30).
    if (!cli.enterExtendedSession(uuid)) {
        std::cerr << "raw AT needs the Extended Session\n";
        return 1;
    }

    Latch<std::string> latch;
    cli.session.post([&cli, uuid, command = args.words[2], &latch] {
        cli.session.controller().executeAtCommand(uuid, command, [&latch](client::ClientResult<std::string> result) {
            latch.complete(std::move(result));
        });
    });
    return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
}

int commandReset(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl reset <dev> <soft|hard>\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const bool hard = args.words.size() > 2 && args.words[2] == "hard";

    // Device Reset needs Control role and the Extended Session (§24).
    if (!cli.enterExtendedSession(opened.value())) {
        return 1;
    }

    Latch<std::string> latch;
    cli.session.post([&cli, uuid = opened.value(), hard, &latch] {
        cli.session.controller().resetDevice(uuid, hard ? protocol::DeviceResetType::Hard : protocol::DeviceResetType::Soft,
                                             [&latch](client::ClientResult<std::string> result) {
                                                 latch.complete(std::move(result));
                                             });
    });
    return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
}

int commandSecurity(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 3) {
        std::cerr << "usage: uwbctl security <dev> <hex-key>\n";
        return 2;
    }
    auto opened = cli.openDevice(args.words[1]);
    if (!opened.ok()) {
        std::cerr << opened.error().toString() << "\n";
        return 1;
    }
    const auto key = parseHexBytes(args.words[2]);
    if (key.empty()) {
        std::cerr << "the key must be hex bytes\n";
        return 2;
    }
    Latch<std::string> latch;
    cli.session.post([&cli, uuid = opened.value(), key, &latch] {
        cli.session.controller().securityUnlock(uuid, key, [&latch](client::ClientResult<std::string> result) {
            latch.complete(std::move(result));
        });
    });
    return cli.finish(latch, args.has("verbose"), cli.commandTimeout);
}

int commandConnect(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl connect <dev>... [--follow]\n";
        return 2;
    }
    for (std::size_t i = 1; i < args.words.size(); ++i) {
        auto opened = cli.openDevice(args.words[i]);
        if (!opened.ok()) {
            std::cerr << opened.error().toString() << "\n";
            return 1;
        }
        auto summary = cli.session.deviceSummary(opened.value());
        if (summary.has_value()) {
            std::cout << "connected " << summary->name << " " << summary->endpoint.toString() << " role "
                      << client::roleName(summary->role) << "\n";
        }
    }
    if (args.has("diagnostics")) {
        printDiagnostics(cli);
    }
    if (!args.has("follow")) {
        return 0;
    }

    // Stay attached and report what the devices send.
    const auto deadline = std::chrono::steady_clock::now() + args.duration("duration", 30000ms);
    while (std::chrono::steady_clock::now() < deadline && !interrupted.load()) {
        client::ClientNotification note;
        if (cli.session.notifications().waitPop(note, 200ms)) {
            printNotification(note);
        }
    }
    return 0;
}

int commandDisconnect(Cli &cli, const CommandLine &args) {
    if (args.words.size() < 2) {
        std::cerr << "usage: uwbctl disconnect <dev>\n";
        return 2;
    }
    ensureRegistry(cli, args);
    auto uuid = cli.session.resolveDevice(args.words[1]);
    if (!uuid.has_value()) {
        std::cerr << "unknown device " << args.words[1] << "\n";
        return 1;
    }
    cli.session.disconnectDevice(*uuid);
    if (!cli.waitFor([&] {
            auto summary = cli.session.deviceSummary(*uuid);
            return !summary.has_value() || summary->state == client::ConnectionState::Disconnected;
        },
        3000ms)) {
        std::cerr << "disconnect did not complete\n";
        return 1;
    }
    std::cout << "disconnected " << args.words[1] << "\n";
    return 0;
}

int commandMonitor(Cli &cli, const CommandLine &args) {
    if (args.words.size() == 1) {
        cli.session.discoverNow();
        std::this_thread::sleep_for(800ms);
    }
    for (std::size_t i = 1; i < args.words.size(); ++i) {
        auto opened = cli.openDevice(args.words[i]);
        if (!opened.ok()) {
            std::cerr << opened.error().toString() << "\n";
            return 1;
        }
    }
    printDeviceTable(cli.session.listDevices());

    const auto deadline = std::chrono::steady_clock::now() + args.duration("duration", 10000ms);
    while (std::chrono::steady_clock::now() < deadline && !interrupted.load()) {
        client::ClientNotification note;
        if (cli.session.notifications().waitPop(note, 200ms)) {
            printNotification(note);
        }
    }
    client::ClientNotification note;
    while (cli.session.notifications().tryPop(note)) {
        printNotification(note);
    }
    if (args.has("diagnostics")) {
        printDiagnostics(cli);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    CommandLine args = parseArguments(argc, argv);
    if (args.words.empty() || args.has("help")) {
        printUsage();
        return args.words.empty() ? 2 : 0;
    }

    const std::string command = args.words[0];
    if (command == "version") {
        std::cout << "uwbctl " << uwb::client::clientCoreVersion() << "\n";
        return 0;
    }

    // Normalise "<group> <sub> <dev> ..." so every command finds the device in
    // words[1] and the group verb in args.sub.
    if (command == "did" || command == "config" || command == "stream") {
        args.sub = args.words.size() > 1 ? args.words[1] : "get";
        if (args.words.size() > 1) {
            args.words.erase(args.words.begin() + 1);
        }
    } else if (command == "routine") {
        if (args.words.size() > 1 && args.words[1] == "stop") {
            args.sub = "stop";
            args.words.erase(args.words.begin() + 1);
        } else {
            args.sub = "start";
        }
    }

    Cli cli(buildConfig(args));
    cli.connectTimeout = args.duration("connect-timeout", 3000ms);
    cli.commandTimeout = args.duration("request-timeout", 8000ms);
    cli.key = parseHexBytes(args.value("key"));

    if (command == "discover") {
        return commandDiscover(cli, args);
    }
    if (command == "list") {
        ensureRegistry(cli, args);
        printDeviceTable(cli.session.listDevices());
        return 0;
    }
    if (command == "info") {
        return commandInfo(cli, args);
    }
    if (command == "diagnostics") {
        return commandDiagnostics(cli, args);
    }
    if (command == "did") {
        if (args.sub != "get") {
            std::cerr << "unknown did subcommand " << args.sub << "\n";
            return 2;
        }
        return commandDidGet(cli, args);
    }
    if (command == "config") {
        const std::string sub = args.sub;
        if (sub == "get") {
            return commandConfigGet(cli, args);
        }
        if (sub == "set") {
            return commandConfigSet(cli, args);
        }
        if (sub == "save") {
            return commandConfigSave(cli, args);
        }
        std::cerr << "unknown config subcommand " << sub << "\n";
        return 2;
    }
    if (command == "session") {
        return commandSession(cli, args);
    }
    if (command == "stream") {
        return commandStream(cli, args);
    }
    if (command == "routine") {
        return commandRoutine(cli, args);
    }
    if (command == "at") {
        return commandAt(cli, args);
    }
    if (command == "reset") {
        return commandReset(cli, args);
    }
    if (command == "security") {
        return commandSecurity(cli, args);
    }
    if (command == "connect") {
        return commandConnect(cli, args);
    }
    if (command == "disconnect") {
        return commandDisconnect(cli, args);
    }
    if (command == "monitor") {
        return commandMonitor(cli, args);
    }

    std::cerr << "unknown command " << command << "\n";
    printUsage();
    return 2;
}
