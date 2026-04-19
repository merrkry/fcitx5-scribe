# AGENTS.md

Fcitx5-scribe is an experimental fcitx5 addon that provides building blocks for voice input engines on Linux.

## Design

Fcitx5-scribe is a bridge between actual transcription provider ("transcripters") and fcitx5.

- Registers fcitx5 hotkeys as an alternative to Wayland hotkeys.
- Communicates with transcripter with authentication.
- Manages transcription sessions, sends session status update to transcripters.
- Receives transcription result from transcriptor.
- Shows preedit or commits text if feasible.

Audio handling and transcription implementation are out of scope.

The goal is to provide a provider-agnostics abstraction layer, so that providers don't have to workaround all the obstacles of Wayland protocols themselves.

Due to the nature of input methods, security considerations should be prioritized to reduce the risk of keystroke injections.

## Toolchain

- Follow modern C++ best practices and designs from Rust, avoid legacy styles.
- CMake build directory is `build`.
- All toolchain-related commands should be executed with `nix develop -c`.
- Format, build and run test suite before marking a task as completed.
  - For C++ files, use `clang-format -i` for formatting.
- Prefer using `cmake` directly for incremental builds. Avoid calling expensive `nix build` unless explicitly requested.

## References

A source tree of fcitx5 is available at `./.ref/fcitx5`.
If missing, it could be shallow cloned from <https://github.com/fcitx/fcitx5>.
