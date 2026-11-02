{
  # Pinned esptool for the softproxy hub flash host (yoga, SP-135; TIN-7428).
  # requirements.txt pins esptool==5.0.0, the version the fork's build and merge steps use.
  # nixpkgs never packaged 5.0.0 (4.9.0 went to 5.0.2), so this takes the 5.0.2 expression at a
  # locked nixos-unstable rev and swaps in the v5.0.0 source. No hosted flasher (flash-steps step 6).
  # Nothing here flashes: it only builds the tool. Usage: softproxy/flash/README.md.
  description = "esptool 5.0.0 pinned for the softproxy OpenShock hub flash (TIN-7428)";

  # nixos-unstable on 2025-09-12, the last channel rev before esptool 5.1.0 landed.
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
    in {
      packages = forAll (pkgs: rec {
        esptool = esptool500 pkgs;
        default = esptool;
      });
      devShells = forAll (pkgs: {
        default = pkgs.mkShell { packages = [ (esptool500 pkgs) ]; };
      });
    };
}
