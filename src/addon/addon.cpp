#include "addon.hpp"

#include <fcitx-utils/event.h>
#include <fcitx/inputcontext.h>

#include <utility>

namespace scribe {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

FCITX_DEFINE_LOG_CATEGORY(scribe_log, "scribe");

Addon::Addon(fcitx::Instance* instance) : instance_(instance) {
  io_ = std::make_unique<Io>(
      instance_->eventDispatcher(),
      Io::Callbacks{
          .onAuthenticated = [this]() { dispatch(ConnectionAuthenticated{}); },
          .onClosed = [this]() { dispatch(ConnectionClosed{}); },
          .onFailure =
              [this](const Error& error) {
                dispatch(ConnectionFailed{.error = error});
              },
          .onCommitText =
              [this](const v0::CommitText& commitText) {
                dispatch(ConnectionCommitText{.commitText = commitText});
              },
          .onVadTimeout =
              [this](const v0::VadTimeout& vadTimeout) {
                dispatch(ConnectionVadTimeout{.vadTimeout = vadTimeout});
              },
          .onError =
              [this](const v0::Error& error) {
                dispatch(ConnectionErrorMessage{.error = error});
              },
      });

  reloadConfig();

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::Default,
      [this](fcitx::Event& event) { handleInputContextKeyEvent(event); }));

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextFocusIn, fcitx::EventWatcherPhase::Default,
      [this](fcitx::Event& event) { handleInputContextFocusInEvent(event); }));

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextFocusOut, fcitx::EventWatcherPhase::Default,
      [this](fcitx::Event& event) { handleInputContextFocusOutEvent(event); }));

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextDestroyed,
      fcitx::EventWatcherPhase::Default, [this](fcitx::Event& event) {
        handleInputContextDestroyedEvent(event);
      }));
}

Addon::~Addon() = default;

void Addon::setConfig(const fcitx::RawConfig& config) {
  config_.set(config);
  reloadResolvedConfig();
}

void Addon::reloadConfig() {
  config_.load();
  reloadResolvedConfig();
}

// Key handling must stay synchronous on the fcitx thread because hotkey
// acceptance is decided in this callback. The addon therefore feeds the engine
// directly and only offloads transport work through emitted effects.
void Addon::handleInputContextKeyEvent(fcitx::Event& event) {
  auto& keyEvent = static_cast<fcitx::KeyEvent&>(event);

  if (keyEvent.isRelease()) {
    return;
  }

  if (!keyEvent.key().checkKeyList(config_.toggleTranscriptionKey())) {
    return;
  }

  auto* inputContext = keyEvent.inputContext();
  if (inputContext == nullptr) {
    return;
  }

  if (dispatch(Toggle{.context = ensureContextId(inputContext),
                      .focused = inputContext->hasFocus()})) {
    keyEvent.filterAndAccept();
  }
}

void Addon::handleInputContextFocusInEvent(fcitx::Event& event) {
  auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);
  if (auto* inputContext = icEvent.inputContext(); inputContext != nullptr) {
    dispatch(FocusIn{.context = ensureContextId(inputContext)});
  }
}

void Addon::handleInputContextFocusOutEvent(fcitx::Event& event) {
  auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);
  if (auto* inputContext = icEvent.inputContext(); inputContext != nullptr) {
    dispatch(FocusOut{.context = ensureContextId(inputContext)});
  }
}

void Addon::handleInputContextDestroyedEvent(fcitx::Event& event) {
  auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);
  auto* inputContext = icEvent.inputContext();
  if (inputContext == nullptr) {
    return;
  }

  if (const auto contextId = findContextId(inputContext); contextId) {
    dispatch(ContextDestroyed{.context = *contextId});
    eraseContext(*contextId);
  }
}

void Addon::reloadResolvedConfig() {
  auto connectionConfig = config_.connectionConfig();
  if (connectionConfig) {
    SCRIBE_INFO() << "Transcriber connection configured: socket "
                  << connectionConfig->connection.socketPath << " from "
                  << connectionConfig->socketPathSource << ", auth token from "
                  << connectionConfig->authTokenSource;
    dispatch(ConnectionConfigured{
        .result = connectionConfig->connection,
    });
    return;
  }

  dispatch(ConnectionConfigured{
      .result = connectionConfig.error(),
  });
}

// The addon owns the fcitx pointer-to-id mapping. The engine only sees opaque
// ids so it can be tested without fcitx headers or objects.
ContextId Addon::ensureContextId(fcitx::InputContext* inputContext) {
  if (const auto iter = contexts_.left.find(inputContext);
      iter != contexts_.left.end()) {
    return iter->second;
  }

  const auto id = nextContextId_++;
  auto ref = inputContext->watch();
  contextsById_.emplace(id, ref);
  contexts_.insert(ContextMap::value_type(inputContext, id));
  return id;
}

std::optional<ContextId> Addon::findContextId(
    fcitx::InputContext* inputContext) const {
  if (const auto iter = contexts_.left.find(inputContext);
      iter != contexts_.left.end()) {
    return iter->second;
  }
  return std::nullopt;
}

void Addon::eraseContext(ContextId context) {
  const auto iter = contextsById_.find(context);
  if (iter == contextsById_.end()) {
    return;
  }

  if (const auto ptrIter = contexts_.right.find(context);
      ptrIter != contexts_.right.end()) {
    contexts_.right.erase(ptrIter);
  }

  contextsById_.erase(iter);
}

bool Addon::dispatch(const Event& event) {
  std::deque<Event> queue;
  queue.push_back(event);
  bool accepted = false;

  while (!queue.empty()) {
    auto current = std::move(queue.front());
    queue.pop_front();

    for (const auto& effect : engine_.handle(current)) {
      std::visit([this, &accepted,
                  &queue](const auto& value) { apply(value, accepted, queue); },
                 effect);
    }
  }

  return accepted;
}

// Applies adapter-side effects and may enqueue follow-up events when a local
// action fails immediately, such as configuration resolution, connect
// submission, or an outbound send attempted while IO is not ready.
void Addon::apply(const Effect& effect, bool& accepted,
                  std::deque<Event>& queue) {
  std::visit([this, &accepted,
              &queue](const auto& value) { apply(value, accepted, queue); },
             effect);
}

void Addon::apply(const AcceptKey&, bool& accepted, std::deque<Event>&) {
  accepted = true;
}

void Addon::apply(const Connect&, bool&, std::deque<Event>& queue) {
  auto connectionConfig = config_.connectionConfig();
  if (!connectionConfig) {
    queue.emplace_back(ConnectionFailed{.error = connectionConfig.error()});
    return;
  }
  if (auto result = io_->connect(connectionConfig->connection); !result) {
    queue.emplace_back(ConnectionFailed{.error = result.error()});
  }
}

void Addon::apply(const Disconnect&, bool&, std::deque<Event>&) {
  if (auto result = io_->disconnect(); !result) {
    SCRIBE_INFO() << result.error().message;
  }
}

void Addon::apply(const SendEnvelope& effect, bool&, std::deque<Event>& queue) {
  if (auto result = io_->send(effect.envelope); !result) {
    queue.emplace_back(ConnectionFailed{.error = result.error()});
  }
}

void Addon::apply(const CommitText& effect, bool&, std::deque<Event>& queue) {
  const auto iter = contextsById_.find(effect.context);
  if (iter == contextsById_.end()) {
    return;
  }
  if (auto* inputContext = iter->second.get(); inputContext != nullptr) {
    inputContext->commitString(effect.text);
  } else {
    eraseContext(effect.context);
    queue.emplace_back(ContextDestroyed{.context = effect.context});
  }
}

void Addon::apply(const Log& effect, bool&, std::deque<Event>&) {
  switch (effect.level) {
    case LogLevel::kDebug:
      SCRIBE_DEBUG() << effect.message;
      break;
    case LogLevel::kInfo:
      SCRIBE_INFO() << effect.message;
      break;
  }
}

}  // namespace scribe
