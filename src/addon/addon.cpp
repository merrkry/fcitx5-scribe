#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>

namespace scribe {

class Addon final : public fcitx::AddonInstance {};

class AddonFactory final : public fcitx::AddonFactory {
public:
  fcitx::AddonInstance *create(fcitx::AddonManager *) override {
    return new Addon();
  }
};

} // namespace scribe

FCITX_ADDON_FACTORY_V2(scribe, scribe::AddonFactory)
