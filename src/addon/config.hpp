#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

namespace scribe {

FCITX_CONFIGURATION(ScribeConfigData,
                    fcitx::KeyListOption toggleTranscriptionKey{
                        this,
                        "ToggleTranscriptionKey",
                        _("Toggle Transcription"),
                        {},
                        fcitx::KeyListConstrain()};);

class Config {
 public:
  void load();
  void set(const fcitx::RawConfig& config);

  const fcitx::Configuration* configuration() const { return &data_; }
  const fcitx::KeyList& toggleTranscriptionKey() const {
    return data_.toggleTranscriptionKey.value();
  }

 private:
  static constexpr char configFile_[] = "conf/scribe.conf";

  void save() const;

  ScribeConfigData data_;
};

}  // namespace scribe
