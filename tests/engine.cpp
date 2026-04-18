#include "engine.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace scribe {
namespace {

ConnectionConfig testConnectionConfig() {
  return ConnectionConfig{
      .socketPath = "/tmp/scribe.sock",
      .authToken = "secret-token",
      .connectTimeout = std::chrono::milliseconds(250),
  };
}

SessionId testSessionId(uint8_t seed) {
  SessionId value{};
  for (size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<uint8_t>(seed + i);
  }
  return value;
}

template <typename T, typename Predicate>
std::optional<T> findVariantIf(const std::vector<Effect>& effects,
                               Predicate&& predicate) {
  for (const auto& effect : effects) {
    if (const auto* value = std::get_if<T>(&effect)) {
      if (std::forward<Predicate>(predicate)(*value)) {
        return *value;
      }
    }
  }
  return std::nullopt;
}

template <typename T>
std::optional<T> findEffect(const std::vector<Effect>& effects) {
  return findVariantIf<T>(effects, [](const T&) { return true; });
}

std::optional<Log> findLog(const std::vector<Effect>& effects,
                           LogLevel level = LogLevel::kInfo) {
  return findVariantIf<Log>(
      effects, [level](const Log& value) { return value.level == level; });
}

std::optional<Log> findLogContaining(const std::vector<Effect>& effects,
                                     std::string_view needle,
                                     LogLevel level = LogLevel::kInfo) {
  return findVariantIf<Log>(effects, [&](const Log& value) {
    return value.level == level &&
           value.message.find(needle) != std::string::npos;
  });
}

}  // namespace

TEST_CASE("toggle starts connection and accepts hotkey", "[engine]") {
  Engine engine([]() { return Result<SessionId>(testSessionId(0x10)); });

  auto configure = engine.handle(ConnectionConfigured{
      .result = testConnectionConfig(),
  });
  REQUIRE(configure.empty());

  auto effects = engine.handle(Toggle{
      .context = 7,
      .focused = true,
  });

  REQUIRE(findEffect<AcceptKey>(effects).has_value());
  REQUIRE(findEffect<Connect>(effects).has_value());
  REQUIRE(engine.connectionState() == ConnectionState::kConnecting);
  REQUIRE(engine.transcribing());
}

TEST_CASE("toggle from unfocused context is ignored", "[engine]") {
  Engine engine([]() { return Result<SessionId>(testSessionId(0x20)); });

  auto configure = engine.handle(ConnectionConfigured{
      .result = testConnectionConfig(),
  });
  REQUIRE(configure.empty());

  auto effects = engine.handle(Toggle{
      .context = 7,
      .focused = false,
  });

  REQUIRE(effects.empty());
  REQUIRE(engine.connectionState() == ConnectionState::kDisconnected);
  REQUIRE_FALSE(engine.transcribing());
}

TEST_CASE(
    "authenticated connection promotes pending context into session start",
    "[engine]") {
  const auto sessionId = testSessionId(0x30);
  const auto connection = testConnectionConfig();

  Engine engine([sessionId]() { return Result<SessionId>(sessionId); });

  engine.handle(ConnectionConfigured{
      .result = connection,
  });
  engine.handle(FocusIn{.context = 11});

  auto toggleEffects = engine.handle(Toggle{
      .context = 11,
      .focused = true,
  });
  REQUIRE(findEffect<Connect>(toggleEffects).has_value());

  auto readyEffects = engine.handle(ConnectionAuthenticated{});

  const auto start = findEffect<SendEnvelope>(readyEffects);
  const auto log = findLog(readyEffects);

  REQUIRE(start.has_value());
  REQUIRE(start->envelope.has_start_session());
  REQUIRE(start->envelope.start_session().session_id() ==
          std::string(reinterpret_cast<const char*>(sessionId.data()),
                      sessionId.size()));
  REQUIRE(log.has_value());
  REQUIRE(log->message.find("Transcriber connection ready") !=
          std::string::npos);
  REQUIRE(engine.connectionState() == ConnectionState::kReady);
  REQUIRE(engine.transcribing());
}

TEST_CASE("commit text advances sequence and emits commit effect", "[engine]") {
  const auto sessionId = testSessionId(0x60);
  const auto connection = testConnectionConfig();

  Engine engine([sessionId]() { return Result<SessionId>(sessionId); });

  engine.handle(ConnectionConfigured{
      .result = connection,
  });
  engine.handle(FocusIn{.context = 3});
  engine.handle(Toggle{.context = 3, .focused = true});
  engine.handle(ConnectionAuthenticated{});

  v0::CommitText commit;
  commit.set_session_id(sessionId.data(), sessionId.size());
  commit.set_sequence_no(1);
  commit.set_text("hello");

  auto commitEffects =
      engine.handle(ConnectionCommitText{.commitText = commit});
  const auto commitEffect = findEffect<CommitText>(commitEffects);

  REQUIRE(commitEffect.has_value());
  REQUIRE(commitEffect->context == 3);
  REQUIRE(commitEffect->text == "hello");
  REQUIRE(engine.transcribing());
}

TEST_CASE("sequence mismatch interrupts active session", "[engine]") {
  const auto sessionId = testSessionId(0x90);
  const auto connection = testConnectionConfig();

  Engine engine([sessionId]() { return Result<SessionId>(sessionId); });

  engine.handle(ConnectionConfigured{
      .result = connection,
  });
  engine.handle(FocusIn{.context = 5});
  engine.handle(Toggle{.context = 5, .focused = true});
  engine.handle(ConnectionAuthenticated{});

  v0::CommitText badCommit;
  badCommit.set_session_id(sessionId.data(), sessionId.size());
  badCommit.set_sequence_no(2);
  badCommit.set_text("bad");

  auto effects = engine.handle(ConnectionCommitText{.commitText = badCommit});
  const auto interrupt = findEffect<SendEnvelope>(effects);
  const auto log = findLogContaining(effects, "commit sequence mismatch");

  REQUIRE(interrupt.has_value());
  REQUIRE(interrupt->envelope.has_interrupt_session());
  REQUIRE(interrupt->envelope.interrupt_session().reason() ==
          v0::INTERRUPT_REASON_TRANSPORT_FAILURE);
  REQUIRE(log.has_value());
  REQUIRE_FALSE(engine.transcribing());
}

TEST_CASE("connection failure clears pending startup", "[engine]") {
  const auto connection = testConnectionConfig();

  Engine engine([]() { return Result<SessionId>(testSessionId(0xD0)); });

  engine.handle(ConnectionConfigured{
      .result = connection,
  });
  engine.handle(FocusIn{.context = 9});
  engine.handle(Toggle{.context = 9, .focused = true});

  auto failureEffects = engine.handle(ConnectionFailed{
      .error =
          makeError(ErrorKind::kTransport, "transcriber handshake timeout"),
  });

  REQUIRE(findLog(failureEffects).has_value());
  REQUIRE(engine.connectionState() == ConnectionState::kDisconnected);
  REQUIRE_FALSE(engine.transcribing());
}

}  // namespace scribe
