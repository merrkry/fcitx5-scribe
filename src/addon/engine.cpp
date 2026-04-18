#include "engine.hpp"

#include <algorithm>
#include <utility>

#include "util.hpp"

namespace scribe {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

bool bytesEqual(std::string_view input, const SessionId& sessionId) {
  return input.size() == sessionId.size() &&
         std::equal(input.begin(), input.end(), sessionId.begin(),
                    [](char lhs, uint8_t rhs) {
                      return static_cast<uint8_t>(lhs) == rhs;
                    });
}

}  // namespace

Engine::Engine(SessionIdGenerator sessionIdGenerator)
    : sessionIdGenerator_(std::move(sessionIdGenerator)) {}

std::vector<Effect> Engine::handle(const Event& event) {
  std::vector<Effect> effects;
  std::visit(Overloaded{
                 [this, &effects](const ConnectionConfigured& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const Toggle& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const FocusIn& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const FocusOut& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ContextDestroyed& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ConnectionAuthenticated& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ConnectionClosed& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ConnectionFailed& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ConnectionCommitText& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ConnectionVadTimeout& value) {
                   handleEvent(value, effects);
                 },
                 [this, &effects](const ConnectionErrorMessage& value) {
                   handleEvent(value, effects);
                 },
             },
             event);
  return effects;
}

bool Engine::transcribing() const noexcept {
  return pendingContext_.has_value() || session_.has_value();
}

ConnectionState Engine::connectionState() const noexcept {
  return connectionState_;
}

// Connection reconfiguration invalidates any in-flight session because the
// authentication secret, socket path, or timeout budget may have changed.
void Engine::handleEvent(const ConnectionConfigured& event,
                         std::vector<Effect>& effects) {
  const bool hadActivity = connectionState_ != ConnectionState::kDisconnected ||
                           pendingContext_.has_value() || session_.has_value();
  if (hadActivity) {
    abortInteraction(effects, "connection configuration changed",
                     AbortKind::kQuiet);
    effects.emplace_back(Disconnect{});
  }

  connectionConfig_.reset();
  connectionConfigError_.reset();

  if (const auto* connection = std::get_if<ConnectionConfig>(&event.result);
      connection != nullptr) {
    connectionConfig_ = *connection;
    return;
  }

  if (const auto* error = std::get_if<Error>(&event.result); error != nullptr) {
    connectionConfigError_ = *error;
  }
}

// Toggle is the user entry point for starting or interrupting transcription in
// the currently focused input context.
void Engine::handleEvent(const Toggle& event, std::vector<Effect>& effects) {
  auto& context = ensureContext(event.context);
  context.alive = true;
  if (event.focused) {
    setFocusedContext(event.context);
  } else if (focusedContext_ == event.context) {
    setFocusedContext(std::nullopt);
  }

  if (session_) {
    interruptSession(v0::INTERRUPT_REASON_TOGGLE, "toggle", effects);
    effects.emplace_back(AcceptKey{});
    return;
  }

  if (pendingContext_ == event.context) {
    clearPending();
    effects.emplace_back(AcceptKey{});
    return;
  }

  if (!event.focused) {
    return;
  }

  if (!connectionConfig_) {
    if (connectionConfigError_) {
      pushLog(effects, LogLevel::kInfo, connectionConfigError_->message);
    } else {
      pushLog(effects, LogLevel::kInfo,
              "Cannot start transcription: connection is not configured");
    }
    return;
  }

  pendingContext_ = event.context;
  effects.emplace_back(AcceptKey{});

  switch (connectionState_) {
    case ConnectionState::kDisconnected:
      connectionState_ = ConnectionState::kConnecting;
      effects.emplace_back(Connect{});
      break;
    case ConnectionState::kConnecting:
      break;
    case ConnectionState::kReady:
      // TODO: If the backend died while this connection was idle, the first
      // StartSession send will currently fail and force the user to press the
      // toggle again after the reconnect. Preserve that pending user intent and
      // retry once automatically on a fresh connection.
      tryStartPending(effects);
      break;
  }
}

void Engine::handleEvent(const FocusIn& event, std::vector<Effect>& effects) {
  auto& context = ensureContext(event.context);
  context.alive = true;
  setFocusedContext(event.context);

  if (pendingContext_ && *pendingContext_ != event.context) {
    clearPending();
  }

  if (session_ && session_->context != event.context) {
    interruptSession(v0::INTERRUPT_REASON_FOCUS_CHANGED, "focus changed",
                     effects);
  }
}

void Engine::handleEvent(const FocusOut& event, std::vector<Effect>& effects) {
  auto& context = ensureContext(event.context);
  context.alive = true;
  if (focusedContext_ == event.context) {
    setFocusedContext(std::nullopt);
  }

  if (pendingContext_ == event.context) {
    clearPending();
  }

  if (session_ && session_->context == event.context) {
    interruptSession(v0::INTERRUPT_REASON_FOCUS_LOST, "focus lost", effects);
  }
}

void Engine::handleEvent(const ContextDestroyed& event,
                         std::vector<Effect>& effects) {
  auto& context = ensureContext(event.context);
  context.alive = false;
  if (focusedContext_ == event.context) {
    setFocusedContext(std::nullopt);
  }

  if (pendingContext_ == event.context) {
    clearPending();
  }

  if (session_ && session_->context == event.context) {
    interruptSession(v0::INTERRUPT_REASON_INPUT_CONTEXT_DESTROYED,
                     "input context destroyed", effects);
  }
}

void Engine::handleEvent(const ConnectionAuthenticated&,
                         std::vector<Effect>& effects) {
  if (connectionState_ != ConnectionState::kConnecting) {
    effects.emplace_back(Disconnect{});
    return;
  }

  connectionState_ = ConnectionState::kReady;
  pushLog(effects, LogLevel::kInfo, "Transcriber connection ready");
  tryStartPending(effects);
}

void Engine::handleEvent(const ConnectionClosed&,
                         std::vector<Effect>& effects) {
  const bool activeInteraction =
      pendingContext_.has_value() || session_.has_value();
  if (!activeInteraction && connectionState_ == ConnectionState::kReady) {
    resetConnection(effects);
    pushLog(effects, LogLevel::kDebug, "Transcriber connection closed");
    return;
  }

  if (connectionState_ == ConnectionState::kDisconnected &&
      !activeInteraction) {
    return;
  }

  abortInteraction(effects, "transport disconnected",
                   AbortKind::kConnectionFailed);
}

void Engine::handleEvent(const ConnectionFailed& event,
                         std::vector<Effect>& effects) {
  pushLog(effects, LogLevel::kInfo,
          "Transport failure (" + std::string(errorKindName(event.error.kind)) +
              "): " + event.error.message);
  abortInteraction(effects, "transport failure", AbortKind::kConnectionFailed);
}

void Engine::handleEvent(const ConnectionCommitText& event,
                         std::vector<Effect>& effects) {
  handleCommitText(event.commitText, effects);
}

void Engine::handleEvent(const ConnectionVadTimeout& event,
                         std::vector<Effect>& effects) {
  handleVadTimeout(event.vadTimeout, effects);
}

void Engine::handleEvent(const ConnectionErrorMessage& event,
                         std::vector<Effect>& effects) {
  handleErrorEnvelope(event.error, effects);
}

Engine::ContextState& Engine::ensureContext(ContextId context) {
  return contexts_[context];
}

bool Engine::contextCanCommit(ContextId context) const {
  if (const auto iter = contexts_.find(context); iter != contexts_.end()) {
    return iter->second.alive && contextFocused(context);
  }
  return false;
}

bool Engine::contextFocused(ContextId context) const noexcept {
  return focusedContext_ == context;
}

void Engine::setFocusedContext(std::optional<ContextId> context) noexcept {
  focusedContext_ = context;
}

void Engine::clearPending() noexcept { pendingContext_.reset(); }

void Engine::resetConnection(std::vector<Effect>&) {
  connectionState_ = ConnectionState::kDisconnected;
}

void Engine::abortInteraction(std::vector<Effect>& effects,
                              std::string_view reason, AbortKind kind) {
  resetConnection(effects);
  clearPending();
  session_.reset();
  if (kind == AbortKind::kConnectionFailed) {
    pushLog(effects, LogLevel::kInfo,
            "Transcription interrupted (" + std::string(reason) + ')');
  }
}

// Interrupt messages are best-effort: if the connection is already gone the
// engine still clears local session state and only skips the outbound envelope.
void Engine::interruptSession(v0::InterruptReason reason,
                              std::string_view logReason,
                              std::vector<Effect>& effects) {
  if (!session_) {
    clearPending();
    return;
  }

  if (connectionState_ == ConnectionState::kReady) {
    v0::Envelope envelope;
    auto* interrupt = envelope.mutable_interrupt_session();
    interrupt->set_session_id(session_->id.data(), session_->id.size());
    interrupt->set_reason(reason);
    effects.emplace_back(SendEnvelope{std::move(envelope)});
  }

  session_.reset();
  clearPending();
  pushLog(effects, LogLevel::kInfo,
          "Transcription interrupted (" + std::string(logReason) + ')');
}

// A pending context becomes an active session only after IO reports an
// authenticated connection and the target context is still alive and focused.
void Engine::tryStartPending(std::vector<Effect>& effects) {
  if (!pendingContext_ || connectionState_ != ConnectionState::kReady) {
    return;
  }

  if (!contextCanCommit(*pendingContext_)) {
    clearPending();
    return;
  }

  auto sessionId = sessionIdGenerator_();
  if (!sessionId) {
    pushLog(effects, LogLevel::kInfo,
            "Cannot start transcription: " + sessionId.error().message);
    clearPending();
    return;
  }

  session_ = SessionState{
      .context = *pendingContext_,
      .id = *sessionId,
      .nextSequenceNo = 1,
  };
  clearPending();

  v0::Envelope envelope;
  envelope.mutable_start_session()->set_session_id(session_->id.data(),
                                                   session_->id.size());
  effects.emplace_back(SendEnvelope{std::move(envelope)});
  pushLog(effects, LogLevel::kInfo,
          "Transcription started for session " +
              sessionIdToLogString(session_->id));
}

// CommitText is ordered per session. A sequence mismatch is treated as a
// transport/protocol failure because accepting out-of-order text risks
// keystroke injection into the wrong logical session.
void Engine::handleCommitText(const v0::CommitText& commitText,
                              std::vector<Effect>& effects) {
  if (!session_ || !bytesEqual(commitText.session_id(), session_->id)) {
    pushLog(effects, LogLevel::kDebug,
            "Ignoring CommitText for inactive session");
    return;
  }

  if (commitText.sequence_no() != session_->nextSequenceNo) {
    pushLog(effects, LogLevel::kInfo,
            "Interrupting session after unexpected CommitText sequence " +
                std::to_string(commitText.sequence_no()) + ", expected " +
                std::to_string(session_->nextSequenceNo));
    interruptSession(v0::INTERRUPT_REASON_TRANSPORT_FAILURE,
                     "commit sequence mismatch", effects);
    return;
  }

  const auto iter = contexts_.find(session_->context);
  if (iter == contexts_.end() || !iter->second.alive) {
    interruptSession(v0::INTERRUPT_REASON_INPUT_CONTEXT_DESTROYED,
                     "input context destroyed before commit", effects);
    return;
  }
  if (!contextFocused(session_->context)) {
    interruptSession(v0::INTERRUPT_REASON_FOCUS_LOST,
                     "input context lost focus before commit", effects);
    return;
  }

  effects.emplace_back(
      CommitText{.context = session_->context, .text = commitText.text()});
  ++session_->nextSequenceNo;
}

void Engine::handleVadTimeout(const v0::VadTimeout& timeout,
                              std::vector<Effect>& effects) {
  if (!session_ || !bytesEqual(timeout.session_id(), session_->id)) {
    pushLog(effects, LogLevel::kDebug,
            "Ignoring VadTimeout for inactive session");
    return;
  }

  pushLog(effects, LogLevel::kInfo,
          "Session " + sessionIdToLogString(session_->id) +
              " ended with VAD timeout");
  session_.reset();
}

void Engine::handleErrorEnvelope(const v0::Error& error,
                                 std::vector<Effect>& effects) {
  if (!session_ || !bytesEqual(error.session_id(), session_->id)) {
    pushLog(effects, LogLevel::kDebug, "Ignoring Error for inactive session");
    return;
  }

  pushLog(effects, LogLevel::kInfo,
          "Session " + sessionIdToLogString(session_->id) +
              " failed with transcriber error " + std::to_string(error.code()) +
              ": " + error.message());
  session_.reset();
}

void Engine::pushLog(std::vector<Effect>& effects, LogLevel level,
                     std::string message) const {
  effects.emplace_back(Log{.level = level, .message = std::move(message)});
}

}  // namespace scribe
