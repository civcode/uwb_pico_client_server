#include <catch2/catch_all.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "client_fakes.hpp"
#include "uwb/client/client_connection.hpp"
#include "uwb/protocol/events.hpp"

using namespace uwb::client;
using namespace uwb::client::test;
using namespace uwb::protocol;
namespace domain = uwb::domain;

namespace {

constexpr std::uint16_t kServerAddress = 0x1000;
constexpr std::uint16_t kClientAddress = 0x0E01;

domain::DeviceUuid deviceUuidOf(std::uint8_t id) {
    return domain::uuidFromBoardId(domain::BoardId{id, 0, 0, 0, 0, 0, 0, 0});
}

ConnectionActivationResponse acceptedActivation() {
    ConnectionActivationResponse response;
    response.serverLogicalAddress = kServerAddress;
    response.assignedRole = ConnectionRole::Control;
    response.responseCode = ActivationResponseCode::AcceptedRequestedRole;
    response.deviceUuid = deviceUuidOf(1);
    response.serverMaxPayload = 2048;
    response.capabilityMask = 0x01FFU;
    return response;
}

// Drives one ClientConnection against a scripted transport.
class Harness {
public:
    explicit Harness(ConnectionOptions options = testConnectionOptions())
        : options_(options), connection_(std::make_shared<ClientConnection>(options_, clock_, executor_)) {
        auto transport = std::make_unique<ScriptedTransport>(*connection_);
        scripted_ = transport.get();
        connection_->bindTransport(std::move(transport));
        connection_->setSetupHandler([this](ClientConnection &connection) { setups.push_back(connection.state()); });
        connection_->setStateCallback([this](const ClientConnection &connection) { states.push_back(connection.state()); });
        connection_->setErrorCallback([this](const ClientConnection &, const ClientError &error) { errors.push_back(error); });
        connection_->setReconnectHandler([this](ClientConnection &connection) {
            ++reconnects;
            // The real reconnect is performed by ConnectionManager, which binds
            // a fresh transport. Re-using the scripted transport keeps the
            // scripted device state (activation answers, events) intact.
            connection.reconnectNow();
        });
        connection_->setEventCallback([this](const EventDelivery &delivery) { events.push_back(delivery); });
        connection_->start();
    }

    // TCP established: the connection must immediately send the activation request.
    void acceptTcp() { scripted_->acceptConnect(); }

    void activate(ConnectionActivationResponse response = acceptedActivation()) {
        scripted_->deliverFrame(activationResponseFrame(response));
    }

    [[nodiscard]] ClientResult<RequestId> read(std::uint16_t did = 0xF001, bool ackRequired = false) {
        ServiceRequest request = readDidRequest(did);
        request.ackRequired = ackRequired;
        return connection_->sendRequest(request, [this](ClientResult<ServiceResponse> result) { completed.push_back(std::move(result)); });
    }

    void tick() { connection_->tick(clock_.nowUs()); }
    void advanceMs(std::uint64_t ms) { clock_.advanceMs(ms); }

    [[nodiscard]] std::shared_ptr<ClientConnection> &connection() noexcept { return connection_; }
    [[nodiscard]] ScriptedTransport &transport() noexcept { return *scripted_; }
    [[nodiscard]] ManualClientClock &clock() noexcept { return clock_; }

    std::vector<ClientResult<ServiceResponse>> completed;
    std::vector<ClientError> errors;
    std::vector<EventDelivery> events;
    std::vector<ConnectionState> states;
    std::vector<ConnectionState> setups;
    std::size_t reconnects = 0;

private:
    ConnectionOptions options_;
    ManualClientClock clock_;
    InlineExecutor executor_;
    std::shared_ptr<ClientConnection> connection_;
    ScriptedTransport *scripted_ = nullptr;
};

template <typename T>
ClientErrorCode codeOf(const ClientResult<T> &result) {
    return static_cast<ClientErrorCode>(result.error().code);
}

} // namespace

TEST_CASE("a new connection activates before any service is allowed", "[unit][client]") {
    Harness h;
    CHECK(h.connection()->state() == ConnectionState::Connecting);

    h.acceptTcp();
    CHECK(h.connection()->state() == ConnectionState::TcpConnected);

    auto activation = firstWriteOfType(h.transport(), PayloadType::ConnectionActivationRequest);
    REQUIRE(activation.has_value());
    auto decoded = decodeConnectionActivationRequest(activation->payloadBytes());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().clientLogicalAddress == h.connection()->clientLogicalAddress());
    CHECK(decoded.value().requestedRole == ConnectionRole::Control);

    // §18: no service request may be sent before activation succeeded.
    auto early = h.read();
    CHECK(early.failed());
    CHECK(static_cast<ClientErrorCode>(early.error().code) == ClientErrorCode::NotReady);
}

TEST_CASE("activation response completes the handshake and records device identity", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    CHECK(h.connection()->state() == ConnectionState::Ready);
    CHECK(h.connection()->isActive());
    CHECK(h.connection()->role() == ConnectionRole::Control);
    CHECK(h.connection()->serverLogicalAddress() == kServerAddress);
    CHECK(h.connection()->serverMaxPayload() == 2048U);
    CHECK(h.connection()->effectiveCapabilities() == 0x01FFU);
    CHECK(h.connection()->deviceUuid() == deviceUuidOf(1));
    CHECK(h.setups.size() == 1U);
    CHECK(h.errors.empty());
}

TEST_CASE("activation downgrade to Observer is still a usable connection", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    ConnectionActivationResponse response = acceptedActivation();
    response.assignedRole = ConnectionRole::Observer;
    response.responseCode = ActivationResponseCode::AcceptedDowngradedToObserver;
    h.activate(response);

    CHECK(h.connection()->state() == ConnectionState::Ready);
    CHECK(h.connection()->role() == ConnectionRole::Observer);
    CHECK(h.errors.empty());
}

TEST_CASE("activation rejection is reported and reconnecting is suppressed", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    ConnectionActivationResponse response = acceptedActivation();
    response.responseCode = ActivationResponseCode::RejectedRolePolicyDenied;
    response.assignedRole = ConnectionRole::None;
    h.activate(response);

    REQUIRE(h.errors.size() == 1);
    CHECK(static_cast<ClientErrorCode>(h.errors[0].code) == ClientErrorCode::ActivationRejected);
    CHECK(h.connection()->state() == ConnectionState::Disconnected);
    CHECK(h.connection()->isActive() == false);

    h.advanceMs(5000);
    h.tick();
    CHECK(h.reconnects == 0U); // §73: a policy rejection is not retried
    CHECK(h.connection()->state() == ConnectionState::Disconnected);
}

TEST_CASE("responses correlate by transaction id, never by arrival order", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto first = h.read(0xF001);
    auto second = h.read(0xF002);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(first.value() != second.value());
    CHECK(first.value() != 0U);
    CHECK(h.connection()->pendingCount() == 2U);

    // Answer the second request first.
    h.transport().deliverFrame(readDidResponseFrame(second.value(), 0xF002, ConstBytes{}));
    REQUIRE(h.completed.size() == 1);
    CHECK(h.completed[0].ok());
    CHECK(h.completed[0].value().transactionId == second.value());
    CHECK(h.completed[0].value().pduBytes()[1] == 0xF0U);

    h.transport().deliverFrame(readDidResponseFrame(first.value(), 0xF001, ConstBytes{}));
    REQUIRE(h.completed.size() == 2);
    CHECK(h.completed[1].value().transactionId == first.value());
    CHECK(h.connection()->pendingCount() == 0U);
    CHECK(h.connection()->diagnostics().responsesMatched == 2U);
}

TEST_CASE("a response for an unknown transaction id is counted, not delivered", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());
    h.transport().deliverFrame(readDidResponseFrame(request.value() + 7U, 0xF001, ConstBytes{}));

    CHECK(h.completed.empty());
    CHECK(h.connection()->pendingCount() == 1U);
    CHECK(h.connection()->diagnostics().lateResponses == 1U);
    CHECK(h.connection()->diagnostics().unmatchedResponses == 1U);
}

TEST_CASE("a negative response reports the raw NRC unchanged", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read(0xF01F);
    REQUIRE(request.ok());
    h.transport().deliverFrame(negativeResponseFrame(request.value(), 0x22, ServiceNrc::ConditionsNotCorrect));

    REQUIRE(h.completed.size() == 1);
    CHECK(h.completed[0].failed());
    CHECK(codeOf(h.completed[0]) == ClientErrorCode::ServiceRejected);
    REQUIRE(h.completed[0].error().nrc.has_value());
    CHECK(h.completed[0].error().nrc.value() == static_cast<std::uint8_t>(ServiceNrc::ConditionsNotCorrect));
    REQUIRE(h.completed[0].error().transactionId.has_value());
    CHECK(h.completed[0].error().transactionId.value() == request.value());
    CHECK(h.connection()->diagnostics().negativeResponses == 1U);
}

TEST_CASE("ResponsePending keeps the transaction outstanding instead of answering", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read(0xF01F);
    REQUIRE(request.ok());

    h.transport().deliverFrame(pendingResponseFrame(request.value(), 0x22));
    CHECK(h.completed.empty());
    CHECK(h.connection()->pendingCount() == 1U);

    // The pending interim re-arms the deadline: the request must not expire at
    // the original deadline.
    h.advanceMs(900);
    h.tick();
    CHECK(h.connection()->pendingCount() == 1U);

    h.transport().deliverFrame(readDidResponseFrame(request.value(), 0xF01F, ConstBytes{}));
    REQUIRE(h.completed.size() == 1);
    CHECK(h.completed[0].ok());
}

TEST_CASE("outstanding requests expire and report a timeout", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());
    CHECK(h.connection()->isBusy());

    h.advanceMs(1100); // requestTimeoutMs is 1000 in the test options
    h.tick();

    REQUIRE(h.completed.size() == 1);
    CHECK(codeOf(h.completed[0]) == ClientErrorCode::Timeout);
    CHECK(h.connection()->pendingCount() == 0U);
    CHECK(h.connection()->diagnostics().requestTimeouts == 1U);
    CHECK_FALSE(h.connection()->isBusy());
}

TEST_CASE("cancel completes the request and a late answer is only counted", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());
    h.connection()->cancelRequest(request.value());

    REQUIRE(h.completed.size() == 1);
    CHECK(codeOf(h.completed[0]) == ClientErrorCode::Cancelled);
    CHECK(h.connection()->pendingCount() == 0U);

    h.transport().deliverFrame(readDidResponseFrame(request.value(), 0xF001, ConstBytes{}));
    CHECK(h.completed.size() == 1U); // still no second delivery
    CHECK(h.connection()->diagnostics().lateResponses == 1U);
}

TEST_CASE("the outstanding-request bound rejects new requests instead of growing", "[unit][client]") {
    ConnectionOptions options = testConnectionOptions();
    options.maxOutstandingRequests = 2;
    Harness h(options);
    h.acceptTcp();
    h.activate();

    REQUIRE(h.read().ok());
    REQUIRE(h.read().ok());

    auto rejected = h.read();
    CHECK(rejected.failed());
    CHECK(static_cast<ClientErrorCode>(rejected.error().code) == ClientErrorCode::TooManyOutstanding);
    CHECK(h.connection()->pendingCount() == 2U);

    // Transaction ids advance monotonically; a freed id becomes available again
    // only when the allocator wraps (§20.1).
    h.transport().deliverFrame(readDidResponseFrame(1U, 0xF001, ConstBytes{}));
    auto reused = h.read();
    REQUIRE(reused.ok());
    CHECK(reused.value() == 3U);
    CHECK(h.connection()->pendingCount() == 2U);
}

TEST_CASE("ack-required requests stay open until the application ack arrives", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read(0xF001, true);
    REQUIRE(request.ok());
    CHECK(h.connection()->pendingCount() == 1U);

    h.advanceMs(1100);
    h.tick();
    // Without an ack the request must not be answered by a plain timeout alone.
    REQUIRE_FALSE(h.completed.empty());
    CHECK(codeOf(h.completed.back()) == ClientErrorCode::Timeout);

    auto written = firstWriteOfType(h.transport(), PayloadType::ApplicationMessage);
    REQUIRE(written.has_value());
    auto envelope = decodeApplicationEnvelope(written->payloadBytes());
    REQUIRE(envelope.ok());
    CHECK(envelope.value().ackRequired());
}

TEST_CASE("an idle connection performs an alive check and accepts the matching nonce", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    h.advanceMs(1100); // aliveCheckIdleMs is 1000
    h.tick();

    auto probe = firstWriteOfType(h.transport(), PayloadType::AliveCheckRequest);
    REQUIRE(probe.has_value());
    auto request = decodeAliveCheckRequest(probe->payloadBytes());
    REQUIRE(request.ok());

    h.transport().deliverFrame(aliveCheckResponseFrame(request.value().nonce));
    CHECK(h.connection()->state() == ConnectionState::Ready);
    CHECK(h.connection()->diagnostics().unmatchedResponses == 0U);
    CHECK(h.errors.empty());
}

TEST_CASE("a wrong alive-check nonce is treated as an unmatched response", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    h.advanceMs(1100);
    h.tick();
    h.transport().deliverFrame(aliveCheckResponseFrame(9999ULL));
    CHECK(h.connection()->diagnostics().unmatchedResponses == 1U);
    CHECK(h.connection()->state() == ConnectionState::Ready);
}

TEST_CASE("an unanswered alive check closes the connection", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    h.advanceMs(1100);
    h.tick(); // probe sent
    h.advanceMs(1100);
    h.tick(); // probe deadline exceeded

    CHECK(h.connection()->diagnostics().requestTimeouts == 1U);
    CHECK(h.connection()->state() == ConnectionState::ReconnectWait);
    REQUIRE_FALSE(h.errors.empty());
    CHECK(static_cast<ClientErrorCode>(h.errors.back().code) == ClientErrorCode::Timeout);
}

TEST_CASE("a server-initiated alive check is echoed with the same nonce", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    AliveCheckRequest probe;
    probe.nonce = 4242;
    const ByteBuffer body = encodeAliveCheckRequest(probe);
    h.transport().deliverFrame(frameOf(PayloadType::AliveCheckRequest, bytesOf(body)));

    auto echo = firstWriteOfType(h.transport(), PayloadType::AliveCheckResponse);
    REQUIRE(echo.has_value());
    auto answer = decodeAliveCheckResponse(echo->payloadBytes());
    REQUIRE(answer.ok());
    CHECK(answer.value().nonce == 4242ULL);
}

TEST_CASE("event notifications are delivered with the owning device", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    EventNotification event;
    event.eventId = static_cast<std::uint16_t>(EventId::UwbMeasurement);
    event.streamId = 1;
    event.sequence = 7;
    event.timestampUs = 123456;
    event.payload = encodeMeasurementEvent(MeasurementEvent{});

    h.transport().deliverFrame(eventNotificationFrame(event));

    REQUIRE(h.events.size() == 1);
    CHECK(h.events[0].device == deviceUuidOf(1));
    CHECK(h.events[0].event.sequence == 7U);
    CHECK(h.events[0].event.eventId == static_cast<std::uint16_t>(EventId::UwbMeasurement));
    CHECK(h.connection()->diagnostics().eventNotifications == 1U);

    // Unsolicited notifications must not create pending transactions.
    CHECK(h.connection()->pendingCount() == 0U);
}

TEST_CASE("a measurement event is decoded into the domain measurement", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    MeasurementEvent raw;
    raw.rawRangeMm = 1234;
    raw.anchorId = 2;
    raw.tagId = 1;
    raw.flags = static_cast<std::uint16_t>(MeasurementFlag::RawRangeValid);

    EventNotification event;
    event.eventId = static_cast<std::uint16_t>(EventId::UwbMeasurement);
    event.payload = encodeMeasurementEvent(raw);
    h.transport().deliverFrame(eventNotificationFrame(event));

    REQUIRE(h.events.size() == 1);
    auto decoded = decodeMeasurementEvent(h.events[0].event.payloadBytes());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().rawRangeMm == 1234U);
    CHECK(decoded.value().anchorId == 2U);
}

TEST_CASE("unknown payload types on a TCP connection invalidate the stream", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    const ByteBuffer bogus = frameOf(PayloadType::DeviceIdRequest, ConstBytes{});
    h.transport().deliverFrame(bogus);

    CHECK(h.connection()->diagnostics().protocolErrors == 1U);
    REQUIRE_FALSE(h.errors.empty());
    CHECK(static_cast<ClientErrorCode>(h.errors.back().code) == ClientErrorCode::ProtocolViolation);
    CHECK(h.connection()->state() == ConnectionState::ReconnectWait);
    CHECK_FALSE(h.connection()->isActive());
}

TEST_CASE("a transport-level GenericHeaderNack is surfaced and closes the connection", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    GenericHeaderNack nack;
    nack.code = GenericHeaderNackCode::MessageTooLarge;
    const ByteBuffer body = encodeGenericHeaderNack(nack);
    h.transport().deliverFrame(frameOf(PayloadType::GenericHeaderNack, bytesOf(body)));

    CHECK(h.connection()->diagnostics().transportNacks == 1U);
    REQUIRE_FALSE(h.errors.empty());
    CHECK(static_cast<ClientErrorCode>(h.errors.back().code) == ClientErrorCode::ProtocolViolation);
    CHECK(h.connection()->state() == ConnectionState::ReconnectWait);
}

TEST_CASE("an application NACK fails the matching transaction", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());

    ApplicationMessageNack nack;
    nack.transactionId = request.value();
    nack.code = ApplicationNackCode::ConnectionNotActivated;
    const ByteBuffer body = encodeApplicationMessageNack(nack);
    h.transport().deliverFrame(frameOf(PayloadType::ApplicationMessageNack, bytesOf(body)));

    REQUIRE(h.completed.size() == 1);
    CHECK(h.completed[0].failed());
    CHECK(h.connection()->diagnostics().transportNacks == 1U);
    CHECK(h.connection()->pendingCount() == 0U);
}

TEST_CASE("frames split across socket reads are reassembled", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());
    const ByteBuffer frame = readDidResponseFrame(request.value(), 0xF001, ConstBytes{});

    ConstBytes all{frame.data(), frame.size()};
    h.transport().deliver(ConstBytes{all.data(), 5});           // partial header
    CHECK(h.completed.empty());
    h.transport().deliver(ConstBytes{all.data() + 5, all.size() - 5}); // rest of the frame

    REQUIRE(h.completed.size() == 1);
    CHECK(h.completed[0].ok());
    CHECK(h.connection()->diagnostics().protocolErrors == 0U);
}

TEST_CASE("a partial frame stays buffered until the rest of it arrives", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    ByteBuffer frame = readDidResponseFrame(1U, 0xF001, ConstBytes{});
    ConstBytes partial{frame.data(), frame.size() - 1};
    h.transport().deliver(partial);

    CHECK(h.completed.empty());
    CHECK(h.connection()->diagnostics().protocolErrors == 0U);
    CHECK(h.connection()->state() == ConnectionState::Ready); // not a protocol violation
}

TEST_CASE("an invalid generic header is a framing error that invalidates the connection", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    // Correct length, but protocol version and inverse byte do not match (§13).
    const ByteBuffer garbage{0x20, 0xEF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    h.transport().deliver(ConstBytes{garbage.data(), garbage.size()});

    CHECK(h.connection()->diagnostics().protocolErrors == 1U);
    REQUIRE_FALSE(h.errors.empty());
    CHECK(static_cast<ClientErrorCode>(h.errors.back().code) == ClientErrorCode::FramingError);
    CHECK(h.connection()->state() == ConnectionState::ReconnectWait);
}

TEST_CASE("a stalled transport overflows the transmit queue by dropping the oldest frame", "[unit][client]") {
    ConnectionOptions options = testConnectionOptions();
    options.maxTxQueueFrames = 2;
    Harness h(options);

    // The handshake has to succeed, so open the transport only afterwards.
    h.acceptTcp();
    h.activate();
    std::size_t framesSoFar = h.transport().writes().size();
    h.transport().pauseWrites(); // simulates a transport that cannot take more data

    for (int attempt = 0; attempt < 4; ++attempt) {
        static_cast<void>(h.read());
        h.tick();
    }

    CHECK(h.connection()->diagnostics().txQueueDrops == 2U);
    CHECK(h.connection()->pendingCount() == 4U);

    h.transport().resumeWrites();
    h.tick();
    // Only the two newest frames survived the overflow.
    CHECK(h.transport().writes().size() == framesSoFar + 2U);
}

TEST_CASE("an unexpected peer close schedules a reconnect with backoff", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());

    h.transport().drop(TransportStatus::Closed, "peer closed");

    CHECK(h.connection()->state() == ConnectionState::ReconnectWait);
    CHECK(h.connection()->pendingCount() == 0U);
    REQUIRE(h.completed.size() == 1);
    CHECK(codeOf(h.completed[0]) == ClientErrorCode::Disconnected);
    CHECK(h.connection()->reconnectDelayUs() == 100ULL * 1000ULL); // reconnectInitialDelayMs

    h.advanceMs(50);
    h.tick();
    CHECK(h.reconnects == 0U); // backoff not elapsed yet

    h.advanceMs(60);
    h.tick();
    CHECK(h.reconnects == 1U);
    CHECK(h.connection()->diagnostics().reconnectAttempts == 1U);
}

TEST_CASE("reconnect backoff doubles and saturates at the configured maximum", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    std::vector<std::uint64_t> delays;
    for (int attempt = 0; attempt < 6; ++attempt) {
        h.transport().drop(TransportStatus::Closed, "peer closed");
        delays.push_back(h.connection()->reconnectDelayUs());
        h.advanceMs(2000);
        h.tick();       // performs the reconnect attempt
        h.acceptTcp();  // a fresh transport is bound by the reconnect handler in real life
        h.activate();
    }

    CHECK(delays[0] == 100ULL * 1000ULL);
    CHECK(delays[1] == 200ULL * 1000ULL);
    CHECK(delays[2] == 400ULL * 1000ULL);
    CHECK(delays[3] == 800ULL * 1000ULL);
    CHECK(delays[4] == 1000ULL * 1000ULL); // reconnectMaxDelayMs ceiling
    CHECK(delays[5] == 1000ULL * 1000ULL);
}

TEST_CASE("a manual disconnect does not reconnect", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    h.connection()->close();
    CHECK(h.connection()->state() == ConnectionState::Disconnected);

    h.advanceMs(10000);
    h.tick();
    CHECK(h.reconnects == 0U);
    CHECK(h.transport().isOpen() == false);
}

TEST_CASE("auto-reconnect can be disabled per connection", "[unit][client]") {
    ConnectionOptions options = testConnectionOptions();
    options.autoReconnect = false;
    Harness h(options);
    h.acceptTcp();
    h.activate();

    h.transport().drop(TransportStatus::Closed, "peer closed");
    CHECK(h.connection()->state() == ConnectionState::Disconnected);

    h.advanceMs(10000);
    h.tick();
    CHECK(h.reconnects == 0U);
}

TEST_CASE("an extended session keeps the session alive with ClientPresent", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();
    h.connection()->noteSession(SessionId::Extended);
    h.connection()->markReady();

    h.tick();
    auto first = firstWriteOfType(h.transport(), PayloadType::ApplicationMessage);
    REQUIRE(first.has_value());
    auto envelope = decodeApplicationEnvelope(first->payloadBytes());
    REQUIRE(envelope.ok());
    CHECK(envelope.value().servicePdu[0] == static_cast<std::uint8_t>(ServiceId::ClientPresent));

    h.advanceMs(2100); // clientPresentIntervalMs is 2000
    h.tick();
    std::size_t presents = 0;
    for (const ByteBuffer &written : h.transport().writes()) {
        FrameParser parser;
        static_cast<void>(parser.push(ConstBytes{written.data(), written.size()}));
        for (;;) {
            Result<Frame> frame = parser.popFrame();
            if (!frame.ok()) {
                break;
            }
            if (frame.value().type() != PayloadType::ApplicationMessage) {
                continue;
            }
            auto decoded = decodeApplicationEnvelope(frame.value().payloadBytes());
            if (decoded.ok() && decoded.value().servicePdu[0] == static_cast<std::uint8_t>(ServiceId::ClientPresent)) {
                ++presents;
            }
        }
    }
    CHECK(presents == 2U);
}

TEST_CASE("stream subscriptions are tracked so that the controller can restore them", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    StreamSubscription subscription;
    subscription.streamId = 3;
    subscription.eventId = static_cast<std::uint16_t>(EventId::UwbMeasurement);
    subscription.mode = static_cast<std::uint8_t>(StreamMode::Live);

    h.connection()->addSubscription(subscription);
    h.connection()->addSubscription(subscription); // idempotent: same stream id
    REQUIRE(h.connection()->subscriptions().size() == 1);
    CHECK(h.connection()->subscriptions()[0].streamId == 3U);

    subscription.droppedCount = 5;
    subscription.state = StreamState::Failed;
    h.connection()->addSubscription(subscription);
    REQUIRE(h.connection()->subscriptions().size() == 1);
    CHECK(h.connection()->subscriptions()[0].droppedCount == 5U);
    CHECK(h.connection()->subscriptions()[0].state == StreamState::Failed);

    h.connection()->removeSubscription(3);
    CHECK(h.connection()->subscriptions().empty());

    h.connection()->addSubscription(subscription);
    h.connection()->clearSubscriptions();
    CHECK(h.connection()->subscriptions().empty());
}

TEST_CASE("connection diagnostics expose the counters used by the CLI status line", "[unit][client]") {
    Harness h;
    h.acceptTcp();
    h.activate();

    auto request = h.read();
    REQUIRE(request.ok());
    h.transport().deliverFrame(readDidResponseFrame(request.value(), 0xF001, ConstBytes{}));

    const ConnectionDiagnostics &diagnostics = h.connection()->diagnostics();
    CHECK(diagnostics.framesSent >= 2U); // activation request + service request
    CHECK(diagnostics.framesReceived >= 2U);
    CHECK(diagnostics.requestsSent == 1U);
    CHECK(diagnostics.responsesMatched == 1U);
    CHECK(diagnostics.negativeResponses == 0U);
    CHECK(diagnostics.requestTimeouts == 0U);
    CHECK(diagnostics.protocolErrors == 0U);
    CHECK(diagnostics.lastActivityUs >= 1ULL);
}
