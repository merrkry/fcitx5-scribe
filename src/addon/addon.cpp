#include "addon.hpp"

namespace scribe {

FCITX_DEFINE_LOG_CATEGORY(scribe_log, "scribe");

Addon::Addon(fcitx::Instance* instance) : instance_(instance) {
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

void Addon::setConfig(const fcitx::RawConfig& config) { config_.set(config); }

void Addon::reloadConfig() { config_.load(); }

void Addon::handleInputContextKeyEvent(fcitx::Event& event) {
  auto& keyEvent = static_cast<fcitx::KeyEvent&>(event);

  if (keyEvent.isRelease()) {
    return;
  }

  if (!keyEvent.key().checkKeyList(config_.toggleTranscriptionKey())) {
    return;
  }

  auto* inputContext = keyEvent.inputContext();

  if (transcribing_) {
    interruptTranscription("toggle");
  } else {
    transcribing_ = true;
    transcribingContext_ = inputContext->watch();
    SCRIBE_INFO() << "Transcription toggled: true";
  }

  keyEvent.filterAndAccept();
}

void Addon::handleInputContextFocusInEvent(fcitx::Event& event) {
  auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);
  auto* activeContext = transcribingContext_.get();

  if (!transcribing_ || activeContext == nullptr) {
    return;
  }

  if (icEvent.inputContext() != activeContext) {
    interruptTranscription("focus changed");
  }
}

void Addon::handleInputContextFocusOutEvent(fcitx::Event& event) {
  auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);

  if (!transcribing_) {
    return;
  }

  if (icEvent.inputContext() == transcribingContext_.get()) {
    interruptTranscription("focus lost");
  }
}

void Addon::handleInputContextDestroyedEvent(fcitx::Event& event) {
  auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);

  if (!transcribing_) {
    return;
  }

  if (icEvent.inputContext() == transcribingContext_.get()) {
    interruptTranscription("input context destroyed");
  }
}

void Addon::interruptTranscription(std::string_view reason) {
  if (!transcribing_) {
    return;
  }

  transcribing_ = false;
  transcribingContext_.unwatch();

  SCRIBE_INFO() << "Transcription interrupted (" << reason << ')';
}

}  // namespace scribe
