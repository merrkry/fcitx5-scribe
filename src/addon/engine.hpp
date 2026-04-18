#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "error.hpp"
#include "protocol.hpp"
#include "types.hpp"

namespace scribe {

enum class LogLevel {
  kDebug,
  kInfo,
};

enum class ConnectionState {
  kDisconnected,
  kConnecting,
  kReady,
};

struct ConnectionConfigured {
  // Empty result means no configuration has been loaded yet. A valid
  // ConnectionConfig enables Connect effects, while Error records why the
  // connection cannot currently be opened.
  std::variant<std::monostate, ConnectionConfig, Error> result;
};

struct Toggle {
  ContextId context;
  bool focused;
};

struct FocusIn {
  ContextId context;
};

struct FocusOut {
  ContextId context;
};

struct ContextDestroyed {
  ContextId context;
};

struct ConnectionAuthenticated {};

struct ConnectionClosed {};

struct ConnectionFailed {
  Error error;
};

struct ConnectionCommitText {
  v0::CommitText commitText;
};

struct ConnectionVadTimeout {
  v0::VadTimeout vadTimeout;
};

struct ConnectionErrorMessage {
  v0::Error error;
};

using Event =
    std::variant<ConnectionConfigured, Toggle, FocusIn, FocusOut,
                 ContextDestroyed, ConnectionAuthenticated, ConnectionClosed,
                 ConnectionFailed, ConnectionCommitText, ConnectionVadTimeout,
                 ConnectionErrorMessage>;

struct AcceptKey {};
struct Connect {};
struct Disconnect {};

struct SendEnvelope {
  v0::Envelope envelope;
};

struct CommitText {
  ContextId context;
  std::string text;
};

struct Log {
  LogLevel level;
  std::string message;
};

using Effect =
    std::variant<AcceptKey, Connect, Disconnect, SendEnvelope, CommitText, Log>;

// Engine is the fcitx-free session state machine. It consumes addon events and
// events from an already authenticated connection, then emits effect requests
// for both sides.
//
// The class is intentionally synchronous and deterministic so it can run on the
// fcitx thread and be unit-tested without fcitx.
class Engine final {
 public:
  using SessionIdGenerator = std::function<Result<SessionId>()>;
  Engine(SessionIdGenerator sessionIdGenerator = generateSessionId);

  // Applies one event to the state machine and returns the ordered list of
  // effects the adapter should perform.
  //
  // Parameters:
  // - event: state transition input from config, fcitx, or IO.
  std::vector<Effect> handle(const Event& event);

  bool transcribing() const noexcept;

  ConnectionState connectionState() const noexcept;

 private:
  struct ContextState {
    bool alive = true;
  };

  enum class AbortKind {
    kQuiet,
    kConnectionFailed,
  };

  struct SessionState {
    ContextId context;
    SessionId id;
    uint64_t nextSequenceNo = 1;
  };

  void handleEvent(const ConnectionConfigured& event,
                   std::vector<Effect>& effects);
  void handleEvent(const Toggle& event, std::vector<Effect>& effects);
  void handleEvent(const FocusIn& event, std::vector<Effect>& effects);
  void handleEvent(const FocusOut& event, std::vector<Effect>& effects);
  void handleEvent(const ContextDestroyed& event, std::vector<Effect>& effects);
  void handleEvent(const ConnectionAuthenticated& event,
                   std::vector<Effect>& effects);
  void handleEvent(const ConnectionClosed& event, std::vector<Effect>& effects);
  void handleEvent(const ConnectionFailed& event, std::vector<Effect>& effects);
  void handleEvent(const ConnectionCommitText& event,
                   std::vector<Effect>& effects);
  void handleEvent(const ConnectionVadTimeout& event,
                   std::vector<Effect>& effects);
  void handleEvent(const ConnectionErrorMessage& event,
                   std::vector<Effect>& effects);

  // Returns the mutable bookkeeping record for one logical input context,
  // creating a default record when the context has not been seen before.
  ContextState& ensureContext(ContextId context);

  // Checks whether a pending or active session is still allowed to commit into
  // the given context.
  bool contextCanCommit(ContextId context) const;

  bool contextFocused(ContextId context) const noexcept;

  void setFocusedContext(std::optional<ContextId> context) noexcept;

  void clearPending() noexcept;

  // Clears connection-level state after the authenticated peer goes away.
  void resetConnection(std::vector<Effect>& effects);

  // Resets session and connection state after a transport-level failure or
  // protocol violation.
  //
  // Parameters:
  // - reason: short human-readable explanation for logs.
  // - kind: whether this abort should surface as a user-visible interruption.
  void abortInteraction(std::vector<Effect>& effects, std::string_view reason,
                        AbortKind kind);

  // Ends the active session and emits an interrupt envelope when the
  // connection is still ready.
  //
  // Parameters:
  // - reason: protocol interrupt code sent to the transcriber.
  // - logReason: short human-readable reason used for local logging.
  void interruptSession(v0::InterruptReason reason, std::string_view logReason,
                        std::vector<Effect>& effects);

  // Starts a queued session once the connection is authenticated and the target
  // input context is still valid.
  void tryStartPending(std::vector<Effect>& effects);

  // Validates ordering and target context for one committed text fragment.
  void handleCommitText(const v0::CommitText& commitText,
                        std::vector<Effect>& effects);

  // Handles a server-side end-of-session signal caused by VAD timeout.
  void handleVadTimeout(const v0::VadTimeout& timeout,
                        std::vector<Effect>& effects);

  // Handles a transcriber-provided session error envelope.
  void handleErrorEnvelope(const v0::Error& error,
                           std::vector<Effect>& effects);

  void pushLog(std::vector<Effect>& effects, LogLevel level,
               std::string message) const;

  SessionIdGenerator sessionIdGenerator_;
  std::unordered_map<ContextId, ContextState> contexts_;
  std::optional<ConnectionConfig> connectionConfig_;
  std::optional<Error> connectionConfigError_;
  std::optional<ContextId> focusedContext_;
  std::optional<ContextId> pendingContext_;
  std::optional<SessionState> session_;
  ConnectionState connectionState_ = ConnectionState::kDisconnected;
};

}  // namespace scribe
