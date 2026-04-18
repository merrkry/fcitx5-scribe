#include "config.hpp"

#include <fcitx-config/iniparser.h>
#include <wordexp.h>

#include <cstdlib>

namespace scribe {
namespace {

constexpr char kSocketPathEnv[] = "FCITX5_SCRIBE_DEBUG_SOCKET_PATH";
constexpr char kAuthTokenEnv[] = "FCITX5_SCRIBE_DEBUG_AUTH_TOKEN";

std::string readEnv(const char* name) {
  if (const char* value = std::getenv(name);
      value != nullptr && value[0] != '\0') {
    return value;
  }

  return {};
}

}  // namespace

constexpr char Config::configFile_[];

void Config::load() {
  readAsIni(data_, configFile_);
  resolveSocketPath();
}

void Config::set(const fcitx::RawConfig& config) {
  data_.load(config, true);
  resolveSocketPath();
  save();
}

void Config::save() const { safeSaveAsIni(data_, configFile_); }

// Expands SocketPath using shell-style environment expansion but forbids
// command execution. The result is cached so the engine and addon work with a
// validated path string instead of repeatedly parsing config text.
void Config::resolveSocketPath() {
  resolvedSocketPath_.clear();
  socketPathValid_ = true;

  if (data_.socketPath.value().empty()) {
    return;
  }

  wordexp_t socketPath{};
  const bool socketPathResolved =
      wordexp(data_.socketPath.value().c_str(), &socketPath,
              WRDE_NOCMD | WRDE_UNDEF) == 0;
  if (!socketPathResolved || socketPath.we_wordc != 1 ||
      socketPath.we_wordv[0][0] == '\0') {
    socketPathValid_ = false;
    if (socketPathResolved) {
      wordfree(&socketPath);
    }
    return;
  }

  resolvedSocketPath_ = socketPath.we_wordv[0];
  wordfree(&socketPath);
}

std::string Config::effectiveSocketPath() const {
  // Debug-only overrides stay lazy and unvalidated on purpose. The DEBUG
  // prefix keeps them distinct from normal environment-driven configuration so
  // an unrelated session variable does not silently bypass persisted-path
  // validation for one running addon instance.
  if (auto override = readEnv(kSocketPathEnv); !override.empty()) {
    return override;
  }

  return configuredSocketPath();
}

std::string Config::effectiveSocketPathSource() const {
  if (!readEnv(kSocketPathEnv).empty()) {
    return kSocketPathEnv;
  }

  return "config";
}

std::string Config::configuredAuthToken() const {
  if (!data_.authTokenEnv.value().empty()) {
    if (const char* value = std::getenv(data_.authTokenEnv.value().c_str());
        value != nullptr && value[0] != '\0') {
      return value;
    }
  }

  return {};
}

std::string Config::effectiveAuthToken() const {
  // Keep the auth-token override aligned with effectiveSocketPath(): this is a
  // DEBUG-only development escape hatch rather than normal configuration, so
  // it is read only when needed and not folded into saved config state.
  if (auto override = readEnv(kAuthTokenEnv); !override.empty()) {
    return override;
  }

  return configuredAuthToken();
}

std::string Config::effectiveAuthTokenSource() const {
  if (!readEnv(kAuthTokenEnv).empty()) {
    return kAuthTokenEnv;
  }

  if (!data_.authTokenEnv.value().empty()) {
    return "configured env var " + data_.authTokenEnv.value();
  }

  return "config";
}

Result<ResolvedConnectionConfig> Config::connectionConfig() const {
  auto socketPathOverride = readEnv(kSocketPathEnv);
  if (data_.socketPath.value().empty() && socketPathOverride.empty()) {
    return std::unexpected(
        makeError(ErrorKind::kConfig,
                  "Cannot connect to transcriber: SocketPath is empty"));
  }

  if (!data_.socketPath.value().empty() && !socketPathValid_ &&
      socketPathOverride.empty()) {
    return std::unexpected(
        makeError(ErrorKind::kConfig,
                  "Cannot connect to transcriber: SocketPath is invalid"));
  }

  auto token = effectiveAuthToken();
  if (token.empty()) {
    return std::unexpected(
        makeError(ErrorKind::kConfig,
                  "Cannot connect to transcriber: auth token is empty"));
  }

  return ResolvedConnectionConfig{
      .connection =
          ConnectionConfig{
              .socketPath = effectiveSocketPath(),
              .authToken = std::move(token),
              .connectTimeout = std::chrono::milliseconds(connectTimeoutMs()),
          },
      .socketPathSource = effectiveSocketPathSource(),
      .authTokenSource = effectiveAuthTokenSource(),
  };
}

}  // namespace scribe
