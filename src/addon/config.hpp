#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

#include <string>

#include "error.hpp"
#include "types.hpp"

namespace scribe {

struct ResolvedConnectionConfig {
  ConnectionConfig connection;
  std::string socketPathSource;
  std::string authTokenSource;
};

FCITX_CONFIGURATION(ScribeConfigData,
                    fcitx::KeyListOption toggleTranscriptionKey{
                        this,
                        "ToggleTranscriptionKey",
                        _("Toggle Transcription"),
                        {},
                        fcitx::KeyListConstrain()};
                    fcitx::Option<std::string> socketPath{this, "SocketPath",
                                                          _("Socket Path"), ""};
                    fcitx::Option<std::string> authTokenEnv{
                        this, "AuthTokenEnv",
                        _("Authentication Token Environment Variable"), ""};
                    fcitx::Option<int, fcitx::IntConstrain> connectTimeoutMs{
                        this, "ConnectTimeoutMs", _("Connect Timeout (ms)"),
                        1000, fcitx::IntConstrain(1, 60000)};);

class Config {
 public:
  void load();

  void set(const fcitx::RawConfig& config);

  const fcitx::Configuration* configuration() const { return &data_; }

  const fcitx::KeyList& toggleTranscriptionKey() const {
    return data_.toggleTranscriptionKey.value();
  }

  const std::string& socketPath() const { return data_.socketPath.value(); }

  const std::string& resolvedSocketPath() const { return resolvedSocketPath_; }

  bool socketPathValid() const { return socketPathValid_; }

  const std::string& authTokenEnv() const { return data_.authTokenEnv.value(); }

  int connectTimeoutMs() const { return data_.connectTimeoutMs.value(); }

  // Returns the socket path from persisted config, before any env override.
  //
  // Assumptions:
  // - The persisted socket path has already been loaded and expanded.
  const std::string& configuredSocketPath() const {
    return resolvedSocketPath_;
  }

  // Returns the live socket path after applying the optional dev env override.
  //
  // Assumptions:
  // - The persisted socket path remains unchanged and is what the GUI edits.
  std::string effectiveSocketPath() const;

  // Describes where effectiveSocketPath() came from without exposing secrets.
  std::string effectiveSocketPathSource() const;

  // Reads the authentication token from the configured environment variable,
  // before applying any direct dev token override.
  //
  // Assumptions:
  // - The environment variable may be unset or empty, in which case the result
  //   is an empty string.
  std::string configuredAuthToken() const;

  // Returns the live authentication token after applying the optional dev env
  // override. The returned secret must not be logged.
  std::string effectiveAuthToken() const;

  // Describes where effectiveAuthToken() came from without exposing secrets.
  std::string effectiveAuthTokenSource() const;

  // Returns the validated connection parameters needed by the engine and IO.
  //
  // Assumptions:
  // - On-disk config has already been loaded and remains the GUI-visible source
  //   of truth.
  // - Env overrides are applied only to the returned live connection values.
  Result<ResolvedConnectionConfig> connectionConfig() const;

 private:
  static constexpr char configFile_[] = "conf/scribe.conf";

  void save() const;
  void resolveSocketPath();

  ScribeConfigData data_;
  std::string resolvedSocketPath_;
  bool socketPathValid_ = true;
};

}  // namespace scribe
