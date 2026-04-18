#include "io.hpp"

#include <arpa/inet.h>
#include <openssl/crypto.h>

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <cstring>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <variant>

#include "util.hpp"

namespace scribe {

namespace {

namespace asio = boost::asio;
using Socket = asio::local::stream_protocol::socket;
using Endpoint = asio::local::stream_protocol::endpoint;
using ErrorCode = boost::system::error_code;

Error describeError(ErrorKind kind, std::string_view context,
                    const ErrorCode& error) {
  return makeError(kind, std::string(context) + ": " + error.message());
}

bool proofMatches(std::string_view input, const std::vector<uint8_t>& proof) {
  return input.size() == proof.size() &&
         CRYPTO_memcmp(input.data(), proof.data(), input.size()) == 0;
}

}  // namespace

struct Io::SharedState : public std::enable_shared_from_this<Io::SharedState> {
  struct ConnectCommand {
    ConnectionConfig config;
  };

  struct DisconnectCommand {};

  struct SendCommand {
    std::shared_ptr<std::vector<uint8_t>> frame;
  };

  using Command = std::variant<ConnectCommand, DisconnectCommand, SendCommand>;
  using CommandChannel =
      asio::experimental::concurrent_channel<asio::any_io_executor,
                                             void(ErrorCode, Command)>;

  explicit SharedState(fcitx::EventDispatcher& dispatcher, Callbacks callbacks)
      : dispatcher(dispatcher),
        callbacks(std::move(callbacks)),
        workGuard(asio::make_work_guard(ioContext)),
        commands(ioContext.get_executor(), 128),
        socket(ioContext) {}

  // Main actor loop for the transport thread. All socket lifecycle changes and
  // outbound queue mutations happen here so the addon only needs to send coarse
  // commands.
  asio::awaitable<void> run() {
    while (!isStopping()) {
      auto [messageError, command] =
          co_await commands.async_receive(asio::as_tuple(asio::use_awaitable));
      if (messageError) {
        if (isStopping()) {
          break;
        }
        continue;
      }

      std::visit(
          [this](auto&& value) {
            handleCommand(std::forward<decltype(value)>(value));
          },
          std::move(command));
    }
  }

  void handleCommand(ConnectCommand command) {
    if (isStopping() || connectionActive) {
      return;
    }

    connectionActive = true;
    const auto generation = ++generationCounter;
    asio::co_spawn(
        ioContext,
        [shared = shared_from_this(), generation,
         command = std::move(command)]() mutable {
          return shared->runConnection(generation, std::move(command));
        },
        asio::detached);
  }

  void handleCommand(DisconnectCommand) {
    closeConnection(std::nullopt, false);
  }

  void handleCommand(SendCommand command) {
    if (!connected || !socket.is_open()) {
      return;
    }

    outgoingFrames.push_back(std::move(command.frame));
    if (writerRunning) {
      return;
    }

    writerRunning = true;
    const auto generation = activeGeneration;
    asio::co_spawn(
        ioContext,
        [shared = shared_from_this(), generation]() {
          return shared->writeLoop(generation);
        },
        asio::detached);
  }

  // Starts one connection generation. Generations prevent stale read/write
  // coroutines from operating on a newly reopened socket after reconnects.
  asio::awaitable<void> runConnection(uint64_t generation,
                                      ConnectCommand command) {
    activeGeneration = generation;
    currentConfig = command.config;
    ErrorCode error;

    try {
      socket = Socket(ioContext);
      Endpoint endpoint(command.config.socketPath);
      co_await socket.async_connect(
          endpoint,
          asio::cancel_after(command.config.connectTimeout,
                             asio::redirect_error(asio::use_awaitable, error)));
      if (error) {
        closeConnection(describeError(ErrorKind::kTransport,
                                      "failed to connect to transcriber at " +
                                          command.config.socketPath,
                                      error),
                        true);
        co_return;
      }
    } catch (const std::exception& exception) {
      closeConnection(
          makeError(ErrorKind::kTransport,
                    "failed to connect to transcriber at " +
                        command.config.socketPath + ": " + exception.what()),
          true);
      co_return;
    }

    auto ready = co_await authenticate(generation);
    if (!ready) {
      co_return;
    }

    connected.store(true, std::memory_order_release);
    publishAuthenticated();

    asio::co_spawn(
        ioContext,
        [shared = shared_from_this(), generation]() {
          return shared->readLoop(generation);
        },
        asio::detached);
  }

  // Continuously reads framed post-authentication envelopes until the socket
  // closes or the generation becomes stale.
  asio::awaitable<void> readLoop(uint64_t generation) {
    while (!isStopping() && generation == activeGeneration &&
           socket.is_open()) {
      std::array<uint8_t, sizeof(uint32_t)> header{};
      ErrorCode error;
      co_await asio::async_read(
          socket, asio::buffer(header),
          asio::redirect_error(asio::use_awaitable, error));
      if (error) {
        if (error == asio::error::eof) {
          closeConnection(std::nullopt, false);
          co_return;
        }
        closeConnection(describeError(ErrorKind::kTransport,
                                      "failed to read frame length", error),
                        true);
        co_return;
      }

      uint32_t networkSize = 0;
      std::memcpy(&networkSize, header.data(), sizeof(networkSize));
      const uint32_t frameSize = ntohl(networkSize);
      if (frameSize == 0 || frameSize > kMaxFrameSize) {
        closeConnection(makeError(ErrorKind::kProtocolViolation,
                                  "invalid inbound frame size"),
                        true);
        co_return;
      }

      std::vector<uint8_t> payload(frameSize);
      co_await asio::async_read(
          socket, asio::buffer(payload),
          asio::redirect_error(asio::use_awaitable, error));
      if (error) {
        closeConnection(describeError(ErrorKind::kTransport,
                                      "failed to read inbound frame", error),
                        true);
        co_return;
      }

      auto envelope = decodeFrame(payload);
      if (!envelope) {
        closeConnection(std::move(envelope.error()), true);
        co_return;
      }

      if (envelope->has_commit_text()) {
        publishCommitText(envelope->commit_text());
        continue;
      }
      if (envelope->has_vad_timeout()) {
        publishVadTimeout(envelope->vad_timeout());
        continue;
      }
      if (envelope->has_error()) {
        publishError(envelope->error());
        continue;
      }

      closeConnection(makeError(ErrorKind::kProtocolViolation,
                                "unexpected message from authenticated peer"),
                      true);
      co_return;
    }
  }

  // Drains queued outbound frames in order. The engine already decides message
  // ordering, so IO preserves that order byte-for-byte.
  asio::awaitable<void> writeLoop(uint64_t generation) {
    while (!isStopping() && generation == activeGeneration &&
           socket.is_open() && !outgoingFrames.empty()) {
      auto frame = outgoingFrames.front();
      ErrorCode error;
      co_await asio::async_write(
          socket, asio::buffer(*frame),
          asio::redirect_error(asio::use_awaitable, error));
      if (error) {
        writerRunning = false;
        closeConnection(describeError(ErrorKind::kTransport,
                                      "failed to write to transcriber", error),
                        true);
        co_return;
      }

      outgoingFrames.pop_front();
    }

    writerRunning = false;
  }

  // Closes the current connection generation and reports one terminal event
  // back to the addon thread through the dispatcher.
  //
  // Parameters:
  // - error: failure reason when the connection ended abnormally.
  // - reportFailure: when true, publish the failure instead of a separate close
  //   event so the engine handles termination exactly once.
  void closeConnection(std::optional<Error> error, bool reportFailure) {
    const bool hadConnection = connectionActive || connected.load() ||
                               socket.is_open() || writerRunning;
    if (!hadConnection) {
      return;
    }

    const bool wasConnected =
        connected.exchange(false, std::memory_order_acq_rel);
    connectionActive = false;
    writerRunning = false;
    ++activeGeneration;
    outgoingFrames.clear();
    currentConfig.reset();
    clientNonce.reset();

    ErrorCode ignored;
    socket.cancel(ignored);
    socket.close(ignored);

    if (reportFailure && error && !isStopping()) {
      publishFailure(*error);
    } else if (wasConnected) {
      publishClosed();
    }
  }

  void stop() {
    setStopping();
    ErrorCode ignored;
    commands.close();
    socket.cancel(ignored);
    socket.close(ignored);
    ioContext.stop();
  }

  bool isStopping() const {
    std::lock_guard lock(stoppingMutex);
    return stopping;
  }

  void setStopping() {
    std::lock_guard lock(stoppingMutex);
    stopping = true;
  }

  // Runs the scribe handshake on a newly connected socket.
  //
  // Parameters:
  // - generation: connection generation that must still be current when the
  //   handshake completes.
  //
  // Assumptions:
  // - currentConfig is populated for this generation.
  // - socket is connected but not yet exposed to the engine as authenticated.
  asio::awaitable<bool> authenticate(uint64_t generation) {
    if (!currentConfig) {
      closeConnection(makeError(ErrorKind::kState,
                                "handshake started without connection config"),
                      true);
      co_return false;
    }

    auto nonce = generateNonce();
    if (!nonce) {
      closeConnection(
          makeError(ErrorKind::kOpenSsl, "failed to generate client nonce"),
          true);
      co_return false;
    }

    clientNonce = *nonce;

    v0::Envelope helloEnvelope;
    auto* hello = helloEnvelope.mutable_client_hello();
    hello->set_protocol_version(std::string(kProtocolVersion));
    hello->set_client_name(std::string(kClientName));
    hello->set_client_nonce(clientNonce->data(), clientNonce->size());

    auto encodedHello = encodeFrame(helloEnvelope);
    if (!encodedHello) {
      closeConnection(encodedHello.error(), true);
      co_return false;
    }

    ErrorCode error;
    co_await asio::async_write(
        socket, asio::buffer(*encodedHello),
        asio::redirect_error(asio::use_awaitable, error));
    if (error) {
      closeConnection(describeError(ErrorKind::kTransport,
                                    "failed to write ClientHello", error),
                      true);
      co_return false;
    }

    std::array<uint8_t, sizeof(uint32_t)> header{};
    co_await asio::async_read(
        socket, asio::buffer(header),
        asio::cancel_after(currentConfig->connectTimeout,
                           asio::redirect_error(asio::use_awaitable, error)));
    if (error) {
      closeConnection(
          describeError(ErrorKind::kTransport,
                        "failed to read handshake frame length", error),
          true);
      co_return false;
    }

    uint32_t networkSize = 0;
    std::memcpy(&networkSize, header.data(), sizeof(networkSize));
    const uint32_t frameSize = ntohl(networkSize);
    if (frameSize == 0 || frameSize > kMaxFrameSize) {
      closeConnection(makeError(ErrorKind::kProtocolViolation,
                                "invalid inbound handshake frame size"),
                      true);
      co_return false;
    }

    std::vector<uint8_t> payload(frameSize);
    co_await asio::async_read(
        socket, asio::buffer(payload),
        asio::cancel_after(currentConfig->connectTimeout,
                           asio::redirect_error(asio::use_awaitable, error)));
    if (error) {
      closeConnection(describeError(ErrorKind::kTransport,
                                    "failed to read handshake frame", error),
                      true);
      co_return false;
    }

    auto envelope = decodeFrame(payload);
    if (!envelope) {
      closeConnection(envelope.error(), true);
      co_return false;
    }
    if (!envelope->has_server_hello()) {
      closeConnection(makeError(ErrorKind::kProtocolViolation,
                                "expected ServerHello during handshake"),
                      true);
      co_return false;
    }

    auto validation = validateServerHello(envelope->server_hello());
    if (!validation) {
      closeConnection(validation.error(), true);
      co_return false;
    }

    if (generation != activeGeneration || isStopping() || !socket.is_open()) {
      co_return false;
    }

    clientNonce.reset();
    co_return true;
  }

  // Validates the transcriber response to ClientHello.
  //
  // Parameters:
  // - hello: decoded ServerHello envelope payload from the connected peer.
  //
  // Assumptions:
  // - currentConfig and clientNonce still describe the in-flight handshake.
  Result<void> validateServerHello(const v0::ServerHello& hello) {
    if (!hello.accepted()) {
      return std::unexpected(makeError(
          ErrorKind::kProtocolViolation,
          hello.error_message().empty()
              ? "transcriber rejected handshake"
              : "transcriber rejected handshake: " + hello.error_message()));
    }

    if (hello.protocol_version() != kProtocolVersion) {
      return std::unexpected(
          makeError(ErrorKind::kProtocolViolation,
                    "transcriber protocol version mismatch"));
    }

    if (!currentConfig || !clientNonce) {
      return std::unexpected(
          makeError(ErrorKind::kState, "handshake state is incomplete"));
    }

    Nonce serverNonce;
    if (!copyExactBytes(hello.server_nonce(),
                        std::span<uint8_t>(serverNonce))) {
      return std::unexpected(
          makeError(ErrorKind::kProtocolViolation,
                    "transcriber returned an invalid nonce"));
    }

    auto expectedProof =
        computeServerProof(currentConfig->authToken, *clientNonce, serverNonce);
    if (!expectedProof) {
      return std::unexpected(expectedProof.error());
    }

    if (!proofMatches(hello.proof(), *expectedProof)) {
      return std::unexpected(makeError(ErrorKind::kProtocolViolation,
                                       "transcriber authentication failed"));
    }

    return {};
  }

  void publishAuthenticated() {
    dispatcher.scheduleWithContext(owner, [shared = shared_from_this()]() {
      if (shared->callbacks.onAuthenticated) {
        shared->callbacks.onAuthenticated();
      }
    });
  }

  void publishClosed() {
    dispatcher.scheduleWithContext(owner, [shared = shared_from_this()]() {
      if (shared->callbacks.onClosed) {
        shared->callbacks.onClosed();
      }
    });
  }

  void publishCommitText(v0::CommitText commitText) {
    dispatcher.scheduleWithContext(
        owner,
        [shared = shared_from_this(), commitText = std::move(commitText)]() {
          if (shared->callbacks.onCommitText) {
            shared->callbacks.onCommitText(commitText);
          }
        });
  }

  void publishVadTimeout(v0::VadTimeout vadTimeout) {
    dispatcher.scheduleWithContext(
        owner,
        [shared = shared_from_this(), vadTimeout = std::move(vadTimeout)]() {
          if (shared->callbacks.onVadTimeout) {
            shared->callbacks.onVadTimeout(vadTimeout);
          }
        });
  }

  void publishError(v0::Error protocolError) {
    dispatcher.scheduleWithContext(
        owner, [shared = shared_from_this(),
                protocolError = std::move(protocolError)]() {
          if (shared->callbacks.onError) {
            shared->callbacks.onError(protocolError);
          }
        });
  }

  void publishFailure(Error error) {
    dispatcher.scheduleWithContext(
        owner, [shared = shared_from_this(), error = std::move(error)]() {
          if (shared->callbacks.onFailure) {
            shared->callbacks.onFailure(error);
          }
        });
  }

  fcitx::EventDispatcher& dispatcher;
  Callbacks callbacks;
  asio::io_context ioContext;
  asio::executor_work_guard<asio::io_context::executor_type> workGuard;
  CommandChannel commands;
  Socket socket;
  std::thread worker;
  std::atomic<bool> connected{false};
  // This stop path is intentionally mutex-backed. The code is not
  // performance-critical, and a mutex keeps the shared shutdown state easier to
  // audit and easier to extend if it grows beyond a single flag later.
  mutable std::mutex stoppingMutex;
  bool stopping = false;
  bool connectionActive = false;
  bool writerRunning = false;
  uint64_t generationCounter = 0;
  uint64_t activeGeneration = 0;
  std::deque<std::shared_ptr<std::vector<uint8_t>>> outgoingFrames;
  std::optional<ConnectionConfig> currentConfig;
  std::optional<Nonce> clientNonce;
  fcitx::TrackableObjectReference<Io> owner;
};

Io::Io(fcitx::EventDispatcher& dispatcher, Callbacks callbacks)
    : dispatcher_(dispatcher),
      shared_(std::make_shared<SharedState>(dispatcher_, std::move(callbacks))),
      self_(watch()) {
  shared_->owner = self_;
  shared_->worker = std::thread([shared = shared_]() {
    asio::co_spawn(shared->ioContext, shared->run(), asio::detached);
    shared->ioContext.run();
  });
}

Io::~Io() {
  if (!shared_) {
    return;
  }
  shared_->stop();
  if (shared_->worker.joinable()) {
    shared_->worker.join();
  }
}

Result<void> Io::connect(const ConnectionConfig& config) {
  const bool queued = shared_->commands.try_send(
      ErrorCode{}, SharedState::ConnectCommand{.config = config});
  if (!queued) {
    return std::unexpected(makeError(
        ErrorKind::kTransport,
        "failed to queue connect command for socket " + config.socketPath));
  }

  return {};
}

Result<void> Io::disconnect() {
  const bool queued =
      shared_->commands.try_send(ErrorCode{}, SharedState::DisconnectCommand{});
  if (!queued) {
    return std::unexpected(
        makeError(ErrorKind::kTransport, "failed to queue disconnect command"));
  }

  return {};
}

Result<void> Io::send(const v0::Envelope& envelope) {
  if (!shared_->connected.load(std::memory_order_acquire)) {
    return std::unexpected(
        makeError(ErrorKind::kState, "transcriber connection not ready"));
  }

  auto encoded = encodeFrame(envelope);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }

  const bool queued = shared_->commands.try_send(
      ErrorCode{},
      SharedState::SendCommand{
          .frame = std::make_shared<std::vector<uint8_t>>(std::move(*encoded)),
      });
  if (!queued) {
    return std::unexpected(
        makeError(ErrorKind::kTransport, "failed to queue outbound frame"));
  }

  return {};
}

}  // namespace scribe
