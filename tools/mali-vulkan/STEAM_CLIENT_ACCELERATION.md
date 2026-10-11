# C8B.1: client graphics audit

Status: **Phase A preserved; Phase A2 ANGLE audit and standalone phone probe;
Phase B0 native build reported successful; portable packaging deferred in favor
of a temporary raw-test CI artifact defined below. Hardware client acceleration
is not phone-proven or enabled for Steam.** C8A's working software client remains
the fallback and current behavior. No Gamescope, proxy, broker, compositor,
session or APK packaging changes.

## Decision

Native AArch64 glibc PanVK/kbase is plausible; the audited backend is not
inherently Bionic-only. However, the candidate revision is **not ready to adopt
as a truthful, supported Zink client stack**. Do not stage its Android release
binary, enable its compatibility feature flags, or switch Steam to it.

The concrete capability gate is `alphaToOne = false` in the candidate's feature
table, whereas Mesa documents it as a minimum Zink OpenGL 2.1 requirement.
Additional advertised features and external-memory behavior need verification
before adopting this fork. This is not a claim that glibc compilation is
impossible, nor that a narrowly limited hardware draw could never work.

Phase B (native VkDevice plus accelerated GL context/visible draw) and Phase C
(Steam selection) remain gated. No hardware renderer or FPS success is claimed.

## Reproducible candidate and sources

Audited on 2026-10-11, using small source downloads only:

- [FristOneRR source](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source),
  commit `efd07bab401b263a9ac114560ecdb9a2934f5d43`.
- [Release build options](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/tools/fristonerr/build-options.txt)
  and [Android cross file](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/android-cross.txt).
- [Native Linux CI recipe](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/.github/workflows/kbase-release.yml)
  and [kbase documentation](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/docs/panvk-kbase.md).
- [Zink requirements](https://docs.mesa3d.org/drivers/zink.html).

The repository contains inherited documentation/recipes for CSF GPUs alongside
this revision's JM build. The documentation's G710/G925 and performance claims
are not evidence for this phone's G52/JM. Likewise, the presence of a Linux CI
recipe does not prove that this exact revision builds or works on this phone.

## Current production environment

`SessionService.runSession()` selects `maliSteam`, starts `MaliNormalBroker`,
then calls `addClientEnvironment()` and appends the existing proxy environment.
`LinuxRuntime.command()` runs `droiddeck-session steam` under proot. The script's
outer Mali branch starts Gamescope with `--mali-wayland-session --mali-xwayland`
and re-enters the same script as its client.

Patch `0124-mali-steam-xwayland-client.patch`, `RunMaliWaylandSession()`'s child
branch, removes `VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, `BL_VK_DRIVER`, broker
socket, normal-session and renderer/AHB diagnostic selectors **after fork**.
It then forces:

```text
LIBGL_ALWAYS_SOFTWARE=1
MESA_LOADER_DRIVER_OVERRIDE=swrast
GALLIUM_DRIVER=llvmpipe
```

The script selects `-cef-use-gl=angle -cef-use-angle=gl` for this client.
This explains the phone's ANGLE/llvmpipe renderer. Merely removing those variables
would not install a hardware driver or give the client a usable Vulkan stack.

Gamescope's parent retains its proxy ICD and broker environment. The non-Mali
service branch separately selects Zink/Turnip and `LIBGL_KOPPER_DRI2=true`; that
configuration is not applied to Mali Steam. Arbitrary extra environment entries
are suppressed for Mali Steam, apart from the existing explicit profiling
overrides. A global client ICD export would break the required isolation.

## Runtime and build inventory

| Component | Source evidence | What remains unknown |
| --- | --- | --- |
| GL/EGL/GLX | `runtime-sonames.txt` lists GLVND `libGL`, `libEGL`, `libGLX`, Mesa EGL/GLX vendors | Actual installed files/search paths in this phone's runtime |
| Gallium/LLVM | Inventory lists `libgallium-26.2.3-arch1.1.so`, LLVM 22.1 | Exact built-in drivers and module entrypoints |
| Zink | Runtime catalog describes Mesa 26.2 with Zink and KGSL Turnip | Installed `zink_dri.so`/Kopper pairing; CEF's effective library resolution |
| Software GL | Script requires `/usr/lib/dri/swrast_dri.so`; phone reports llvmpipe | No doubt about the active software renderer; aliases/package metadata still need inventory |
| Vulkan | Inventory lists `libvulkan.so.1.4.357` and `libvulkan_freedreno.so` | No source evidence of an installed kbase PanVK ICD |

There is no unpacked phone rootfs in this checkout. The soname list is a build
link-compatibility inventory, not a complete rootfs file list. Do not infer a
usable PanVK or Zink context from a shared-library name alone.

`LinuxRuntimeInstaller.CATALOG_URL` points to the external winlator-contents
`linuxfs.json`. At audit time it selects r9, a 790,907,606-byte archive, SHA256
`d3240c1531531615b0dd06b6ad56e92db9015a8641eb18c3a613835ae45992e7`.
The catalog says Arch Linux ARM/Mesa 26.2/Zink/Turnip and attributes the rootfs
build to Bannerlator. The archive was not downloaded or unpacked for this audit;
the phone's installed version is not established by the current catalog.

This checkout's `.github/workflows/build.yml` builds Gamescope on the Mali branch
and stages helpers/overlays into APK assets. `build-gamescope.yml` separately
publishes the patched Gamescope component. Neither builds the hosted rootfs or
a client PanVK/Mesa stack. **No Actions changes were made:** automatic packaging
of this unvalidated candidate would bypass the audit gate.

## Native glibc and device access

The Android release recipe targets `aarch64-linux-android35`, enables only the
Android platform, and disables GLX/EGL. Those binaries are not a drop-in glibc
client graphics stack.

In contrast, the Linux recipe configures `platforms=x11` and builds a native
Vulkan ICD with distro libdrm, X11/XCB, xshmfence, LLVM/libclc and host shader
compiler tools. `src/panfrost/lib/kmod/kbase_kmod.c` uses Linux ioctl/mmap/poll,
pthread and device-node APIs. `panvk_android.h` supplies non-Android inline stubs;
`panvk_android.c` is included only for the Android platform. No proprietary
`libGLES_mali.so`, Android vendor Vulkan library or Bionic bridge is required by
that audited native path. An actual glibc build/link/dependency audit is still
necessary in CI; it was not run locally.

PanVK's Meson file builds JM architectures 6/7/9 in this revision.
`panvk_enumerate_physical_devices()` explicitly scans `/dev/maliN`, and
`create_kmod_dev_kbase()` opens the node `O_RDWR | O_CLOEXEC`, bypassing DRM device
creation via `pan_kmod_dev_create_with_driver(..., "kbase", ...)`.

`LinuxRuntime.binds()` binds `/dev`, `/proc` and `/sys`. Proot does not hide
`/dev/mali0` and does not turn it into a DRM device. The guest uses the Android
app's kernel credentials; proot UID emulation grants no kernel/SELinux access.
Direct open and kbase version/property/job-submission compatibility therefore
require phone proof. The working Bionic broker is encouraging evidence, not
proof that the guest driver accepts the same kernel ABI or mmap behavior.

## Capability and presentation gates

At the pinned revision:

1. `panvk_vX_physical_device.c:348` reports `alphaToOne = false`. Mesa's minimum
   Zink GL 2.1 list requires it (as well as logicOp, fillModeNonSolid and
   shaderClipDistance). A context that happens to start is not proof of correct
   required GL behavior. Do not flip this bit to pass selection.
2. `panvk_instance.c:405-409` has a constructor enabling
   `PANVK_DXVK_COMPAT=1` unless explicitly overridden. The feature table uses it
   for geometryShader, multiViewport, shaderClipDistance/shaderCullDistance and
   additional BC format reporting. This automatic compatibility mode is not
   implementation evidence. With it explicitly disabled, shaderClipDistance is
   reported false, introducing another documented minimum Zink gap. Do not
   enable it as a workaround for the missing capabilities.
3. `panvk_vX_physical_device.c:337-344` enables fillModeNonSolid with a comment
   about avoiding Zink's selection rejection. Audited JM draw code translates
   primitive topology; the inspected draw/shader files do not establish complete
   polygon-mode lowering. Treat that feature's correctness as unproven, not as
   verified support. Logic-op blend handling does exist in `panvk_vX_blend.c`;
   vendor Vulkan feature bits cannot simply be applied to this separate driver.
4. `kbase_kmod_bo_export_fd():1965-1989` creates a standalone, sized memfd when
   a BO lacks a dma-buf. The function does not copy or alias the GPU BO into it;
   its own comment acknowledges incorrect sharing is possible. Do not treat
   successful FD export as proof of shared GPU storage. Real dma-heap-backed
   allocation/import is a separate path, requiring device access and proof.

The existing Xwayland is deliberately started with `XWAYLAND_NO_GLAMOR=1`,
disabling glamor and DRI3. PanVK's fast raw-FD route requires dma-heap allocation
and a private Termux:X11/Winlator DRI3 modifier (`1274`); this Xwayland/session
does not provide that transport. Enabling it through an environment variable
cannot supply the missing server implementation.

**DRI3 absence alone does not rule out hardware rasterization.**
`panvk_wsi_init()` uses `sw_device` for the kbase CPU presentation fallback,
without making the Vulkan device a CPU rasterizer. Its X11 WSI permits this
fallback without DRI3. In this revision, MIT-SHM selection itself is conditioned
on DRI3/Present support; with current Xwayland it can fall back further to
`xcb_put_image`. That implies client GPU-to-CPU presentation/copy overhead even
if Zink rendering works. Gamescope's existing SHM input and zero-copy Android
AHB output would remain separate and unchanged.

Kopper has a no-DRM-fd Vulkan discovery path. An appropriate native Mesa
GLX/EGL/Zink/Kopper build could therefore be investigated with CPU X11
presentation, but the exact installed Mesa pairing, capabilities and real GL
context must pass a probe. A Vulkan enumeration success or GL renderer string
alone is insufficient. GLX pixmap/popup fixes and resolution changes remain out
of scope.

## Next safe validation steps

First inventory the **installed guest**, inside the existing proot environment,
using its actual client `DISPLAY` (not Android shell's environment):

```sh
cat /.version
printf 'DISPLAY=%s XDG_RUNTIME_DIR=%s\n' "$DISPLAY" "$XDG_RUNTIME_DIR"
ls -l /dev/mali0 /usr/lib/dri /usr/share/vulkan/icd.d
ls -l /usr/lib/libGL.so* /usr/lib/libEGL.so* /usr/lib/libGLX.so* \
  /usr/lib/libgallium* /usr/lib/libvulkan.so*
pacman -Q mesa vulkan-icd-loader
```

Missing files/packages are useful failures to record, not reasons to download
replacement graphics libraries automatically. Node existence/readability does
not prove that `open(O_RDWR)`, ioctls or GPU jobs work. No phone commands were
executed during this audit.

Before Phase B, obtain a pinned driver revision with truthful required features
and safe external-memory behavior, or narrow the proposal to a separately
reviewed client implementation that does not require the missing features.
If proceeding with native PanVK, build an **isolated client artifact in CI**,
not a rootfs/global Mesa replacement. Audit ELF dependencies for glibc rather
than Android/Bionic libraries. Keep its GL/EGL/GLX/Zink modules ABI-matched and
separate from Gamescope's libraries and proxy ICD.

The eventual Phase B executable must run as the Xwayland client **instead of
Steam**, after the existing fork/environment isolation boundary. It must select
only the candidate client ICD, create a real Vulkan device, verify actual
required capabilities, create GLX/EGL context, draw a visible test and report
ICD/device/vendor/renderer/version, direct-rendering status, DISPLAY/socket and
failure exit status. Reject llvmpipe/softpipe/swrast/LLVMpipe and capability
failures. `vulkaninfo --summary`, `glxinfo -B` and `eglinfo` are useful supporting
commands once installed, but do not replace that device-plus-visible-draw test.
The original Phase A added no Phase B launcher or PASS-producing probe. Phase
A2 below adds a narrower ANGLE-only probe, without adopting Zink.

Only after that phone proof may a client-scoped selector replace the software
environment. Then test at the manually selected 1280x800: Gamescope normal
startup, zero-copy AHB output, Steam/CEF/login or Big Picture, real hardware
renderer in `webhelper_gpu.txt` (GPU0/GL_VENDOR/GL_RENDERER/ANGLE/backend/features),
UI performance against C8A, correct pixels, clean Stop and second launch. Keep
software selection available; a fallback is never an accelerated PASS.

## Client-only alternatives; not implemented

| Route | Potential benefit | Required work / blocker |
| --- | --- | --- |
| Native glibc PanVK + Zink | No Bionic graphics bridge; direct kbase backend | Resolve truthful Zink feature coverage, memory sharing and guest kernel compatibility; prove X11 CPU presentation first |
| ANGLE Vulkan with dedicated client WSI | Avoids desktop Zink's complete minimum feature set | Independently audit ANGLE's real feature requirements; current proxy has no client graphics-pipeline/draw/swapchain implementation; dedicated WSI/resource/sync path needed |
| glibc-to-Bionic EGL/GLES bridge | Could use vendor GLES without shipping proprietary libraries | Cross-libc ABI/object/thread/library isolation plus X11 surface/pixmap transport and synchronization; not a library symlink or LD_LIBRARY_PATH change |

Neither alternative is implemented or selected. Both require a separate
architecture decision and authorization before substantial bridge work. The
existing Vulkan protocol, 60 Hz pacing, staging/registry/upload optimizations,
producer wait, AHB/SYNC_FD ownership, Android releases, input and non-Mali launch
behavior remain untouched.

## Phase A2: bundled ANGLE Vulkan over client-only PanVK

**Decision: sufficient source evidence for a limited standalone GLES 2 draw
experiment; insufficient evidence to enable Steam acceleration.** ANGLE does
not inherit Zink's `alphaToOne` requirement. A pbuffer plus explicit CPU pixel
copy avoids this fork's unsafe external-memory fallback. This proves neither
the complete driver nor Steam/CEF UI compatibility. No alternative Bionic
EGL/GLES bridge is implemented.

### Installed versions and selection evidence

The user supplied these phone version/command-line lines on 2026-10-11:

| Component | Phone version |
| --- | --- |
| Steam client / webhelper build | `1791592380` |
| Chromium / CEF | `Chrome/126.0.6478.183` |
| ANGLE | `5d4df51d1d7d` |
| Skia | `Skia/126 be621ea04206d8fae23952783d1d588d6ce0d9b3` |
| webhelper executable | `/root/.local/share/Steam/steamrtarm64/steamwebhelper` |

The supplied webhelper and GPU-child argv contain no `--use-gl=angle` or
`--use-angle=vulkan`. They include `--no-sandbox`; the parent includes
`--ignore-gpu-blocklist`, and the child includes `--type=gpu-process` and
`--enable-chrome-runtime`. Combined with the earlier phone renderer report,
the current client is ANGLE/OpenGL/llvmpipe. Missing switches alone do not prove
that CEF cannot select a backend through embedder settings.

The matching public ANGLE revision resolves to
`5d4df51d1d7d6a290d54111527a4798f10c7ca3c`. This audit uses that revision and
Chromium tag `126.0.6478.183`, not current mainline. Valve's embedded patches,
bundled build options and library hashes are not present in this checkout.
The short ANGLE version identifies the public source baseline, not every
Valve modification or which optional backends were compiled.

The current `droiddeck-session` has Steam-specific
`-cef-use-gl=angle -cef-use-angle=gl` in its Mali branch. The non-Mali branch's
`-cef-use-angle=vulkan` is evidence of an existing launch convention, not proof
of what this installed Valve build forwards. In public Chromium 126:

- [`gl_implementation.cc`](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/gl/gl_implementation.cc)
  recognizes ANGLE Vulkan selection through `use-gl` / `use-angle` and can infer
  ANGLE when only `use-angle` is specified.
- [`gpu_process_host.cc`](https://github.com/chromium/chromium/blob/126.0.6478.183/content/browser/gpu/gpu_process_host.cc)
  copies whitelisted switches, then invokes the embedder's
  `AppendExtraCommandLineSwitches`. The
  [`GL switch list`](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/gl/gl_switches.cc)
  includes `kUseANGLE`; `kUseGL` is not in that list. Do not assume every parent
  argument is copied to the GPU child.
- The proprietary Steam-to-CEF `-cef-*` translation/filtering cannot be proven
  from those public sources. Real GPU-child argv, `cef_log.txt` and
  `webhelper_gpu.txt` must confirm any eventual Steam experiment. This change
  does not change the launch arguments or claim they have been accepted.

### Exact ANGLE requirements versus candidate PanVK

Primary sources are matching ANGLE
[`vk_renderer.cpp`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/libANGLE/renderer/vulkan/vk_renderer.cpp),
[`vk_caps_utils.cpp`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/libANGLE/renderer/vulkan/vk_caps_utils.cpp),
and pinned PanVK
[`panvk_vX_physical_device.c`](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/vulkan/panvk_vX_physical_device.c).
These are client-driver requirements; Android's vendor Vulkan 1.1.131 remains
independent and unchanged.

| Requirement / capability | Matching ANGLE behavior | Pinned PanVK / conclusion |
| --- | --- | --- |
| Vulkan instance and physical-device API | Minimum **1.1**, checked in renderer initialization | JM source reports 1.3; experimental, not conformance proof. Probe queries actual device, does not override versions. |
| Queue | One family with both graphics and compute; nonzero queue count | JM queue advertises graphics/compute/transfer. Actual device creation and jobs must succeed on phone. |
| Memory | Nonzero memory types; host-visible staging, including host-visible **cached** types; device-local allocation | Candidate supplies coherent and cached noncoherent host memory with cache-maintenance paths. Runtime allocation/mapping/cache correctness remains a phone test. |
| Linux XCB instance extensions | `VK_KHR_surface`, `VK_KHR_xcb_surface` for this display implementation | Requires a native X11-enabled PanVK build. Android release ICD is unsuitable. |
| XCB device extension | `VK_KHR_swapchain` when display chooses WSI | Required even for a pbuffer using ANGLE's XCB display. Probe creates no Vulkan swapchain; ANGLE initialization still verifies this extension. |
| Formats / limits | Supported renderable BGRA8 XCB configs, sampled/color and depth/stencil format capabilities, Vulkan limits determine actual EGL/GLES caps | Must query actual context and render. Source feature tables alone do not establish compatible pixels. Probe selects RGBA8 pbuffer and reads it back. |
| `alphaToOne` | Used conditionally for `EXT_multisample_compatibility`, **not a minimum ANGLE Vulkan GLES 2 gate** | False is acceptable for this limited test; original Zink blocker remains. |
| `logicOp`, `fillModeNonSolid`, geometry/tessellation/clip features | Enabled from actual reported bits for corresponding optional GLES extensions; not universal initialization prerequisites | No compatibility flags or forced feature reporting. Candidate's suspicious polygon-mode reporting remains unverified; triangle probe does not test polygon mode. |
| GLES 3 transform feedback | Requires native transform-feedback extension/features **or** `vertexPipelineStoresAndAtomics` emulation | G52/arch 7 has no native transform-feedback extension; stores/atomics require a force option outside normal support. With truthful defaults, expect ANGLE capped to GLES 2. Do not force the bit. |
| Other GLES 3 limits | Standard sample locations, independent blend, usable uniform-buffer/vertex-output limits; additional limits for 3.1 | Runtime caps matter. A GLES 2 success does not prove CEF/Skia's required context or texture-sharing operations. |
| External memory/semaphore/fence FD extensions | Optional feature-dependent integration paths, not necessary for this pbuffer/readback proof | Do not exercise BO export; unrelated memfd fallback remains invalid sharing. |

Optional instance extensions include surface-capabilities2, surface maintenance,
swapchain color space and validation/debug utilities. Device features/extensions
are enumerated and gated by availability and driver workarounds in
`queryDeviceExtensionFeatures`, `initFeatures`, and the `enableDeviceExtensions*`
functions. Timeline/synchronization2, extended dynamic state, renderpass2,
depth/stencil resolve, float16, storage8, transform feedback and external FD
support are not one unconditional GLES 2 prerequisite list. Higher API core
versions can supply promoted functionality. Do not copy Zink's minimum list or
the compositor proxy's capability restrictions onto this separate driver.

`getMaxSupportedESVersion()` explicitly downgrades without required capabilities;
the test never enables `exposeNonConformantExtensionsAndVersions`. It requests
GLES 2 deliberately. This is a limited feasibility test, not a way to claim
missing GLES 3 semantics. No hard Steam/CEF GLES 3 requirement was established
from the inspected Chromium 126 initialization code; that application's real
requirements remain to be tested after the standalone proof.

PanVK's matching
[`panvk_drirc_gen.py`](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/vulkan/panvk_drirc_gen.py)
defaults `pan_force_enable_shader_atomics` and
`pan_enable_vertex_pipeline_stores_atomics` to false. The probe explicitly keeps
both false rather than inheriting a debug override. Existing drirc files and
the CI build configuration still require audit; it does not certify the fork's
entire advertised feature table.

### Loader and client isolation

Matching ANGLE
[`libvulkan_loader.cpp`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/common/vulkan/libvulkan_loader.cpp)
tries Linux `libvulkan.so`, then `libvulkan.so.1`. A custom-loader build searches
its module directory; the usual build uses the bare-name dynamic-loader search.
Steam's existing LD_LIBRARY_PATH includes `steamrtarm64` and its `panorama`
directory before inherited paths. Therefore source inspection cannot establish
which installed loader wins. Obtain the actual `/proc/<gpu-pid>/maps` path and
audit that loader's ELF dependencies; a library name is not sufficient.

ANGLE's
[`vulkan_icd.cpp`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/common/vulkan/vulkan_icd.cpp)
replaces ICD selection for its mock/SwiftShader modes, while the default
hardware mode leaves the selected ICD environment intact. The standalone probe
sets both `VK_DRIVER_FILES` and `VK_ICD_FILENAMES` to one canonical manifest in
its own exec child only. An older loader can use the latter. It verifies the
actual PanVK shared object is mapped, and queries the **same VkInstance,
VkPhysicalDevice and VkDevice used by ANGLE**, through
[`EGL_ANGLE_device_vulkan / DeviceVk`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/libANGLE/renderer/vulkan/DeviceVk.cpp).
It does not substitute a separately enumerated Vulkan device as proof.

Run only after the existing Gamescope client fork has cleared the compositor
broker/ICD selectors. The probe rejects a compositor broker/session environment,
clears inherited software/version/ANGLE feature overrides and interposers,
then sets `PANVK_DXVK_COMPAT=0` and `PANVK_KBASE_DRI3=0`. It does not modify the
parent environment, Gamescope manifest or production launcher. Native PanVK
still requires real `/dev/mali0` access, compatible kbase ioctls and successful
GPU jobs; proot cannot grant permissions denied to the app uid.

### X11 presentation and the no-DRI3 distinction

ANGLE's matching
[`DisplayVkXcb`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/libANGLE/renderer/vulkan/linux/xcb/DisplayVkXcb.cpp)
connects to the existing DISPLAY; its
[`WindowSurfaceVkXcb`](https://github.com/google/angle/blob/5d4df51d1d7d6a290d54111527a4798f10c7ca3c/src/libANGLE/renderer/vulkan/linux/xcb/WindowSurfaceVkXcb.cpp)
uses `vkCreateXcbSurfaceKHR` and the common Vulkan swapchain implementation.
There is no DRI3 test in ANGLE's display connection itself.

The candidate
[`panvk_wsi.c`](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/vulkan/panvk_wsi.c)
selects kbase CPU WSI by default. Mesa's
[`wsi_common_x11.c`](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/vulkan/wsi/wsi_common_x11.c)
allows that path without DRI3. Current no-glamor Xwayland likely uses ordinary
XPutImage rather than the DRI3-conditioned MIT-SHM optimization. Missing private
raw DRI3 therefore blocks that fast sharing path, not every possible GPU draw
with CPU presentation. These remain source-derived possibilities, not measured
phone WSI success.

Chromium adds separate policy. Its X11 Vulkan-support helper tests DRI3, ATI or
NVIDIA extensions. However, in matching
[`gl_display.cc`](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/gl/gl_display.cc),
the scoped DISPLAY override ends **before** `eglInitialize`; that helper alone
does not prove ANGLE cannot initialize XCB. The separate
[`VulkanImplementationX11`](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/ozone/platform/x11/vulkan_implementation_x11.cc)
does gate Vulkan view surfaces. Do not conflate Chromium's own Vulkan/Skia
integration with ANGLE's Vulkan backend. Do not add Skia-Vulkan flags for this
proof. Also, Chromium's
[`X11SurfaceFactory`](https://github.com/chromium/chromium/blob/126.0.6478.183/ui/ozone/platform/x11/x11_surface_factory.cc)
chooses its automatic EGL readback surface for SwiftShader, not arbitrary
hardware ANGLE. A successful manual CPU-copy test does not establish automatic
CEF fallback or native swapchain presentation.

### Standalone Phase B experiment, not a Steam selector

Added `probe_steam_angle.py`; it is **not packaged or called by production**.
It uses Python ctypes so no probe compilation is required. Its phone inputs
are the installed Steam `libEGL.so` / `libGLESv2.so` pair and a separately built,
pinned native glibc PanVK ICD. ELF64/AArch64 validation does not establish glibc
ABI compatibility: inspect `readelf -d` dependencies of the CI artifact before
installation. Do not use the Android release artifact or replace rootfs Mesa.

First locate and hash the actual installed Steam libraries; their directories
were not supplied with the phone version lines. Then run as the client command
of the **existing** Gamescope `--mali-wayland-session --mali-xwayland -- <client>`
invocation inside the existing Linux runtime, replacing Steam only for this
manual experiment. Preserve its parent proxy environment and outer Wayland
connection. The following is the client command, **not** a new launcher:

```sh
python3 -B /path/to/probe_steam_angle.py \
  --icd /path/to/client-panvk/panvk.json \
  --panvk-library /path/to/client-panvk/libvulkan_panfrost_kbase.so \
  --egl /root/.local/share/Steam/ACTUAL_PAIR_DIRECTORY/libEGL.so \
  --gles /root/.local/share/Steam/ACTUAL_PAIR_DIRECTORY/libGLESv2.so
```

The placeholders require actual installed paths; no binaries are downloaded or
installed by the probe. It must inherit the existing client DISPLAY/socket.
The normal Launch button remains the existing software Steam path.

The probe requires the bundled ANGLE Vulkan client extension, selects that
backend explicitly, creates an RGBA8 GLES 2 pbuffer, and queries ANGLE's actual
Vulkan handles. It checks ARM vendor `0x13b5`, Mali-G52 hardware, API >= 1.1,
ANGLE commit, PanVK identification and the explicit loaded ICD/library pair.
It rejects llvmpipe, softpipe, swrast, SwiftShader, lavapipe and wrong backends.
A triangle shader must compile/link, draw, and pass center/corner RGBA readback
checks. Synchronous `glReadPixels` completes the GPU work before CPU access.
Verified CPU bytes are copied into a normal X11 window; XGetImage checks the
server-side pixels. Confirm the visible red triangle on the phone as well.
This presentation exports **no Vulkan GPU-memory FD**, and cannot treat the
fork's unrelated memfd as shared GPU storage.

Successful EGL resource teardown and child exit=0 are required. A 30-second
parent timeout, missing backend/device query, incorrect pixels, mismatched
library, native crash or nonzero child exit is failure. The final message says
Steam/CEF acceleration remains UNPROVEN; there is no accelerated Steam PASS.
The native ctypes/EGL/X11 path has not run on this phone or host. Cheap host
tests cover isolation, input/library identity, software rejection and failed
pixel rejection; they do not validate native rendering.

### Gates after this change

1. Later CI: produce the isolated native glibc X11-enabled PanVK artifact from
   the pinned source with truthful options; audit dependencies and driver
   configuration. No heavy builds were run locally for A2.
2. Phone: run the standalone probe; collect its full stdout/stderr, loaded
   loader/ICD paths, visible result and clean exit. Resolve any backend/device,
   kernel, format or context failure without changing the validated compositor.
3. Only after real standalone success: separately select client ANGLE Vulkan
   for Steam and prove Valve's flag propagation, GPU-process lifetime and
   actual CEF context/presentation compatibility. Require `webhelper_gpu.txt`
   showing Vulkan/PanVK/Mali and no software renderer, correct UI pixels, Stop
   and relaunch, and measured UI FPS. Select 1280x800 manually if desired.

C8B.1 is **not phone-successful** until that final Steam evidence exists.

## Phase B0: isolated CI artifact build

### Build-system audit before implementation

Source remains pinned to `efd07bab401b263a9ac114560ecdb9a2934f5d43`.
The audited [root Meson file](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/meson.build)
unconditionally adds `-llog` before platform selection. That Android-only
link requirement must be guarded by `host_machine.system() == 'android'` for
native glibc. This is a build portability change, not a logging stub, feature
override or driver behavior change. Other inspected Android helpers already
have non-Android implementations/guards. CI linking will verify this assessment;
no local compile was used to establish it.

The [Panfrost kmod build](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/lib/kmod/meson.build)
supports `panfrost-kmds=kbase` alone. It does not require enabling panthor or
the DRM panfrost kernel backend. Generic Mesa/Vulkan WSI and kmod code still
use libdrm types/helpers, so **libdrm remains a build/runtime dependency**;
kbase is not converted into a DRM device. The [PanVK build](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/vulkan/meson.build)
already limits driver variants to JM arches 6, 7 and 9. Arch 7 covers G52; no
new architecture patch or CSF backend is needed.

`panfrost-rust=false` is an existing default option; the audited GPU model and
shader compiler have a C implementation. LLVM/Clang, libclc,
SPIRV-LLVM-Translator and SPIRV-Tools are still needed to build Mesa CLC and
Panfrost's precompiled shader generators. They are **build tools**, not a
software renderer. No Gallium, Zink, llvmpipe or lavapipe driver is selected.
Debian 13 supplies LLVM/Clang/libclc/translator 19 and SPIRV-Tools >= 2024.1,
along with Meson >= 1.4, Python >= 3.10, Mako, packaging, YAML, C/C++ compilers,
Bison, Flex, Ninja, pkg-config, libdrm, libelf and zlib development files.
The actual installed package/toolchain versions will be recorded per artifact.

The first CI attempt stopped at Meson setup because `auto_features=disabled`
also disabled the `spirv-tools` feature. The narrow correction explicitly sets
`-Dspirv-tools=enabled`; Debian's `spirv-tools` package was already in the workflow.
Keep both compiler generators: the pinned
[`libpan/meson.build`](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/panfrost/libpan/meson.build)
uses `prog_mesa_clc` to generate per-architecture SPIR-V and
`prog_panfrost_compile` to generate embedded Mali helper shaders. PanVK consumes
these generated dependencies. Even `mesa-clc=auto` selects CLC when PanVK is
enabled; removing the explicit flags would not remove this requirement. This
fix does not establish that the next CI build will succeed.

The second CI attempt found SPIRV-Tools 2025.1.1 but the CLC API probe failed
before testing `LinkerOptions::SetAllowPtrTypeMismatch`: its own `args` list in
[`src/compiler/clc/meson.build`](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/blob/efd07bab401b263a9ac114560ecdb9a2934f5d43/src/compiler/clc/meson.build#L86-L101)
unconditionally passed `-Wno-error=unguarded-availability` and
`-Wno-error=unguarded-availability-new`, which GCC 14 rejects. The second source
patch filters these flags with `cpp.get_supported_arguments()` before passing
them to the probe. GCC remains the compiler; the actual API test, LLVM-version
failure condition and workaround definition remain unchanged. No SPIRV-Tools
package or capability is substituted. The next CI run must establish the real
API result and whether compilation succeeds; no local build was run.

WSI could be omitted for an independently built ANGLE offscreen display, but
that is not the supplied Steam bundle's proven configuration. Matching
`DisplayVkXcb::getWSIExtension()` requests `VK_KHR_xcb_surface` unconditionally;
ANGLE then requires `VK_KHR_surface` and `VK_KHR_swapchain`, including for the
existing pbuffer probe. Therefore this build needs `platforms=x11`. X11/XCB,
X11-XCB, DRI3/Present/RandR/SHM/Sync/Xfixes and xshmfence development dependencies
follow from Mesa's X11 WSI configuration. Compiling them does not enable missing
server DRI3 support. Wayland and Android WSI are not selected. The probe still
uses explicit CPU pixel copying and never exercises GPU-memory FD export.

The authoritative build options will be kept separately in
`tools/panvk/meson-options.txt`, with EGL/GLX/GLES/GBM/OpenGL, Gallium drivers,
Vulkan layers, tools, video codecs, tests and optional instrumentation disabled.
No advertised API, feature, extension or memory-sharing implementation is
patched. The existing experimental/compatibility caveats from Phase A/A2 remain.

### Defined CI architecture and packaging

`.github/workflows/build-panvk-glibc.yml` is **workflow_dispatch only**, with
read-only repository permissions. It runs natively on `ubuntu-24.04-arm` inside
`debian:13-slim`; the container is used only by GitHub Actions. There is no
cross compiler, emulation, QEMU or local container invocation. It is independent
of `build.yml`, Gamescope builds, release publishing and APK assets. No CI run
was dispatched as part of this task.

`tools/panvk/source.json` is the single source pin consumed by the second
checkout and checked before building. `build-glibc.sh` refuses execution outside
the native ARM64 Actions environment, verifies a clean source checkout, applies
`patches/0001-android-only-liblog.patch` and
`patches/0002-clc-supported-availability-warnings.patch`, and runs Meson with
the recorded options. Artifact metadata records both patch filenames and hashes.
The workflow now defaults to `artifact_mode=raw-test`, which bypasses
`package_glibc.py` after the same build; explicitly choosing `audited-package`
retains the strict path described here. The build script's three-argument default
also remains the strict path. No packaging validator or dependency allowlist is
weakened by the development bypass.
Ninja builds only `libvulkan_panfrost.so` and
`panfrost_icd.aarch64.json`, plus their required generated/build dependencies,
with two workers. CLC and precomp generators are not installed or shipped.

`package_glibc.py` inspects ELF headers, dynamic tags and version requirements
using readelf; it never dlopens the ICD or runs ldd on it. It requires ELF64
AArch64 shared objects, matching SONAMEs, and portable search paths. Only the
allowlisted dependency closure found in Debian's system library directories is
copied beside the ICD, with symlinks dereferenced. Missing, escaping or
unreviewed dependencies fail packaging. This includes rejecting `liblog`,
Vulkan loader DSOs, LLVM runtime DSOs, other Mesa drivers and vendor GLES blobs.
In particular, **`research/phone-mali/libGLES_mali.so` is never an input or an
artifact member**. No Android/vendor library is bundled.

Generic dependencies such as libdrm, libelf, X11/XCB and their transitive
non-glibc libraries are included only when ELF DT_NEEDED requires them. glibc
and `ld-linux-aarch64.so.1` are deliberately left to the existing guest.
The exact resulting library names, hashes, dependency edges, required symbol
versions and Debian package versions are recorded in `BUILD_INFO.txt`, together
with source/patch provenance, Mesa version, compiler/toolchain and Meson options.
License notices are included. The minimum guest ABI must satisfy all recorded
glibc symbol requirements; the container's glibc version alone is not the exact
minimum. Nothing replaces `/usr/lib`, rootfs ICD discovery files, the Vulkan
loader, software Mesa or the Gamescope proxy.

The optional **audited-package** Actions artifact named `panvk-glibc-arm64` contains:

```text
panvk-glibc-arm64.tar.gz
panvk-glibc-arm64.tar.gz.sha256
```

The tarball has this layout:

```text
panvk-glibc-arm64/
    lib/
        libvulkan_panfrost.so
        <only audited transitive non-glibc runtime DSOs, named by SONAME>
    share/vulkan/icd.d/panvk_droiddeck.json
    share/licenses/Mesa-license.rst
    share/licenses/<bundled Debian dependency copyright notices>
    BUILD_INFO.txt
    SHA256SUMS
```

The ICD JSON retains **every generated semantic field**, including
`file_format_version` and `api_version`. Only `ICD.library_path` becomes
`../../../lib/libvulkan_panfrost.so`, resolved relative to the JSON directory.
The upstream manifest advertises its instance API; the G52 physical-device API
is still independently queried. No API/feature version is raised or overridden.

File ordering, tar ownership/timestamps and gzip headers are normalized, and
every payload file is checksummed. Source/configuration and membership rules are
fixed; the Debian image tag and apt updates are not snapshot-pinned, so this is
**not a claim of bit-identical builds across different package updates**.
Actual package/compiler versions and file hashes make each output identifiable.
Configure failures preserve Meson logs in a separate failure-only artifact;
a failed build/package step produces no successful runtime artifact.

### Phase B1 preparation and phone commands

#### Quick phone proof (development-only ADB trigger)

The raw CI artifact **droiddeck-panvk-raw-test-arm64** is now reported green
and available. Polished B0 packaging remains deferred. Download its Actions ZIP
on the laptop. Install a **CI-built DroidDeck APK containing the test receiver**
once (no local build and no PanVK rebuild):

```sh
adb install -r /path/to/updated-droiddeck.apk
```

On the phone, start **normal Mali Steam Launch** and wait for its client to open.
From this repository on the laptop, with the phone authorized over ADB, run:

```sh
python3 tools/panvk/stage_phone_test.py ~/Downloads/droiddeck-panvk-raw-test-arm64.zip --run
```

The helper also accepts the raw `.tar.gz` or an extracted artifact directory.
It uses the existing ADB device resolver (`--serial` if needed), copies only
explicit raw files plus the two Python helpers to shared Downloads, triggers
the development-only `PanvkClientProbeReceiver`, waits for the request-specific
report and prints the complete result. No root, `run-as`, manual environment
setup, second display or PanVK build is needed. A missing receiver or inactive
Mali session is a clear failure. The shell-only receiver requires Android's
`DUMP` permission, like the existing checkpoint activity; normal Launch never
invokes it. Reports remain in `Download/DroidDeck/panvk-client-probe-<id>.log`.

If the existing **second-screen Linux terminal** is available, the receiver/APK
update is optional: transfer without `--run`, then paste the printed command
into that terminal (on the usual primary Android user it is):

```sh
python3 -B /storage/emulated/0/Download/DroidDeckPanVKTest/run_panvk_phone_test.py
```

This runs alongside Steam; it does not replace Steam's startup command. It
discovers the live Mali `droiddeck-session steam` client's actual environment
through same-UID `/proc` entries, rejecting the compositor's broker/proxy
environment and multiple displays. The receiver reuses `SessionTerminal`'s
already-prepared command, binds and host environment, replacing only its
interactive shell with the test helper. It registers that auxiliary runtime
process with the existing session cleanup and enforces a bounded timeout.
It never guesses `DISPLAY=:0`, starts another compositor, modifies Gamescope's
environment or changes the normal session command. No activity switch is needed.
The helper finds the paired ANGLE libraries under the installed
`/root/.local/share/Steam/steamrtarm64`. If more than one pair exists it reports
the candidate paths and fails; add `--angle-dir /actual/pair/directory` to select
one explicitly. It never substitutes the guest's system EGL/GLES.

Only the raw driver, original ICD, local path-only ICD, probe and available
dependency/provenance text are staged under **`/root/panvk-test/<content-id>/`**.
Each content version gets its own directory; repeated tests never overwrite an
already-loaded DSO. The original generated JSON remains unchanged. The probe
child alone selects the local PanVK ICD and library search directory. Gamescope,
the app's Mali broker/proxy, Steam's running software GL client and their
environments stay unchanged. No dependencies, vendor DSOs, glibc or loader are
installed. No download happens inside the runtime.

A separate dependency preflight prints the exact loader `OSError` (including
missing DSO or symbol-version requirement), then the existing probe runs and
prints its GL/device/loaded-library and pixel evidence. Any preflight failure,
native crash, timeout, missing evidence or nonzero probe result ends with
**`C8B.1 PanVK CLIENT-ONLY PROBE FAIL`** and a nonzero exit. Success requires all
existing hardware/backend/identity/pixel/cleanup checks and prints individual
PASS lines followed by **`C8B.1 PanVK CLIENT-ONLY PROBE PASS`**. Keep the complete
terminal output and confirm the red triangle. This proves the standalone ANGLE
path only; actual Steam/CEF UI acceleration is a subsequent step.

#### Temporary raw-test path (current development default)

The native Mesa build reached `[901/901] Linking target
src/panfrost/vulkan/libvulkan_panfrost.so` in CI. Polished portable packaging is
intentionally deferred. Run **Build isolated PanVK glibc ARM64 probe artifact**
manually on the intended branch with **artifact_mode: raw-test**. Download the
Actions artifact **droiddeck-panvk-raw-test-arm64** and extract the download ZIP
on the host. It contains a `.tar.gz` and its `.sha256` file. In that directory:

```sh
sha256sum -c droiddeck-panvk-raw-test-arm64.tar.gz.sha256
tar -xzf droiddeck-panvk-raw-test-arm64.tar.gz
(cd droiddeck-panvk-raw-test-arm64 && sha256sum -c SHA256SUMS)
```

This is **TEMPORARY / DEVELOPMENT-ONLY / NOT PRODUCTION-READY**. Its explicit
file list is the built `libvulkan_panfrost.so`, the **byte-for-byte generated**
`panfrost_icd.aarch64.json`, `DEPENDENCIES.txt` (readelf dynamic dependencies and
symbol-version requirements), `BUILD_INFO.txt`, `SOURCE_DIFF.patch`, Mesa's
license and `SHA256SUMS`. It bundles **no dependencies**, Android vendor DSOs,
glibc or Vulkan loader. It does not claim portable ABI compatibility or successful
phone execution. No strict packaging code runs in this mode.

Use the existing DroidDeck Linux-runtime file-transfer mechanism to copy the
whole extracted directory to
`/root/panvk-probe/droiddeck-panvk-raw-test-arm64/` **inside the Linux guest**.
Also copy `tools/mali-vulkan/probe_steam_angle.py` to
`/root/panvk-probe/probe_steam_angle.py`. An Android filesystem copy alone does
not establish that the files are present at these guest paths. In the guest:

```sh
cd /root/panvk-probe/droiddeck-panvk-raw-test-arm64
sha256sum -c SHA256SUMS
cat BUILD_INFO.txt
cat DEPENDENCIES.txt
getconf GNU_LIBC_VERSION
```

The generated ICD is format **1.0.1**, API **1.4.354**, `library_arch: "64"`,
with build-install path
`/opt/droiddeck-client-panvk/lib/libvulkan_panfrost.so`. Preserve that original
JSON as evidence. Create a **separate local ICD**, changing only `library_path`
to locate the adjacent raw library. The standalone probe now accepts validated
1.0.0/1.0.1 manifests and rejects a non-64-bit architecture field.

Run the following only in the existing isolated **Xwayland client environment**,
whose live `DISPLAY` is set and whose compositor broker/proxy selectors have
already been cleared by the client fork. Find the actual paired ANGLE libraries
in the installed Steam tree and replace `ACTUAL_PAIR_DIRECTORY` below. Keep
Gamescope's parent environment unchanged; do not replace normal Steam startup.

```sh
(
set -eu
panvk_test=/root/panvk-probe/droiddeck-panvk-raw-test-arm64
angle_pair=/root/.local/share/Steam/ACTUAL_PAIR_DIRECTORY
python3 -B - "$panvk_test" <<'PY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
manifest = json.loads((root / 'panfrost_icd.aarch64.json').read_text())
assert manifest['file_format_version'] == '1.0.1'
assert manifest['ICD']['library_arch'] == '64'
assert Path(manifest['ICD']['library_path']).name == 'libvulkan_panfrost.so'
manifest['ICD']['library_path'] = './libvulkan_panfrost.so'
(root / 'panvk-local.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
export VK_DRIVER_FILES="$panvk_test/panvk-local.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export LD_LIBRARY_PATH="$panvk_test:$angle_pair:/root/.local/share/Steam/steamrtarm64/panorama:/usr/lib"
# A separate process reports the exact missing DSO or symbol version before ANGLE
# can reduce an ICD load failure to a generic eglInitialize error.
python3 -B -c 'import ctypes,sys; ctypes.CDLL(sys.argv[1])' "$panvk_test/libvulkan_panfrost.so"
python3 -B /root/panvk-probe/probe_steam_angle.py \
  --icd "$VK_DRIVER_FILES" \
  --panvk-library "$panvk_test/libvulkan_panfrost.so" \
  --egl "$angle_pair/libEGL.so" \
  --gles "$angle_pair/libGLESv2.so"
)
```

If the preflight or probe reports a missing dependency, preserve the **exact
`OSError` library name/version** and `DEPENDENCIES.txt`; stop and report it.
Do not copy CI system libraries or vendor libraries into this artifact or replace
the guest's glibc/loader. Native glibc and dependency compatibility remain phone
questions, not promises made by a raw upload.

Require `ANGLE backend=Vulkan`, the explicit PanVK ICD/library and one guest
Vulkan loader, a real Mali-G52 device (software renderers including llvmpipe are
rejected), `GPU triangle readback: verified`, verified X11 CPU-copy pixels, clean
teardown and `probe exit=0`. Keep all stdout/stderr and visually confirm the red
triangle. This proves the standalone ANGLE draw/readback path only; Steam/CEF UI
acceleration and polished B0 packaging/upload remain unproven. The raw artifact
upload is now reported successful; phone execution remains to be measured.

#### Optional audited-package path (deferred)

If explicitly choosing `artifact_mode=audited-package`, the unchanged strict
packaging path must complete before the following instructions apply. Do not
infer packaging/upload success from host unit tests. Download its two files plus
`tools/mali-vulkan/probe_steam_angle.py` into an isolated guest directory.
For example, inside the existing Linux guest, with those files already present:

```sh
cd /root/panvk-probe
sha256sum -c panvk-glibc-arm64.tar.gz.sha256
tar -xzf panvk-glibc-arm64.tar.gz
(cd panvk-glibc-arm64 && sha256sum -c SHA256SUMS)
cat panvk-glibc-arm64/BUILD_INFO.txt
getconf GNU_LIBC_VERSION
```

Inspect the recorded required glibc symbol versions against the guest ABI. Keep
the unpacked tree together. Do not copy its JSON to `/usr/share/vulkan/icd.d` or
copy its libraries over installed libraries. Locate the actual installed Steam
ANGLE library pair first; no guessed path or system EGL substitute is accepted.

In the existing Mali Linux-runtime/Gamescope invocation, use the following as
the **client command instead of Steam**, after the existing fork has cleared
the parent proxy/broker selectors. Its client DISPLAY must be live, while the
parent keeps its normal broker/proxy and outer Wayland environment. B0 does not
add a new launcher or alter normal Launch to do this automatically:

```sh
python3 -B /root/panvk-probe/probe_steam_angle.py \
  --icd /root/panvk-probe/panvk-glibc-arm64/share/vulkan/icd.d/panvk_droiddeck.json \
  --panvk-library /root/panvk-probe/panvk-glibc-arm64/lib/libvulkan_panfrost.so \
  --egl /root/.local/share/Steam/ACTUAL_PAIR_DIRECTORY/libEGL.so \
  --gles /root/.local/share/Steam/ACTUAL_PAIR_DIRECTORY/libGLESv2.so
```

Do not start Steam for this proof. The probe's exec child alone receives both
`VK_DRIVER_FILES` and `VK_ICD_FILENAMES` pointing to the same canonical manifest,
and LD_LIBRARY_PATH starting at this artifact's `lib/`, followed by the paired
ANGLE directory and the existing guest search locations. The artifact contains
no Vulkan loader, so the actual existing loader must be observed, not assumed.
The parent environment remains unchanged. Compatibility/feature overrides are
cleared or kept disabled as described in A2.

The probe now explicitly reports `Vulkan loader=`, `PanVK library=`, exact ICD,
actual Vulkan device/vendor/device/API, ANGLE backend, GL strings, software
renderer rejection, GPU draw/readback result, X11 CPU-copy result, cleanup and
exit status. It refuses missing/mismatched PanVK or an ambiguous mapped loader.
Keep the entire stdout/stderr and visually confirm the red triangle. A driver
load/context/native crash/incorrect-pixel failure is not an accelerated PASS.

Outstanding questions remain: native kbase permissions/ioctls/jobs on G52,
honest formats/caps/cache behavior, actual bundled ANGLE backend/device-query
availability and likely GLES 2 limits. Unsafe external-memory fallback is not
tested or accepted. The pbuffer/manual CPU-copy proof does not establish native
swapchain/CEF sharing, Valve flag forwarding, GPU-process stability or Steam UI
acceleration. Those require later validation; Steam and production graphics
remain unchanged.

### Lightweight validation boundary

Host tests use synthetic ELF headers, mocked readelf data and tiny fixtures.
They check dependency closure/exclusions, wrong architecture/SONAME/search paths,
manifest semantic preservation/relocation, the actual probe's artifact selection,
software rejection and loader-path reporting. YAML and shell/Python syntax,
the portability patch against the exact downloaded Meson source, and diff/new
file whitespace are checked without compiling Mesa or loading Vulkan. No APK,
Gamescope, PanVK or container build, full suite, QEMU, commit or push is performed.
