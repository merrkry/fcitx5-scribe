#pragma once

#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/addoninstance.h>
#include <fcitx/event.h>
#include <fcitx/instance.h>

#include <boost/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "config.hpp"
#include "engine.hpp"
#include "io.hpp"

namespace scribe {

FCITX_DECLARE_LOG_CATEGORY(scribe_log);

#define SCRIBE_DEBUG() FCITX_LOGC(::scribe::scribe_log, Debug)
#define SCRIBE_INFO() FCITX_LOGC(::scribe::scribe_log, Info)

class Addon final : public fcitx::AddonInstance,
                    public fcitx::TrackableObject<Addon> {
 public:
  explicit Addon(fcitx::Instance* instance);

  ~Addon() override;

  const fcitx::Configuration* getConfig() const override {
    return config_.configuration();
  }

  void setConfig(const fcitx::RawConfig& config) override;

  void reloadConfig() override;

  fcitx::Instance* instance() { return instance_; }

  bool transcribing() const { return engine_.transcribing(); }

 private:
  using ContextMap =
      boost::bimap<boost::bimaps::unordered_set_of<fcitx::InputContext*>,
                   boost::bimaps::unordered_set_of<ContextId>>;

  void handleInputContextKeyEvent(fcitx::Event& event);
  void handleInputContextFocusInEvent(fcitx::Event& event);
  void handleInputContextFocusOutEvent(fcitx::Event& event);
  void handleInputContextDestroyedEvent(fcitx::Event& event);

  // Recomputes the resolved connection config and pushes the result into the
  // engine as a configuration event.
  void reloadResolvedConfig();

  // Returns a stable opaque context id for one fcitx input context.
  //
  // Parameters:
  // - inputContext: live fcitx input context pointer owned by fcitx.
  //
  // Assumptions:
  // - inputContext is non-null.
  // - The same pointer maps to the same id until InputContextDestroyed.
  ContextId ensureContextId(fcitx::InputContext* inputContext);

  // Looks up the existing opaque context id for an fcitx input context.
  //
  // Parameters:
  // - inputContext: fcitx input context pointer, possibly unknown to the addon.
  std::optional<ContextId> findContextId(
      fcitx::InputContext* inputContext) const;

  // Removes both direction mappings for one context id.
  //
  // Parameters:
  // - context: opaque context id previously allocated by ensureContextId().
  void eraseContext(ContextId context);

  // Runs one engine event to completion, including follow-up events generated
  // by adapter-side effects such as connection or send failures.
  //
  // Parameters:
  // - event: input event coming from fcitx, config reload, or IO.
  //
  // Returns:
  // - true when the engine requested synchronous key acceptance.
  bool dispatch(const Event& event);

  void apply(const Effect& effect, bool& accepted, std::deque<Event>& queue);
  void apply(const AcceptKey& effect, bool& accepted, std::deque<Event>& queue);
  void apply(const Connect& effect, bool& accepted, std::deque<Event>& queue);
  void apply(const Disconnect& effect, bool& accepted,
             std::deque<Event>& queue);
  void apply(const SendEnvelope& effect, bool& accepted,
             std::deque<Event>& queue);
  void apply(const CommitText& effect, bool& accepted,
             std::deque<Event>& queue);
  void apply(const Log& effect, bool& accepted, std::deque<Event>& queue);

  fcitx::Instance* instance_;
  Config config_;
  Engine engine_;
  std::unique_ptr<Io> io_;
  ContextId nextContextId_ = 1;
  ContextMap contexts_;
  std::unordered_map<ContextId,
                     fcitx::TrackableObjectReference<fcitx::InputContext>>
      contextsById_;
  std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>>
      eventHandlers_;
};

}  // namespace scribe
