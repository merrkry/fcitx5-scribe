{
  description = "An experimental fcitx5 addon that provides building blocks for voice input engines on Linux";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = pkgs.lib;
        version = lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);

        filteredSrc = lib.fileset.toSource {
          root = ./.;
          fileset = lib.fileset.unions [
            ./data
            ./src
            ./tests
            ./CMakeLists.txt
            ./VERSION
          ];
        };

        fcitx5-scribe = pkgs.callPackage (
          {
            boost,
            catch2_3,
            stdenv,
            cmake,
            ninja,
            pkg-config,
            fcitx5,
            openssl,
            protobuf,
            buildType ? "Release",
            runTests ? true,
          }:
          stdenv.mkDerivation {
            pname = "fcitx5-scribe";
            inherit version;
            src = filteredSrc;

            nativeBuildInputs = [
              cmake
              ninja
              pkg-config
              protobuf
            ];

            buildInputs = [
              boost
              fcitx5
              openssl
              protobuf
            ]
            ++ lib.optionals runTests [
              catch2_3
            ];

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=${buildType}"
              "-DBUILD_TESTING=${if runTests then "ON" else "OFF"}"
            ];

            doCheck = runTests;
            checkPhase = lib.optionalString runTests ''
              ctest --output-on-failure
            '';

            # TODO: meta
          }
        ) { };

        fcitx5-with-scribe = pkgs.qt6Packages.fcitx5-with-addons.override {
          addons = [
            (fcitx5-scribe.override {
              buildType = "Debug";
              runTests = false;
            })
          ];
        };
      in
      {
        devShells.default = pkgs.mkShell {
          inputsFrom = [ fcitx5-scribe ];
          packages = with pkgs; [
            catch2_3
            python3
            python3Packages.protobuf
          ];

          shellHook = ''
            export FCITX5_SCRIBE_DEBUG_SOCKET_PATH="$PWD/.cache/poc-transcriber.sock"
            export FCITX5_SCRIBE_DEBUG_AUTH_TOKEN_FILE="$PWD/.cache/scribe-auth-token"

            mkdir -p .cache
            if [ ! -s "$FCITX5_SCRIBE_DEBUG_AUTH_TOKEN_FILE" ]; then
              touch "$FCITX5_SCRIBE_DEBUG_AUTH_TOKEN_FILE"
              tr -dc 'a-zA-Z0-9' < /dev/urandom | head -c 16 > "$FCITX5_SCRIBE_DEBUG_AUTH_TOKEN_FILE"
            fi
            chmod 600 "$FCITX5_SCRIBE_DEBUG_AUTH_TOKEN_FILE"

            export FCITX5_SCRIBE_DEBUG_AUTH_TOKEN="$(cat "$FCITX5_SCRIBE_DEBUG_AUTH_TOKEN_FILE")"
          '';
        };

        packages = {
          default = fcitx5-scribe;
          inherit fcitx5-with-scribe;
        };
      }
    );
}
