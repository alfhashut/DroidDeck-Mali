#!/usr/bin/env bash
set -euo pipefail
repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
compiler=${CC:-aarch64-linux-gnu-gcc}
output=${1:-"$repo_root/app/src/main/assets/mali-vulkan"}
headers=${VULKAN_HEADERS:-/usr/include}
macros=$("$compiler" -dM -E - </dev/null)
if ! grep -q '__aarch64__' <<<"$macros"; then
    echo "CC must target AArch64 glibc (default: aarch64-linux-gnu-gcc)" >&2
    exit 1
fi
if [[ ! -f "$headers/vulkan/vk_icd.h" ]]; then
    echo "Install Vulkan headers (libvulkan-dev), or set VULKAN_HEADERS to their include directory" >&2
    exit 1
fi
mkdir -p -- "$output"
# Search host's architecture-independent Vulkan headers after glibc cross headers.
flags=(-std=c11 -O2 -Wall -Wextra -Werror -idirafter "$headers")
"$compiler" "${flags[@]}" -shared -fPIC -fvisibility=hidden -Wl,-z,defs \
    "$repo_root/tools/mali-vulkan/icd_proxy.c" -pthread -o "$output/libdroiddeck_mali_proxy.so"
"$compiler" "${flags[@]}" "$repo_root/tools/mali-vulkan/loader_test.c" -ldl -o "$output/vulkan_loader_test"
"$compiler" "${flags[@]}" "$repo_root/tools/mali-vulkan/capability_inventory.c" -o "$output/capability_inventory"
cp -- "$repo_root/tools/mali-vulkan/mali_proxy_icd.json" "$output/mali_proxy_icd.json"
printf 'Built AArch64 glibc diagnostic ICD and normal-loader test: %s\n' "$output"
