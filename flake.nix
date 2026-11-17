{
  # Build and flash shell for the Softproxy fork on yoga (TIN-7545, TIN-7125 c7cf5b6b: builds and the flash run
  # on yoga through `just`, CI deferred). `just` recipes run inside `nix develop`, see justfile and SOFTPROXY.md.
  #
  # nixpkgs is the rev softproxy/flash pins on feat/tin-7428-esptool-pin, so esptool is the same 5.0.0 build
  # (nixpkgs never packaged 5.0.0: the 5.0.2 expression with the v5.0.0 source, upstream tests run in the build).
  # PlatformIO core 6.1.18 matches requirements.txt. PlatformIO downloads espressif32 6.12.0 (platformio.ini),
  # its toolchain and framework into PLATFORMIO_CORE_DIR on first run.
  description = "Softproxy OpenShock fork: pinned build, host test and flash shell for yoga (TIN-7545)";

  # nixos-unstable on 2025-09-12, the rev softproxy/flash/flake.nix pins (TIN-7428).
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/c8c766921bb153b97b0fc6eec6f0d04e53066f98";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});

      esptool500 = pkgs: pkgs.esptool.overridePythonAttrs (old: rec {
        version = "5.0.0";
        src = pkgs.fetchFromGitHub {
          owner = "espressif";
          repo = "esptool";
          tag = "v${version}"; # commit 785a3db318961001fff6a19fd728410ead681cc9
          hash = "sha256-2ZBaIQNZLKqNlRq3X2CnTpq4wxFWxw9NTtYHESKcsc8=";
        };
      });

      # One interpreter for PlatformIO, its SCons extra scripts and the chips/*/merge-image.py step. The extra
      # scripts import gitpython, fonttools, brotli and intelhex (scripts/install_dependencies.py) and would
      # pip-install them into a read-only store path when missing, so they are in the env. `python -m platformio`
      # keeps sys.executable on this env for the SCons child.
      pyEnv = pkgs: pkgs.python3.withPackages (ps: [
        (ps.toPythonModule pkgs.platformio-core)
        (ps.toPythonModule (esptool500 pkgs))
        ps.gitpython
        ps.fonttools
        ps.brotli
        ps.intelhex
        ps.pyserial
      ]);
    in {
      packages = forAll (pkgs: rec {
        esptool = esptool500 pkgs;
        python = pyEnv pkgs;
        default = python;
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [
            (pyEnv pkgs)
            (esptool500 pkgs)
            pkgs.nodejs_22
            pkgs.pnpm_10
            pkgs.git
            pkgs.just
            pkgs.gcc
            pkgs.coreutils
            pkgs.diffutils
            pkgs.findutils
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.bubblewrap ];
        };
      });
    };
}
