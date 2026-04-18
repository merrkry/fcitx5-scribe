#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "protocol.hpp"

namespace scribe {

// Formats a binary session id for logs and diagnostics.
//
// Parameters:
// - id: opaque session identifier bytes from the protocol layer.
std::string sessionIdToLogString(std::span<const uint8_t, kSessionIdSize> id);

// Copies input into output only when the lengths match exactly.
//
// Parameters:
// - input: byte string received from protobuf or another serialized source.
// - output: fixed-size destination buffer.
//
// Assumptions:
// - The byte contents are treated as opaque.
// - The caller needs exact length validation, not truncation.
bool copyExactBytes(std::string_view input, std::span<uint8_t> output) noexcept;

}  // namespace scribe
