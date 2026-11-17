# Softproxy fork: build, host test, backup and flash on yoga (TIN-7545; Jess, TIN-7125 c7cf5b6b: "keep / rely
# on yoga just dirven RBE / builds and exe on yoga"). Each recipe enters the pinned shell in flake.nix unless it
# already runs in one. SOFTPROXY.md "Build and flash on yoga" says which recipe runs when.

set shell := ["bash", "-euo", "pipefail", "-c"]

dev := if env_var_or_default("IN_NIX_SHELL", "") == "" { "nix develop --command" } else { "" }

# List the recipes.
default:
    @just --list

# Build <env> and merge its flash image as ci-build.yml does, into out/<env>/ with SHA256SUMS.
build env="Pishock-2023" out="":
    @echo "build: pio run -e {{env}}, buildfs, merge to out/{{env}}/OpenShock_{{env}}_<version>.bin"
    {{dev}} bash softproxy/just/build.sh {{env}} {{ if out == "" { "out/" + env } else { out } }}

# Build <env> twice into out/<env>.a and out/<env>.b and require identical SHA256SUMS.
build-twice env="Pishock-2023":
    @echo "build-twice: two clean builds of {{env}}, then compare SHA256SUMS"
    {{dev}} bash softproxy/just/build.sh {{env}} out/{{env}}.a
    {{dev}} bash softproxy/just/build.sh {{env}} out/{{env}}.b
    bash softproxy/just/compare.sh out/{{env}}.a out/{{env}}.b

# Run the native host tests that .github/workflows/softproxy-host-test.yml runs.
host-test:
    @echo "host-test: local lockdown, command path and radio path tests (g++ -std=c++20)"
    {{dev}} bash softproxy/just/host-test.sh

# Write out/<env>/OpenShock_<env>_*.bin at 0x0 with esptool 5.0.0. <go> is the TIN-7125 comment id of the GO.
flash env port go:
    @echo "flash: esptool write-flash 0x0 out/{{env}}/OpenShock_{{env}}_*.bin to {{port}}, GO TIN-7125 {{go}}"
    {{dev}} bash softproxy/just/flash.sh {{env}} {{port}} {{go}}

# Reset the hub on <port> and print its serial `version` reply at 115200 baud.
read-version port:
    @echo "read-version: reset the hub on {{port}}, send version, print the reply"
    {{dev}} python softproxy/just/read_version.py {{port}}

# Read the whole flash of the hub on <port> to <out> twice and require the two reads to match.
backup port out:
    @echo "backup: esptool read-flash 0 ALL from {{port}} to {{out}}, twice, then compare"
    {{dev}} bash softproxy/just/backup.sh {{port}} {{out}}
