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
            ./src
            ./VERSION
            ./CMakeLists.txt
          ];
        };

        fcitx5-scribe = pkgs.callPackage (
          {
            stdenv,
            cmake,
            ninja,
            pkg-config,
            fcitx5,
            buildType ? "Release",
          }:
          stdenv.mkDerivation {
            pname = "fcitx5-scribe";
            inherit version;
            src = filteredSrc;

            nativeBuildInputs = [
              cmake
              ninja
              pkg-config
            ];

            buildInputs = [
              fcitx5
            ];

            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=${buildType}"
            ];

            # TODO: meta
          }
        ) { };

        fcitx5-with-scribe = pkgs.qt6Packages.fcitx5-with-addons.override {
          addons = [
            (fcitx5-scribe.override {
              buildType = "Debug";
            })
          ];
        };
      in
      {
        devShells.default = pkgs.mkShell {
          inputsFrom = [ fcitx5-scribe ];
        };

        packages = {
          default = fcitx5-scribe;
          inherit fcitx5-with-scribe;
        };
      }
    );
}
