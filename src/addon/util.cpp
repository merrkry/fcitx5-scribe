#include "util.hpp"

#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace scribe {

std::string sessionIdToLogString(
    std::span<const uint8_t, kSessionIdSize> sessionId) {
  boost::uuids::uuid uuid;
  std::ranges::copy(sessionId, uuid.begin());
  return boost::uuids::to_string(uuid);
}

bool copyExactBytes(std::string_view input,
                    std::span<uint8_t> output) noexcept {
  if (input.size() != output.size()) {
    return false;
  }

  std::ranges::copy(input, output.begin());
  return true;
}

}  // namespace scribe
