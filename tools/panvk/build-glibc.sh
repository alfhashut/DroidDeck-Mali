#!/usr/bin/env bash
# Heavy compilation is intentionally restricted to the manual GitHub Actions job.
set -euo pipefail
if [[ ${GITHUB_ACTIONS:-} != true || ${RUNNER_ARCH:-} != ARM64 || $(uname -m) != aarch64 ]]; then
    echo 'PanVK build requires the native ARM64 GitHub Actions job; do not build locally.' >&2
    exit 1
fi
if [[ $# != 3 ]]; then
    echo "usage: $0 SOURCE BUILD OUTPUT" >&2
    exit 2
fi
panvk_tools=$(cd -- "$(dirname -- "$0")" && pwd)
panvk_source=$(realpath -- "$1")
panvk_build=$(realpath -m -- "$2")
panvk_output=$(realpath -m -- "$3")
panvk_commit=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "$panvk_tools/source.json")
[[ $(git -C "$panvk_source" rev-parse HEAD) == "$panvk_commit" ]]
[[ -z $(git -C "$panvk_source" status --porcelain) ]]
[[ ! -e $panvk_build && ! -e $panvk_output ]]
git -C "$panvk_source" apply --check "$panvk_tools/patches/0001-android-only-liblog.patch"
git -C "$panvk_source" apply "$panvk_tools/patches/0001-android-only-liblog.patch"
export LC_ALL=C.UTF-8
export CC=gcc CXX=g++ LLVM_CONFIG=/usr/bin/llvm-config-19
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git -C "$panvk_source" show -s --format=%ct HEAD)
mapfile -t panvk_options < "$panvk_tools/meson-options.txt"
meson setup "$panvk_build" "$panvk_source" "${panvk_options[@]}"
# Build just the ICD and its generated manifest/dependencies, not all Mesa targets.
ninja -C "$panvk_build" -j 2 \
    src/panfrost/vulkan/libvulkan_panfrost.so \
    src/panfrost/vulkan/panfrost_icd.aarch64.json
python3 -B "$panvk_tools/package_glibc.py" \
    --source "$panvk_source" --build "$panvk_build" --output "$panvk_output"
