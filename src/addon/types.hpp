#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace scribe {

using ContextId = uint64_t;

struct ConnectionConfig {
  std::string socketPath;
  std::string authToken;
  std::chrono::milliseconds connectTimeout{1000};
};

}  // namespace scribe
