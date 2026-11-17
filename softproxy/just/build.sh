#!/usr/bin/env bash
# Build one PlatformIO env and merge its flash image the way .github/workflows/ci-build.yml does (TIN-7545).
# Usage: softproxy/just/build.sh <env> <out-dir>. Runs inside the flake's dev shell (see justfile).
#
# CI steps reproduced, in order:
#   build-frontend   pnpm install --frozen-lockfile --strict-peer-dependencies, pnpm run build (frontend/)
#   build-staticfs   pio run --target buildfs -e fs, with CI=true so build_frontend.py does not rebuild the frontend
#   build-firmware   pio run -e <env>
#   merge-partitions python scripts/merge_image.py <env> beside app.bin, bootloader.bin, partitions.bin and
#                    staticfs.bin, then merged.bin -> OpenShock_<env>_<version>.bin
# The version is the one scripts/embed_env_vars.py computes outside CI: the newest tag without a prerelease or
# build part, "-local", then "+" and the 7 character commit. A caller may export OPENSHOCK_FW_VERSION,
# OPENSHOCK_FW_GIT_REF, OPENSHOCK_FW_GIT_COMMIT and OPENSHOCK_FW_BUILD_DATE to give a CI run's values instead.
# To rebuild a CI run's static filesystem as well, also export SOFTPROXY_FRONTEND_VERSION_NAME (the value in that
# run's _app/version.json) and SOFTPROXY_SOURCE_DATE_EPOCH (the time in its gzip headers). SOFTPROXY.md has the
# November 17, 2026 proof against ci-build run 44024185983.
set -euo pipefail

env_name="${1:?usage: build.sh <env> <out-dir>}"
out_dir="${2:?usage: build.sh <env> <out-dir>}"
root="$(git rev-parse --show-toplevel)"
cd "$root"

say() { printf '[build %s] %s\n' "$env_name" "$*"; }

# Canonical paths. The Arduino core's log macros keep __FILE__, so the app image holds the absolute path of 19
# framework sources under PLATFORMIO_CORE_DIR, and a different home directory shifts every later byte. CI builds
# with the core at /home/runner/.platformio and the tree at /home/runner/work/openshock-firmware/openshock-firmware.
# On Linux the build runs in a bubblewrap mount namespace that shows the same two paths, so the image does not
# depend on the host user or the worktree path. SOFTPROXY_BUILD_SANDBOX=0 builds at the real paths instead.
ci_home=/home/runner
ci_tree="$ci_home/work/openshock-firmware/openshock-firmware"
case "$out_dir" in /*) echo "error: out dir must be relative to the repo root, got ${out_dir}" >&2; exit 2 ;; esac
if [ "${SOFTPROXY_IN_SANDBOX:-0}" != 1 ] && [ "${SOFTPROXY_BUILD_SANDBOX:-1}" = 1 ] && [ "$(uname -s)" = Linux ]; then
  common="$(cd "$(git rev-parse --git-common-dir)" && pwd)"
  pio_core="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
  mkdir -p "$pio_core" "$HOME/.cache" "$HOME/.local/share/pnpm"
  say "entering the bubblewrap namespace: tree at ${ci_tree}, PlatformIO core at ${ci_home}/.platformio"
  exec bwrap --dev-bind / / --tmpfs /home \
    --bind "$common" "$common" \
    --bind "$pio_core" "$ci_home/.platformio" \
    --bind "$HOME/.cache" "$ci_home/.cache" \
    --bind "$HOME/.local/share/pnpm" "$ci_home/.local/share/pnpm" \
    --bind "$root" "$ci_tree" \
    --setenv HOME "$ci_home" --setenv PLATFORMIO_CORE_DIR "$ci_home/.platformio" --setenv SOFTPROXY_IN_SANDBOX 1 \
    --chdir "$ci_tree" \
    bash softproxy/just/build.sh "$env_name" "$out_dir"
fi

if ! grep -q "^\[env:${env_name}\]" platformio.ini; then
  echo "error: no [env:${env_name}] in platformio.ini" >&2
  exit 2
fi

commit="$(git rev-parse HEAD)"
dirty=""
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then dirty="+dirty"; fi

if [ -z "${OPENSHOCK_FW_VERSION:-}" ]; then
  # Same rule as embed_env_vars.py sort_semver(): clean tags only, numeric sort, "v" stripped.
  latest_tag="$(git tag --list | grep -E '^v?[0-9]+(\.[0-9]+)*$' | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -n1)"
  OPENSHOCK_FW_VERSION="${latest_tag:-0.0.0}-local+${commit:0:7}"
fi
export OPENSHOCK_FW_VERSION
version="$OPENSHOCK_FW_VERSION"

# Reproducible outputs: one fixed time for the gzip headers that build_frontend.py writes and for the frontend's
# SvelteKit version name. The commit time of HEAD, so a rebuild of the same commit gives the same bytes. The nix
# shell sets SOURCE_DATE_EPOCH to 1980, so this overrides it. SOFTPROXY_SOURCE_DATE_EPOCH sets another time.
SOURCE_DATE_EPOCH="${SOFTPROXY_SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct HEAD)}"
export SOURCE_DATE_EPOCH
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"

say "commit ${commit}${dirty}, version ${version}, SOURCE_DATE_EPOCH ${SOURCE_DATE_EPOCH}"
say "tools: $(python --version 2>&1), PlatformIO $(python -m platformio --version | awk '{print $NF}'), esptool $(python -c 'import esptool; print(esptool.__version__)'), node $(node --version)"
say "PLATFORMIO_CORE_DIR ${PLATFORMIO_CORE_DIR}"

say "step 1/4: frontend (pnpm install --frozen-lockfile, pnpm run build)"
# CI=true as in the runner: pnpm never prompts (it asks before it purges node_modules of another tree path).
(cd frontend && CI=true pnpm install --frozen-lockfile --strict-peer-dependencies && CI=true pnpm run build)

say "step 2/4: static filesystem (pio run --target buildfs -e fs)"
rm -rf .pio/build/fs
CI=true python -m platformio run --target buildfs -e fs

say "step 3/4: firmware (pio run -e ${env_name})"
rm -rf ".pio/build/${env_name}"
CI=true python -m platformio run -e "$env_name"

say "step 4/4: merge (scripts/merge_image.py ${env_name})"
rm -rf "$out_dir"
mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
stage="$(mktemp -d "${TMPDIR:-/tmp}/openshock-merge.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
for p in scripts chips boards platformio.ini; do ln -s "$root/$p" "$stage/$p"; done
cp ".pio/build/${env_name}/app.bin" ".pio/build/${env_name}/bootloader.bin" ".pio/build/${env_name}/partitions.bin" "$stage/"
cp .pio/build/fs/littlefs.bin "$stage/staticfs.bin"
(cd "$stage" && python scripts/merge_image.py "$env_name")
merged="OpenShock_${env_name}_${version}.bin"
mv "$stage/merged.bin" "$out_dir/$merged"
cp "$stage/app.bin" "$stage/bootloader.bin" "$stage/partitions.bin" "$stage/staticfs.bin" "$out_dir/"

{
  echo "env ${env_name}"
  echo "commit ${commit}${dirty}"
  echo "version ${version}"
  echo "merged ${merged}"
  echo "source_date_epoch ${SOURCE_DATE_EPOCH}"
  echo "host $(hostname -s)"
} > "$out_dir/BUILDINFO"
(cd "$out_dir" && sha256sum -- *.bin | LC_ALL=C sort -k2 > SHA256SUMS)

say "done: ${out_dir}/${merged}"
sed 's/^/  /' "$out_dir/SHA256SUMS"
