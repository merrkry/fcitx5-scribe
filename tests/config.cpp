#include "config.hpp"

#include <fcitx-config/rawconfig.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <string>

namespace scribe {
namespace {

constexpr char kSocketPathEnv[] = "FCITX5_SCRIBE_DEBUG_SOCKET_PATH";
constexpr char kAuthTokenEnv[] = "FCITX5_SCRIBE_DEBUG_AUTH_TOKEN";
constexpr char kConfiguredAuthTokenEnv[] = "SCRIBE_TEST_AUTH_TOKEN";

struct ScopedEnvVar {
  explicit ScopedEnvVar(const char* name) : name(name) {
    if (const char* current = std::getenv(name); current != nullptr) {
      hadValue = true;
      value = current;
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
  ScopedEnvVar(ScopedEnvVar&&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

  ~ScopedEnvVar() {
    if (hadValue) {
      setenv(name, value.c_str(), 1);
    } else {
      unsetenv(name);
    }
  }

  const char* name;
  bool hadValue = false;
  std::string value;
};

TEST_CASE("env config overrides persisted socket and auth settings",
          "[config]") {
  ScopedEnvVar socketPath{kSocketPathEnv};
  ScopedEnvVar authToken{kAuthTokenEnv};
  ScopedEnvVar configuredAuthToken{kConfiguredAuthTokenEnv};

  setenv(kSocketPathEnv, "/run/user/1000/fcitx5-scribe/dev.sock", 1);
  setenv(kAuthTokenEnv, "dev-secret", 1);
  setenv(kConfiguredAuthTokenEnv, "config-secret", 1);

  fcitx::RawConfig rawConfig;
  rawConfig.setValueByPath("SocketPath", "/tmp/config.sock");
  rawConfig.setValueByPath("AuthTokenEnv", kConfiguredAuthTokenEnv);
  rawConfig.setValueByPath("ConnectTimeoutMs", "3210");

  Config config;
  config.set(rawConfig);

  CHECK(config.socketPath() == "/tmp/config.sock");
  CHECK(config.configuredSocketPath() == "/tmp/config.sock");
  CHECK(config.authTokenEnv() == kConfiguredAuthTokenEnv);
  const auto connection = config.connectionConfig();
  REQUIRE(connection.has_value());
  CHECK(connection->connection.socketPath ==
        "/run/user/1000/fcitx5-scribe/dev.sock");
  CHECK(connection->connection.authToken == "dev-secret");
  CHECK(connection->socketPathSource == kSocketPathEnv);
  CHECK(connection->authTokenSource == kAuthTokenEnv);
  CHECK(connection->connection.connectTimeout ==
        std::chrono::milliseconds(3210));
}

TEST_CASE("connection config falls back independently to persisted settings",
          "[config]") {
  ScopedEnvVar socketPath{kSocketPathEnv};
  ScopedEnvVar authToken{kAuthTokenEnv};
  ScopedEnvVar configuredAuthToken{kConfiguredAuthTokenEnv};

  unsetenv(kSocketPathEnv);
  unsetenv(kAuthTokenEnv);
  setenv(kConfiguredAuthTokenEnv, "config-secret", 1);

  fcitx::RawConfig rawConfig;
  rawConfig.setValueByPath("SocketPath", "/tmp/config.sock");
  rawConfig.setValueByPath("AuthTokenEnv", kConfiguredAuthTokenEnv);

  Config config;
  config.set(rawConfig);

  auto connection = config.connectionConfig();
  REQUIRE(connection.has_value());
  CHECK(config.socketPath() == "/tmp/config.sock");
  CHECK(config.configuredSocketPath() == "/tmp/config.sock");
  CHECK(config.authTokenEnv() == kConfiguredAuthTokenEnv);
  CHECK(connection->connection.socketPath == "/tmp/config.sock");
  CHECK(connection->connection.authToken == "config-secret");
  CHECK(connection->socketPathSource == "config");
  CHECK(connection->authTokenSource ==
        std::string("configured env var ") + kConfiguredAuthTokenEnv);

  setenv(kSocketPathEnv, "/tmp/env.sock", 1);
  connection = config.connectionConfig();
  REQUIRE(connection.has_value());
  CHECK(config.socketPath() == "/tmp/config.sock");
  CHECK(connection->connection.socketPath == "/tmp/env.sock");
  CHECK(connection->connection.authToken == "config-secret");
  CHECK(connection->socketPathSource == kSocketPathEnv);
  CHECK(connection->authTokenSource ==
        std::string("configured env var ") + kConfiguredAuthTokenEnv);

  unsetenv(kSocketPathEnv);
  setenv(kAuthTokenEnv, "env-secret", 1);
  connection = config.connectionConfig();
  REQUIRE(connection.has_value());
  CHECK(config.authTokenEnv() == kConfiguredAuthTokenEnv);
  CHECK(connection->connection.socketPath == "/tmp/config.sock");
  CHECK(connection->connection.authToken == "env-secret");
  CHECK(connection->socketPathSource == "config");
  CHECK(connection->authTokenSource == kAuthTokenEnv);
}

}  // namespace
}  // namespace scribe
