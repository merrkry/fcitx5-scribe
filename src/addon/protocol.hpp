#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "error.hpp"
#include "scribe/v0/scribe.pb.h"

namespace scribe {

inline constexpr std::string_view kProtocolVersion = "scribe.v0";
inline constexpr std::string_view kClientName = "fcitx5-scribe";
inline constexpr std::string_view kServerProofLabel = "fcitx5-scribe/v0/server";
inline constexpr size_t kSessionIdSize = 16;
inline constexpr size_t kNonceSize = 32;
inline constexpr uint32_t kMaxFrameSize = 64 * 1024;

using SessionId = std::array<uint8_t, kSessionIdSize>;
using Nonce = std::array<uint8_t, kNonceSize>;

// Generates a random session identifier for one transcription session.
//
// Assumptions:
// - OpenSSL random generation is available.
// - Callers treat the bytes as opaque and unique within the active connection.
Result<SessionId> generateSessionId();

// Generates a random nonce for the handshake proof.
//
// Assumptions:
// - OpenSSL random generation is available.
// - The returned bytes are used once per handshake attempt.
Result<Nonce> generateNonce();

// Computes the server proof used to authenticate the transcriber after the
// transport connection is established.
//
// Parameters:
// - token: shared authentication secret from local configuration.
// - clientNonce: nonce previously sent by the addon in ClientHello.
// - serverNonce: nonce received from the transcriber in ServerHello.
//
// Assumptions:
// - The protocol version and proof label match on both peers.
// - Nonces are exactly kNonceSize bytes long.
Result<std::vector<uint8_t>> computeServerProof(
    std::string_view token, std::span<const uint8_t, kNonceSize> clientNonce,
    std::span<const uint8_t, kNonceSize> serverNonce);

// Serializes one protobuf envelope into the framed transport format:
// a 32-bit big-endian payload length followed by the protobuf bytes.
//
// Parameters:
// - envelope: logical message to send to the transcriber.
//
// Assumptions:
// - The encoded payload fits within kMaxFrameSize.
Result<std::vector<uint8_t>> encodeFrame(const v0::Envelope& envelope);

// Parses the protobuf payload of one already length-delimited transport frame.
//
// Parameters:
// - frame: raw protobuf payload bytes with the length prefix already removed.
//
// Assumptions:
// - The caller has already validated the outer frame length.
Result<v0::Envelope> decodeFrame(std::span<const uint8_t> frame);

}  // namespace scribe
