# Pinned esptool for the softproxy hub flash host

Linear: TIN-7428 (lane), TIN-7545 (fork). Host: yoga, the flash and capture host (Softproxy SP-135, TIN-7125 `5007cb07`).

This flake builds the esptool that `requirements.txt` pins (`esptool==5.0.0`), so the bench flash uses the same version as the fork's build and merge steps, from a locked nixpkgs rev and with no hosted flasher (Softproxy `docs/m3-t0/flash-steps.md` step 6). It only builds the tool. Nothing here flashes, reads or erases a device.

| Item | Value | Status |
| --- | --- | --- |
| esptool | 5.0.0, source tag `v5.0.0` (commit `785a3db318961001fff6a19fd728410ead681cc9`, NAR `sha256-2ZBaIQNZLKqNlRq3X2CnTpq4wxFWxw9NTtYHESKcsc8=`) | verified, `esptool version` on yoga, November 2, 2026 |
| nixpkgs | nixos-unstable `c8c766921bb153b97b0fc6eec6f0d04e53066f98` (2025-09-12), NAR `sha256-Ms8A878SzBb8dpIjNx9qBoQaOCgEiPSnbho48R3gNTU=`, in `flake.lock` | verified |
| Python | 3.13.7 from that rev | verified, build log |
| Store path on yoga (x86_64-linux) | `/nix/store/ggclh9mw9k5chcz13q7x89apj0qfqh6d-esptool-5.0.0`, NAR `sha256-LzBipk11Oilu9pkVGsHk5HNgRBnSelN8eXDEK/WP8MU=`, closure 207.8 MiB | verified, `nix path-info` on yoga |
| Upstream tests | 178 passed, 39 skipped (host tests) plus 2 HSM tests, run in the build | verified, build log |

nixpkgs never packaged 5.0.0 (it went from 4.9.0 to 5.0.2), so the flake takes the 5.0.2 expression at the locked rev and replaces the source with `v5.0.0`. The upstream test suite runs in the build, so a source that does not match the expression fails the build.

## Use on yoga

```sh
cd softproxy/flash
nix build .#esptool -o result-esptool   # the out link is a GC root
./result-esptool/bin/esptool version     # expect: esptool v5.0.0, 5.0.0
```

The hub enumerates as a USB serial device. yoga's user is in `dialout`, so the tool needs no root and no udev rule for `/dev/ttyUSB*` or `/dev/ttyACM*` (reported by the softproxy seat, check the device group on the day).

Which `esptool` commands run, and when, is set by `flash-steps.md` and the operator's flash GO at the attended session, not by this file. Every cap stays 0 (SP-9, SP-10).
