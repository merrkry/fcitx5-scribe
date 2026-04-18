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

- C++23. Follow modern C++ best practices, avoid legacy style.
- CMake (build should be executed in `build` directory)
- Nix flake for reproducible packaging and dev environment
- Run `clang-format -i` on relevant files before considering tasks completed.

## References

A source tree of fcitx5 is available at `./.ref/fcitx5`.
If missing, it could be shallow cloned from <https://github.com/fcitx/fcitx5>.
