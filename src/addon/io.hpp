#pragma once

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/trackableobject.h>

#include <functional>
#include <memory>

#include "error.hpp"
#include "scribe/v0/scribe.pb.h"
#include "types.hpp"

namespace scribe {

// Io owns the authenticated transcriber connection.
//
// It establishes the Unix socket, runs the scribe handshake, validates the
// transcriber proof, and only reports post-authentication protocol messages
// back onto the fcitx dispatcher thread.
//
// Callback lifetime model:
// - Callbacks are invoked asynchronously on the fcitx dispatcher thread after
//   connect() returns and before Io destruction completes.
// - Captured objects must therefore outlive Io or be guarded with shared/weak
//   ownership. When binding fcitx-owned objects, prefer the TrackableObject
//   watch/reference pattern used by the addon.
class Io final : public fcitx::TrackableObject<Io> {
 public:
  struct Callbacks {
    std::function<void()> onAuthenticated;
    std::function<void()> onClosed;
    std::function<void(const Error&)> onFailure;
    std::function<void(const v0::CommitText&)> onCommitText;
    std::function<void(const v0::VadTimeout&)> onVadTimeout;
    std::function<void(const v0::Error&)> onError;
  };

  Io(fcitx::EventDispatcher& dispatcher, Callbacks callbacks);
  ~Io();

  Io(const Io&) = delete;
  Io& operator=(const Io&) = delete;

  Io(Io&&) = delete;
  Io& operator=(Io&&) = delete;

  // Queues a connection request for the worker thread.
  //
  // Parameters:
  // - config: transport and authentication parameters for the peer.
  //
  // Returns:
  // - success when the command was queued for execution on the worker thread.
  Result<void> connect(const ConnectionConfig& config);

  // Queues a graceful disconnect request for the worker thread.
  //
  // Returns:
  // - success when the command was queued for execution on the worker thread.
  Result<void> disconnect();

  // Encodes and queues one outbound post-authentication protocol envelope.
  //
  // Parameters:
  // - envelope: logical protocol message to write.
  //
  // Assumptions:
  // - IO has already reported onAuthenticated for the current connection.
  Result<void> send(const v0::Envelope& envelope);

 private:
  struct SharedState;

  fcitx::EventDispatcher& dispatcher_;
  std::shared_ptr<SharedState> shared_;
  fcitx::TrackableObjectReference<Io> self_;
};

}  // namespace scribe
