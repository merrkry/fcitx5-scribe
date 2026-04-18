#include "config.hpp"

#include <fcitx-config/iniparser.h>

namespace scribe {

constexpr char Config::configFile_[];

void Config::load() { readAsIni(data_, configFile_); }

void Config::set(const fcitx::RawConfig& config) {
  data_.load(config, true);
  save();
}

void Config::save() const { safeSaveAsIni(data_, configFile_); }

}  // namespace scribe
