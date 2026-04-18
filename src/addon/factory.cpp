#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>

#include "addon.hpp"

namespace scribe {

class AddonFactory final : public fcitx::AddonFactory {
 public:
  fcitx::AddonInstance* create(fcitx::AddonManager* manager) override {
    return new Addon(manager->instance());
  }
};

}  // namespace scribe

FCITX_ADDON_FACTORY_V2(scribe, scribe::AddonFactory)
