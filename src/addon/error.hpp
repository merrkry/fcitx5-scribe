#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace scribe {

#define SCRIBE_ERROR_KIND_LIST(X)                  \
  X(kConfig, "config")                            \
  X(kOpenSsl, "openssl")                          \
  X(kProtocolViolation, "protocol violation")     \
  X(kTransport, "transport")                      \
  X(kState, "state")

enum class ErrorKind {
#define SCRIBE_ERROR_KIND_ENUM(name, label) name,
  SCRIBE_ERROR_KIND_LIST(SCRIBE_ERROR_KIND_ENUM)
#undef SCRIBE_ERROR_KIND_ENUM
};

struct Error {
  ErrorKind kind;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline Error makeError(ErrorKind kind, std::string message) {
  return Error{.kind = kind, .message = std::move(message)};
}

constexpr std::string_view errorKindName(ErrorKind kind) noexcept {
  switch (kind) {
#define SCRIBE_ERROR_KIND_CASE(name, label) \
  case ErrorKind::name:                     \
    return label;
    SCRIBE_ERROR_KIND_LIST(SCRIBE_ERROR_KIND_CASE)
#undef SCRIBE_ERROR_KIND_CASE
  }

  return "unknown error";
}

#undef SCRIBE_ERROR_KIND_LIST

}  // namespace scribe
