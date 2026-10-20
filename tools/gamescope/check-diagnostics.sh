#!/usr/bin/env bash
set -euo pipefail
binary=${1:?usage: check-diagnostics.sh BINARY [--markers-only]}
mode=${2:-}
[[ -z "$mode" || "$mode" == --markers-only ]] || exit 2
options=(vk-enumerate-only vk-capabilities vk-create-device-test vk-submit-test
    vk-buffer-memory-test vk-image-memory-test vk-ahb-test vk-ahb-present-test
    vk-gamescope-renderer-init-test vk-gamescope-first-frame-test)
for option in "${options[@]}"; do
    grep -a -q -F -- "--$option" "$binary"
done
[[ "$mode" != --markers-only ]] || exit 0
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
"$binary" --help > "$temporary/help.log" 2>&1
for option in "${options[@]}"; do
    grep -q -F -- "--$option" "$temporary/help.log"
    set +e
    env VK_DRIVER_FILES="$temporary/missing-icd.json" VK_ICD_FILENAMES="$temporary/missing-icd.json" \
        VK_LOADER_LAYERS_DISABLE='*' "$binary" "--$option" > "$temporary/$option.log" 2>&1
    status=$?
    set -e
    if [[ "$status" != 1 ]]; then
        cat "$temporary/$option.log" >&2
        echo "--$option: missing-ICD diagnostic returned $status, expected 1" >&2
        exit 1
    fi
done
