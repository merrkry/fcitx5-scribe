# fcitx5-scribe

An experimental fcitx5 addon that provides building blocks for voice input engines on Linux.

This project is in an early WIP stage.

## Protocol

The protocol is intentionally `v0`: no compatibility guarantee in this stage.

Protocol behavior is documented in [docs/protocol.md](docs/protocol.md).

## Development

A Nix devShell is available for dependency management and debugging utilities.
Run `nix develop` to enter interactive devShell or use `nix develop -c <cmd>` to execute commands.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

This also generates `compile_commands.json` for LSP support.

### PoC Transcriber

A proof-of-concept implementation of transcription backend,
which doesn't actually transcribe but demonstrates the ability of committing text from an independent backend.

```sh
nix run .#fcitx5-with-scribe

nix develop -c protoc --python_out=tools --proto_path=src/proto src/proto/scribe/v0/scribe.proto
python tools/poc_transcriber.py
```

Nix devShell will configure the auth token and socket address.
