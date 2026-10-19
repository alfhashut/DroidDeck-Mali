#!/usr/bin/env bash
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
compiler=${CC:-aarch64-linux-gnu-gcc}
output=${1:-"$repo_root/app/src/main/assets/mali-vulkan/broker_probe"}
macros=$("$compiler" -dM -E - </dev/null)
if ! grep -q '__aarch64__' <<<"$macros"; then
    echo "CC must target AArch64 glibc (default: aarch64-linux-gnu-gcc)" >&2
    exit 1
fi
mkdir -p -- "$(dirname -- "$output")"
"$compiler" -std=c11 -O2 -Wall -Wextra -Werror -static \
    "$repo_root/tools/mali-vulkan/broker_probe.c" -o "$output"
printf 'Built static AArch64 glibc probe: %s\n' "$output"
