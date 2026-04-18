#pragma once

#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/addoninstance.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>

#include <memory>
#include <string_view>
#include <vector>

#include "config.hpp"

namespace scribe {

FCITX_DECLARE_LOG_CATEGORY(scribe_log);

#define SCRIBE_DEBUG() FCITX_LOGC(::scribe::scribe_log, Debug)
#define SCRIBE_INFO() FCITX_LOGC(::scribe::scribe_log, Info)

class Addon final : public fcitx::AddonInstance {
 public:
  explicit Addon(fcitx::Instance* instance);

  const fcitx::Configuration* getConfig() const override {
    return config_.configuration();
  }
  void setConfig(const fcitx::RawConfig& config) override;
  void reloadConfig() override;

  fcitx::Instance* instance() { return instance_; }
  bool transcribing() const { return transcribing_; }

 private:
  void handleInputContextKeyEvent(fcitx::Event& event);
  void handleInputContextFocusInEvent(fcitx::Event& event);
  void handleInputContextFocusOutEvent(fcitx::Event& event);
  void handleInputContextDestroyedEvent(fcitx::Event& event);
  void interruptTranscription(std::string_view reason);

  fcitx::Instance* instance_;
  Config config_;
  bool transcribing_ = false;
  fcitx::TrackableObjectReference<fcitx::InputContext> transcribingContext_;
  std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>>
      eventHandlers_;
};

}  // namespace scribe
