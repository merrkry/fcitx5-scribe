#include "protocol.hpp"

#include <arpa/inet.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>
#include <expected>
#include <limits>
#include <string_view>
#include <vector>

namespace scribe {

Result<SessionId> generateSessionId() {
  SessionId sessionId;
  if (RAND_bytes(sessionId.data(), static_cast<int>(sessionId.size())) != 1) {
    return std::unexpected(
        makeError(ErrorKind::kOpenSsl, "RAND_bytes failed for session id"));
  }
  sessionId[6] = static_cast<uint8_t>((sessionId[6] & 0x0fU) | 0x40U);
  sessionId[8] = static_cast<uint8_t>((sessionId[8] & 0x3fU) | 0x80U);
  return sessionId;
}

Result<Nonce> generateNonce() {
  Nonce nonce;
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
    return std::unexpected(
        makeError(ErrorKind::kOpenSsl, "RAND_bytes failed for nonce"));
  }
  return nonce;
}

Result<std::vector<uint8_t>> computeServerProof(
    std::string_view token, std::span<const uint8_t, kNonceSize> clientNonce,
    std::span<const uint8_t, kNonceSize> serverNonce) {
  std::vector<uint8_t> message;
  message.reserve(kServerProofLabel.size() + clientNonce.size() +
                  serverNonce.size());
  message.insert(message.end(), kServerProofLabel.begin(),
                 kServerProofLabel.end());
  message.insert(message.end(), clientNonce.begin(), clientNonce.end());
  message.insert(message.end(), serverNonce.begin(), serverNonce.end());

  if (token.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return std::unexpected(
        makeError(ErrorKind::kOpenSsl, "token too large for HMAC"));
  }

  unsigned int digestLength = EVP_MAX_MD_SIZE;
  std::vector<uint8_t> digest(digestLength);
  if (HMAC(EVP_sha256(), token.data(), static_cast<int>(token.size()),
           message.data(), message.size(), digest.data(),
           &digestLength) == nullptr) {
    return std::unexpected(
        makeError(ErrorKind::kOpenSsl, "HMAC computation failed"));
  }
  digest.resize(digestLength);
  return digest;
}

Result<std::vector<uint8_t>> encodeFrame(const v0::Envelope& envelope) {
  const auto payloadSize = envelope.ByteSizeLong();
  if (payloadSize > kMaxFrameSize) {
    return std::unexpected(
        makeError(ErrorKind::kProtocolViolation, "protobuf frame too large"));
  }

  std::vector<uint8_t> frame(sizeof(uint32_t) + payloadSize);
  const auto networkSize = htonl(static_cast<uint32_t>(payloadSize));
  std::memcpy(frame.data(), &networkSize, sizeof(networkSize));
  if (!envelope.SerializeToArray(frame.data() + sizeof(networkSize),
                                 static_cast<int>(payloadSize))) {
    return std::unexpected(makeError(ErrorKind::kProtocolViolation,
                                     "failed to serialize protobuf envelope"));
  }

  return frame;
}

Result<v0::Envelope> decodeFrame(std::span<const uint8_t> frame) {
  v0::Envelope envelope;
  if (!envelope.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
    return std::unexpected(
        makeError(ErrorKind::kProtocolViolation, "invalid protobuf payload"));
  }

  return envelope;
}

}  // namespace scribe
