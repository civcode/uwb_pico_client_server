#pragma once

// Deterministic fakes for the server-core interfaces (implementation plan §18).
// They record every interaction so tests can assert on behaviour instead of
// timing.

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include <optional>
#include <vector>

#include "uwb/protocol/bytes.hpp"
#include "uwb/protocol/constants.hpp"
#include "uwb/protocol/errors.hpp"
#include "uwb/protocol/generic_header.hpp"
#include "uwb/protocol/payload_types.hpp"
#include "uwb/protocol/payloads.hpp"
#include "uwb/protocol/services.hpp"
#include "uwb/server/clock.hpp"
#include "uwb/server/config_storage.hpp"
#include "uwb/server/logger.hpp"
#include "uwb/server/security_provider.hpp"
#include "uwb/server/transport.hpp"
#include "uwb/server/uwb_backend.hpp"

namespace uwb::test {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::Did;
using uwb::protocol::Result;
using uwb::protocol::RoutineId;

// ---------------------------------------------------------------------------
class FakeClock final : public uwb::server::IClock {
public:
    explicit FakeClock(std::uint64_t startUs = 1'000'000) noexcept : nowUs_(startUs) {}

    [[nodiscard]] std::uint64_t monotonicUs() const noexcept override { return nowUs_; }

    void advanceMs(std::uint64_t ms) noexcept { nowUs_ += uwb::server::msToUs(ms); }
    void advanceUs(std::uint64_t us) noexcept { nowUs_ += us; }
    void setUs(std::uint64_t us) noexcept { nowUs_ = us; }

private:
    std::uint64_t nowUs_;
};

// ---------------------------------------------------------------------------
class FakeConnectionWriter final : public uwb::server::IConnectionWriter {
public:
    [[nodiscard]] bool writeFrame(ConstBytes frame) override {
        if (!open_ || acceptedThisRound_ >= perTickLimit_) {
            return false;
        }
        frames_.emplace_back(frame.begin(), frame.end());
        ++acceptedThisRound_;
        return true;
    }

    [[nodiscard]] bool isOpen() const noexcept override { return open_; }

    void close() noexcept { open_ = false; }
    void resetTickLimit(std::size_t limit) noexcept { acceptedThisRound_ = 0; perTickLimit_ = limit; }
    void clear() noexcept { frames_.clear(); }

    [[nodiscard]] const std::vector<ByteBuffer> &frames() const noexcept { return frames_; }
    [[nodiscard]] std::size_t frameCount() const noexcept { return frames_.size(); }

private:
    bool open_ = true;
    std::size_t acceptedThisRound_ = 0;
    std::size_t perTickLimit_ = 1000;
    std::vector<ByteBuffer> frames_;
};

// ---------------------------------------------------------------------------
class FakeStorage final : public uwb::server::IConfigurationStorage {
public:
    [[nodiscard]] bool available() const noexcept override { return available_; }

    [[nodiscard]] Result<ByteBuffer> readActive() override {
        return Result<ByteBuffer>::ok(active_);
    }
    [[nodiscard]] Result<ByteBuffer> readBackup() override { return Result<ByteBuffer>::ok(backup_); }

    [[nodiscard]] Result<bool> stage(ConstBytes candidate) override {
        if (!available_ || !stageSucceeds_) {
            return Result<bool>::error(uwb::protocol::ProtocolErrorCode::InvalidChecksum);
        }
        staged_ = ByteBuffer(candidate.begin(), candidate.end());
        ++stageCalls_;
        return Result<bool>::ok(true);
    }

    [[nodiscard]] Result<bool> commit() override {
        ++commitCalls_;
        if (!available_ || !commitSucceeds_) {
            return Result<bool>::error(uwb::protocol::ProtocolErrorCode::InvalidChecksum);
        }
        backup_ = active_;
        active_ = staged_;
        staged_.clear();
        return Result<bool>::ok(true);
    }

    void discardStaged() noexcept override {
        ++discardCalls_;
        staged_.clear();
    }

    [[nodiscard]] Result<bool> restoreBackup() override {
        if (!available_) {
            return Result<bool>::error(uwb::protocol::ProtocolErrorCode::InvalidChecksum);
        }
        active_ = backup_;
        return Result<bool>::ok(true);
    }

    bool available_ = true;
    bool stageSucceeds_ = true;
    bool commitSucceeds_ = true;
    ByteBuffer active_;
    ByteBuffer backup_;
    ByteBuffer staged_;
    std::uint32_t stageCalls_ = 0;
    std::uint32_t commitCalls_ = 0;
    std::uint32_t discardCalls_ = 0;
};

// ---------------------------------------------------------------------------
class FakeSecurityProvider final : public uwb::server::ISecurityProvider {
public:
    [[nodiscard]] Result<uwb::server::SecuritySeed> createSeed(uwb::server::ConnectionId connection) override {
        ++seedCalls_;
        lastSeedConnection = connection;
        if (!seedSucceeds_) {
            return Result<uwb::server::SecuritySeed>::error(uwb::protocol::ProtocolErrorCode::InvalidField);
        }
        issuedSeed = seedValue;
        return Result<uwb::server::SecuritySeed>::ok(seedValue);
    }

    [[nodiscard]] uwb::server::SecurityOutcome
    verifyKey(uwb::server::ConnectionId connection, uwb::server::SecuritySeed seed,
              uwb::server::SecurityKey key) override {
        ++keyCalls_;
        lastVerifiedSeed = seed;
        lastVerifiedKey = key;
        (void)connection;
        if (key == expectedKey && decision == uwb::server::SecurityDecision::Accepted) {
            return uwb::server::SecurityOutcome{uwb::server::SecurityDecision::Accepted, 0};
        }
        return uwb::server::SecurityOutcome{decision, retryAfterMs};
    }

    void reset(uwb::server::ConnectionId) noexcept override { ++resetCalls_; }
    void noteFailure(uwb::server::ConnectionId) noexcept override { ++failureCalls_; }

    uwb::server::SecuritySeed seedValue = 0x11223344U;
    uwb::server::SecurityKey expectedKey = 0xAABBCCDDU;
    uwb::server::SecurityDecision decision = uwb::server::SecurityDecision::Accepted;
    std::uint32_t retryAfterMs = 0;
    bool seedSucceeds_ = true;

    uwb::server::ConnectionId lastSeedConnection = 0;
    uwb::server::SecuritySeed issuedSeed = 0;
    uwb::server::SecuritySeed lastVerifiedSeed = 0;
    uwb::server::SecurityKey lastVerifiedKey = 0;
    std::uint32_t seedCalls_ = 0;
    std::uint32_t keyCalls_ = 0;
    std::uint32_t resetCalls_ = 0;
    std::uint32_t failureCalls_ = 0;
};

// ---------------------------------------------------------------------------
// Backend fake: records submissions and lets the test deliver completions.
class FakeBackend final : public uwb::server::IUwbBackend {
public:
    struct Submission {
        enum class Kind : std::uint8_t {
            At = 0,
            DidRead = 1,
            DidWrite = 2,
            Routine = 3,
        };
        Kind kind = Kind::At;
        std::uint32_t owner = 0;
        Did did = static_cast<Did>(0);
        RoutineId routine = RoutineId::InitializeUwbBackend;
        ByteBuffer payload;
        uwb::server::OperationId operation = uwb::server::kInvalidOperationId;
        std::uint32_t timeoutMs = 0;
        bool cancelled = false;

        uwb::server::AtCompletion atDone;
        uwb::server::DidReadCompletion readDone;
        uwb::server::DidWriteCompletion writeDone;
        uwb::server::RoutineCompletion routineDone;
    };

    [[nodiscard]] Result<uwb::server::OperationId> submitAt(std::uint32_t owner, ConstBytes command,
                                                            std::uint32_t timeoutMs,
                                                            uwb::server::AtCompletion done) override {
        return record(Submission::Kind::At, owner, static_cast<Did>(0), RoutineId::InitializeUwbBackend, command,
                      timeoutMs, std::move(done));
    }

    [[nodiscard]] Result<uwb::server::OperationId> readDid(std::uint32_t owner, Did did,
                                                            uwb::server::DidReadCompletion done) override {
        return record(Submission::Kind::DidRead, owner, did, RoutineId::InitializeUwbBackend, {}, 0, std::move(done));
    }

    [[nodiscard]] Result<uwb::server::OperationId> writeDid(std::uint32_t owner, Did did, ConstBytes recordBytes,
                                                             uwb::server::DidWriteCompletion done) override {
        return record(Submission::Kind::DidWrite, owner, did, RoutineId::InitializeUwbBackend, recordBytes, 0,
                      std::move(done));
    }

    [[nodiscard]] Result<uwb::server::OperationId> startRoutine(std::uint32_t owner, RoutineId routine,
                                                                ConstBytes options,
                                                                uwb::server::RoutineCompletion done) override {
        return record(Submission::Kind::Routine, owner, static_cast<Did>(0), routine, options, 0, std::move(done));
    }

    [[nodiscard]] bool requestRoutineCancel(uwb::server::OperationId operation) noexcept override {
        for (Submission &s : submissions_) {
            if (s.operation == operation) {
                s.cancelled = true;
                return true;
            }
        }
        return false;
    }

    void cancelOperations(std::uint32_t owner) noexcept override {
        for (Submission &s : submissions_) {
            if (s.owner == owner) {
                s.cancelled = true;
            }
        }
    }

    void tick(std::uint64_t) noexcept override { ++tickCalls_; }

    [[nodiscard]] bool busy() const noexcept override { return busy_; }
    [[nodiscard]] std::size_t pendingOperations() const noexcept override { return submissions_.size(); }

    // --- test controls ------------------------------------------------------
    bool accept_ = true;
    bool busy_ = false;
    std::vector<Submission> submissions_;
    std::uint32_t tickCalls_ = 0;

    std::size_t countOf(Submission::Kind kind) const noexcept {
        std::size_t n = 0;
        for (const Submission &s : submissions_) {
            if (s.kind == kind) {
                ++n;
            }
        }
        return n;
    }

    Submission *find(std::size_t index) noexcept {
        return index < submissions_.size() ? &submissions_[index] : nullptr;
    }

    Submission *findByOperation(uwb::server::OperationId op) noexcept {
        for (Submission &s : submissions_) {
            if (s.operation == op) {
                return &s;
            }
        }
        return nullptr;
    }

    void completeAt(std::size_t index, uwb::server::AtResult result) {
        if (Submission *s = find(index); s != nullptr && s->atDone) {
            s->atDone(std::move(result));
        }
    }
    void completeRead(std::size_t index, uwb::server::DidReadResult result) {
        if (Submission *s = find(index); s != nullptr && s->readDone) {
            s->readDone(std::move(result));
        }
    }
    void completeWrite(std::size_t index, uwb::server::DidWriteResult result) {
        if (Submission *s = find(index); s != nullptr && s->writeDone) {
            s->writeDone(result);
        }
    }
    void completeRoutine(std::size_t index, uwb::server::RoutineResult result) {
        if (Submission *s = find(index); s != nullptr && s->routineDone) {
            s->routineDone(std::move(result));
        }
    }

private:
    Result<uwb::server::OperationId> record(Submission::Kind kind, std::uint32_t owner, Did did, RoutineId routine,
                                            ConstBytes payload, std::uint32_t timeoutMs, auto &&done) {
        if (!accept_) {
            return Result<uwb::server::OperationId>::error(uwb::protocol::ProtocolErrorCode::UnsupportedService);
        }
        Submission submission;
        submission.kind = kind;
        submission.owner = owner;
        submission.did = did;
        submission.routine = routine;
        submission.payload.assign(payload.begin(), payload.end());
        submission.timeoutMs = timeoutMs;
        submission.operation = nextOperation_++;
        assignCallback(submission, std::forward<decltype(done)>(done));
        submissions_.push_back(std::move(submission));
        return Result<uwb::server::OperationId>::ok(submissions_.back().operation);
    }

    static void assignCallback(Submission &s, uwb::server::AtCompletion done) {
        s.atDone = std::move(done);
    }
    static void assignCallback(Submission &s, uwb::server::DidReadCompletion done) {
        s.readDone = std::move(done);
    }
    static void assignCallback(Submission &s, uwb::server::DidWriteCompletion done) {
        s.writeDone = std::move(done);
    }
    static void assignCallback(Submission &s, uwb::server::RoutineCompletion done) {
        s.routineDone = std::move(done);
    }

    uwb::server::OperationId nextOperation_ = 1;
};

// ---------------------------------------------------------------------------
class FakeLogger final : public uwb::server::ILogger {
public:
    void log(uwb::server::LogLevel level, std::string_view message) noexcept override {
        entries.push_back({level, std::string(message)});
    }

    std::vector<std::pair<uwb::server::LogLevel, std::string>> entries;
};

} // namespace uwb::test

// ---------------------------------------------------------------------------
// Frame building/parsing helpers for server-core tests.
namespace uwb::test {
namespace frame {

using uwb::protocol::ByteBuffer;
using uwb::protocol::ConstBytes;
using uwb::protocol::ApplicationEnvelope;
using uwb::protocol::ConnectionActivationRequest;
using uwb::protocol::ConnectionActivationResponse;
using uwb::protocol::PayloadType;
using uwb::protocol::ServiceNrc;

struct ServerFrame {
    PayloadType type = PayloadType::GenericHeaderNack;
    ByteBuffer payload;
    std::uint32_t declaredLength = 0;
};

inline ByteBuffer client(std::uint16_t addr, std::uint32_t txn, ConstBytes servicePdu, std::uint16_t serverAddress,
                         bool ackRequired = true) {
    ApplicationEnvelope env;
    env.sourceLogicalAddress = addr;
    env.targetLogicalAddress = serverAddress;
    env.transactionId = txn;
    env.flags = ackRequired ? uwb::protocol::kApplicationFlagAckRequired : 0;
    env.servicePdu.assign(servicePdu.begin(), servicePdu.end());
    const ByteBuffer message = uwb::protocol::encodeApplicationEnvelope(env);
    return uwb::protocol::encodeFrame(PayloadType::ApplicationMessage, uwb::protocol::bytesOf(message)).value();
}

inline ByteBuffer activation(const ConnectionActivationRequest &request) {
    return uwb::protocol::encodeFrame(PayloadType::ConnectionActivationRequest,
                                      uwb::protocol::bytesOf(uwb::protocol::encodeConnectionActivationRequest(request)))
        .value();
}

inline ByteBuffer aliveCheck(std::uint64_t nonce) {
    uwb::protocol::AliveCheckRequest request;
    request.nonce = nonce;
    return uwb::protocol::encodeFrame(PayloadType::AliveCheckRequest,
                                      uwb::protocol::bytesOf(uwb::protocol::encodeAliveCheckRequest(request)))
        .value();
}

inline ByteBuffer raw(PayloadType type, ConstBytes payload) {
    return uwb::protocol::encodeFrame(type, payload).value();
}

inline std::vector<ServerFrame> parse(const std::vector<ByteBuffer> &wire) {
    std::vector<ServerFrame> out;
    for (const ByteBuffer &frame : wire) {
        ConstBytes bytes{frame.data(), frame.size()};
        auto header = uwb::protocol::decodeGenericHeader(bytes);
        if (!header.ok()) {
            continue;
        }
        ServerFrame sf;
        sf.type = header.value().payloadType;
        sf.declaredLength = header.value().payloadLength;
        ConstBytes payload = bytes.subspan(uwb::protocol::kGenericHeaderSize);
        out.push_back({sf.type, ByteBuffer(payload.begin(), payload.end()), sf.declaredLength});
    }
    return out;
}

inline ApplicationEnvelope envelope(const ServerFrame &frame) {
    auto decoded = uwb::protocol::decodeApplicationEnvelope(uwb::protocol::ConstBytes{frame.payload.data(),
                                                                                       frame.payload.size()});
    return decoded.ok() ? decoded.value() : ApplicationEnvelope{};
}

// Returns a copy: a span into a local envelope would dangle.
inline ByteBuffer servicePdu(const ServerFrame &frame) {
    return envelope(frame).servicePdu;
}

inline std::optional<std::uint8_t> sid(const ServerFrame &frame) {
    const ByteBuffer pdu = servicePdu(frame);
    if (pdu.empty()) {
        return std::nullopt;
    }
    return pdu[0];
}

inline std::optional<ServiceNrc> nrc(const ServerFrame &frame) {
    const ByteBuffer pdu = servicePdu(frame);
    if (pdu.empty() || pdu[0] != uwb::protocol::kNegativeResponseSid) {
        return std::nullopt;
    }
    auto decoded = uwb::protocol::decodeNegativeResponse(uwb::protocol::bytesOf(pdu));
    return decoded.ok() ? std::optional<ServiceNrc>{decoded.value().second} : std::nullopt;
}

inline std::size_t countType(const std::vector<ServerFrame> &frames, PayloadType type) noexcept {
    std::size_t n = 0;
    for (const ServerFrame &f : frames) {
        if (f.type == type) {
            ++n;
        }
    }
    return n;
}

} // namespace frame
} // namespace uwb::test
