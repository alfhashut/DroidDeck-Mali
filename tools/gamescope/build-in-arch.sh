#!/usr/bin/bash
# Runs INSIDE an Arch Linux ARM container (menci/archlinuxarm:base-devel) on an arm64 runner.
# Builds Arch's own gamescope package of the runtime's version with the patches in ./patches
# added, then keeps only the gamescope binary, checks its library needs against the runtime's
# list, and packs it as gamescope.tzst (usr/local/bin/gamescope) for the apk to stage.
set -euxo pipefail
VERSION=${GAMESCOPE_VERSION:-3.16.29}
WORK=/work
cd "$WORK"
# pacman's download sandbox (Landlock + the alpm user) cannot be set up inside the runner's
# container; the packages still come signed from Arch Linux ARM's mirrors.
grep -q '^DisableSandbox' /etc/pacman.conf || sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf
# The geo-redirecting mirror answered 500s and connection resets mid-transaction; name a few
# concrete mirrors ahead of it so pacman has somewhere to fall over to.
{ for m in https://ca.us.mirror.archlinuxarm.org https://fl.us.mirror.archlinuxarm.org https://de3.mirror.archlinuxarm.org https://nl.mirror.archlinuxarm.org; do echo "Server = $m/\$arch/\$repo"; done; cat /etc/pacman.d/mirrorlist; } > /etc/pacman.d/mirrorlist.new
mv /etc/pacman.d/mirrorlist.new /etc/pacman.d/mirrorlist
pacman -Syu --noconfirm --needed git sudo zstd binutils
# makepkg refuses root; a builder user with passwordless sudo installs the dependencies.
id builder >/dev/null 2>&1 || useradd -m builder
echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder
rm -rf pkg && mkdir pkg && cp -r tools/gamescope/patches pkg/ && chown -R builder pkg
# Arch's packaging for exactly this version: the tag is <pkgver>-<pkgrel>; the first rel is tried
# first, later ones after it.
sudo -u builder bash -c "
set -euxo pipefail
cd pkg
for rel in 1 2 3 4; do
  if git clone -q --depth 1 --branch ${VERSION}-\$rel https://gitlab.archlinux.org/archlinux/packaging/packages/gamescope.git arch 2>/dev/null; then break; fi
done
test -f arch/PKGBUILD
cd arch
grep -q '^pkgver=${VERSION}$' PKGBUILD
cp ../patches/*.patch .
# Add our patches to the source list and apply them after Arch's own prepare() steps.
PATCHES=\$(ls *.patch | sort)
{ echo; echo 'source+=('; for p in \$PATCHES; do echo \"  \$p\"; done; echo ')'; for p in \$PATCHES; do echo \"sha256sums+=('SKIP')\"; done; } >> PKGBUILD
cat >> PKGBUILD <<'PREP'

_droiddeck_prepare() {
  # Arch's own prepare() leaves the shell inside the checkout; start from a known place.
  cd \"\$srcdir/gamescope\"
  for p in \$(ls \"\$srcdir\"/*.patch | sort); do
    echo \"applying \$(basename \"\$p\")\"
    patch -p1 --no-backup-if-mismatch < \"\$p\"
  done
}
if declare -f prepare >/dev/null; then
  eval \"\$(declare -f prepare | sed 's/^prepare ()/_arch_prepare ()/')\"
  prepare() { _arch_prepare; _droiddeck_prepare; }
else
  prepare() { _droiddeck_prepare; }
fi
PREP
makepkg -A -s --noconfirm --skipchecksums --skippgpcheck  # -A: the PKGBUILD lists x86_64 only; Arch Linux ARM builds the same file
ls -l *.pkg.tar.*
"
cd "$WORK"
rm -rf out && mkdir -p out/usr/local/bin
PKG=$(ls pkg/arch/gamescope-*.pkg.tar.* | grep -v -- '-debug-' | head -1)
tar --use-compress-program=unzstd -xf "$PKG" -C out --strip-components=2 usr/bin/gamescope
mv out/gamescope out/usr/local/bin/gamescope
chmod 755 out/usr/local/bin/gamescope
strip --strip-unneeded out/usr/local/bin/gamescope || true
# Check the real packaged executable, without depending on a GPU/Wayland server in CI.
# An explicitly missing ICD must fail at instance creation in the diagnostic path.
out/usr/local/bin/gamescope --help > out/gamescope-help.log 2>&1
grep -F -- '--vk-enumerate-only' out/gamescope-help.log
grep -F -- '--vk-capabilities' out/gamescope-help.log
grep -F -- '--vk-create-device-test' out/gamescope-help.log
grep -F -- '--vk-submit-test' out/gamescope-help.log
set +e
env VK_DRIVER_FILES="$WORK/out/missing-icd.json" VK_ICD_FILENAMES="$WORK/out/missing-icd.json" \
  VK_LOADER_LAYERS_DISABLE='*' out/usr/local/bin/gamescope --vk-enumerate-only > out/gamescope-enumeration.log 2>&1
ENUM_STATUS=$?
set -e
cat out/gamescope-enumeration.log
test "$ENUM_STATUS" -eq 1
grep -F 'gamescope: Vulkan enumeration-only (instance API 1.0, no extensions)' out/gamescope-enumeration.log
grep -F 'gamescope: enumeration-only vkCreateInstance failed:' out/gamescope-enumeration.log
set +e
env VK_DRIVER_FILES="$WORK/out/missing-icd.json" VK_ICD_FILENAMES="$WORK/out/missing-icd.json" \
  VK_LOADER_LAYERS_DISABLE='*' out/usr/local/bin/gamescope --vk-capabilities > out/gamescope-capabilities.log 2>&1
CAP_STATUS=$?
set -e
cat out/gamescope-capabilities.log
test "$CAP_STATUS" -eq 1
grep -F 'gamescope: capabilities vkCreateInstance failed:' out/gamescope-capabilities.log
set +e
env VK_DRIVER_FILES="$WORK/out/missing-icd.json" VK_ICD_FILENAMES="$WORK/out/missing-icd.json" \
  VK_LOADER_LAYERS_DISABLE='*' out/usr/local/bin/gamescope --vk-create-device-test > out/gamescope-device.log 2>&1
DEVICE_STATUS=$?
set -e
cat out/gamescope-device.log
test "$DEVICE_STATUS" -eq 1
grep -F 'gamescope: device test vkCreateInstance failed:' out/gamescope-device.log
set +e
env VK_DRIVER_FILES="$WORK/out/missing-icd.json" VK_ICD_FILENAMES="$WORK/out/missing-icd.json" \
  VK_LOADER_LAYERS_DISABLE='*' out/usr/local/bin/gamescope --vk-submit-test > out/gamescope-submit.log 2>&1
SUBMIT_STATUS=$?
set -e
cat out/gamescope-submit.log
test "$SUBMIT_STATUS" -eq 1
grep -F 'gamescope: submit test vkCreateInstance failed:' out/gamescope-submit.log
# All memory and renderer paths must be in this packaged executable and exit before startup.
for diagnostic in vk-buffer-memory-test vk-image-memory-test vk-ahb-test vk-ahb-present-test vk-gamescope-renderer-init-test vk-gamescope-first-frame-test mali-session-test mali-wayland-client-test mali-wayland-session mali-interactive-client; do
  grep -F -- "--$diagnostic" out/gamescope-help.log
  set +e
  env VK_DRIVER_FILES="$WORK/out/missing-icd.json" VK_ICD_FILENAMES="$WORK/out/missing-icd.json" \
    VK_LOADER_LAYERS_DISABLE='*' out/usr/local/bin/gamescope "--$diagnostic" > "out/gamescope-$diagnostic.log" 2>&1
  INTEROP_STATUS=$?
  set -e
  cat "out/gamescope-$diagnostic.log"
  test "$INTEROP_STATUS" -eq 1
  grep -E 'vkCreateInstance.*VkResult[=:]' "out/gamescope-$diagnostic.log"
done
# Every NEEDED library must be one the runtime ships, or the binary would not load there.
NEEDED=$(readelf -d out/usr/local/bin/gamescope | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
echo "NEEDED: $NEEDED"
MISSING=""
for lib in $NEEDED; do grep -qx "$lib" tools/gamescope/runtime-sonames.txt || MISSING="$MISSING $lib"; done
if [ -n "$MISSING" ]; then echo "ERROR: not in the runtime:$MISSING"; exit 3; fi
(cd out && tar --use-compress-program='zstd -19' -cf ../gamescope.tzst usr)
sha256sum gamescope.tzst | tee gamescope.tzst.sha256
ls -l gamescope.tzst out/usr/local/bin/gamescope
