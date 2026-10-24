# Public Mali project audit, 2026-10-24

The most useful next work is proving a game with a deliberately selected client
driver and compatibility tool. None of the inspected projects provides a tested,
drop-in replacement for this port's G52 presentation path. This audit implements
one small Steam installation fix and leaves graphics behavior unchanged.

Baseline: `mali-0.5-port` at `f745429f90443879b1c897f72e60a574a5b8edd8`.
The working tree was clean before the audit. The user's latest device evidence
establishes working KDE, interactive rendering and Stop/relaunch. Optional PanVK
CEF acceleration was demonstrated on the older implementation; it still needs
reconfirmation on 0.5. No game or new performance improvement is claimed validated
on this phone by this audit.

## Scope and evidence

Public GitHub repository search, repository metadata, exact commit trees, selected
source files and build/provenance documents were inspected. Source downloads went
to `/tmp/droiddeck-mali-audit`, not into the product. No external source was executed
or imported. Author/tester performance reports below are explicitly external
reports, not reproduced benchmarks. A recent commit is evidence of activity, not
a maintenance or compatibility guarantee. All ten repositories in the main table
were non-archived when inspected.

The comparison covers the graphics and launch files identified below, not every
line of every fork. GitHub's unauthenticated API reached its rate limit during
the supplementary driver search; raw pinned files and public Git refs were used
for the supplementary references. Unlicensed or documentation-only projects were
screened without treating their claims as working implementations.

## Repositories and versions

Commit links pin the inspected versions; dates refer to those commits, not release
publication. Base versions come from `app/build.gradle` where an app exists.

| Project / inspected ref | Base / activity | Hardware scope and license |
|---|---|---|
| [ASEbrahim/MaliBridge, main, 224b6e369220415f0f21083322b61171ee1d14de](https://github.com/ASEbrahim/MaliBridge/tree/224b6e369220415f0f21083322b61171ee1d14de) | Standalone bridge, not itself a versioned DroidDeck APK; active 2026-10-24. Older integration is [PR 517](https://github.com/Droid-Deck/DroidDeck/pull/517), head `6b823341454fc03345b809724802ba72a0fd490f`; do not equate that integration with current bridge main. | Primarily tested/reported on G720/G925; additional vendor-driver device reports do not establish G52 support. GPL-3.0-or-later **plus section 7 attribution/name terms** in `NOTICE`. |
| [Droid-Deck/DroidDeck, test/non-adreno-all, f9f65719ab1bca200af4032ad65f561d2a6765c8](https://github.com/Droid-Deck/DroidDeck/tree/f9f65719ab1bca200af4032ad65f561d2a6765c8) | 0.5.0; branch commit 2026-10-22. | PanVK pairs for JM/CSF, Venus and bridge alternatives; also non-Mali profiles. GPL-3.0 app, separately licensed driver components. |
| [The412Banner/droiddeck-nonadreno-drivers, main, f8bbe0fe02addc96a3566eed60eb1e5e646b6dad](https://github.com/The412Banner/droiddeck-nonadreno-drivers/tree/f8bbe0fe02addc96a3566eed60eb1e5e646b6dad) | Driver packaging and test-build coordination; 2026-10-19. Companion to the preceding app branch, not a separate compositor implementation. | JM G52/G76 and newer CSF families via different driver sources. No repository-wide license detected; do not import its scripts merely because its Mesa inputs are open source. |
| [DavidCimo/droiddeck-mali, mali-venus, 4f838891c9644de3ea0d3a385f0f80ae9652ed57](https://github.com/DavidCimo/droiddeck-mali/tree/4f838891c9644de3ea0d3a385f0f80ae9652ed57) | 0.3.1 prototype; latest inspected activity 2026-10-08, not marked abandoned. | Pixel 9 Pro / G715 primary device. GPL-3.0 app; Mesa/virglrenderer retain their own licenses. |
| [wallentx/DroidDeck, wallentx/powervr, c23bfee0984da9eeff35a6f4820df14d68fe0830](https://github.com/wallentx/DroidDeck/tree/c23bfee0984da9eeff35a6f4820df14d68fe0830) | Now reports 0.6.0; PR 570 source pinned 2026-10-23. Later than our 0.5 baseline. | PowerVR/Xclipse plus a Mali system-driver profile; profile-specific support, not a G52 validation. GPL-3.0 app; libhybris and layers require their own notices. |
| [MarkisLin/DroidDeck-Mali, main, 1a57639c97f6835d69775f35ad0a23626da38ec3](https://github.com/MarkisLin/DroidDeck-Mali/tree/1a57639c97f6835d69775f35ad0a23626da38ec3) | `0.4.0-sl1`; inspected branch commit 2026-10-18, repository push 2026-10-22. | Redmi K Pad, G925, CSF UAPI 1.30. GPL-3.0 app, Mesa-derived driver patches. |
| [Betaness/DroidDeck-G2-build, mali-g2, ea0a528e99331d53f5059a726fe001d6e1149d9c](https://github.com/Betaness/DroidDeck-G2-build/tree/ea0a528e99331d53f5059a726fe001d6e1149d9c) | 0.5.0; inspected branch commit 2026-10-23, repository push 2026-10-24. | Mali-G2/v15 Android 17 target. GPL-3.0 app; its Vortek integration is LGPL-2.1-or-later. Optional MaliBridge has separate terms. |
| [bonecold24/Droiddeck-Mali, main, e40c0f3263502f1ff8d3b7977a55f2037f573898](https://github.com/bonecold24/Droiddeck-Mali/tree/e40c0f3263502f1ff8d3b7977a55f2037f573898) | 0.4.0; inspected activity 2026-10-17. | G720-focused PanVK bundle/profile work; broader fallback policy is not proof on other GPUs. GPL-3.0 app; bundled components require individual provenance. |
| [zhengyanmian/droiddeck-dimensity, main, 9a19fa3c3c785a092a14975ef77463ac18027c29](https://github.com/zhengyanmian/droiddeck-dimensity/tree/9a19fa3c3c785a092a14975ef77463ac18027c29) | Separate research/probe repository, not a 0.5 app port; 2026-10-24. | Dimensity 9500/G1-Ultra research target. No repository-wide license detected; facts only, no reuse. |
| [Dkdndje/MaliDeck, main, 324aaf7cb583bb0088b02bd19fb4a929a247f1d6](https://github.com/Dkdndje/MaliDeck/tree/324aaf7cb583bb0088b02bd19fb4a929a247f1d6) | README-only compatibility project at the inspected commit; 2026-10-24. | Broad testing wish list, not a supported-device list or inspectable graphics implementation. README explicitly says no license has been added. |

Supplementary driver/game-launcher references:

* [FristOneRR-Admin/FristOneRR-Panvk-Source, efd07bab401b263a9ac114560ecdb9a2934f5d43](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source/tree/efd07bab401b263a9ac114560ecdb9a2934f5d43):
  **already our exact PanVK source pin**, as recorded in `tools/panvk/source.json`.
  The inspected `src/panfrost/vulkan/panvk_physical_device.c` is SPDX MIT.
  This is a driver rather than a DroidDeck release; it does not establish a newer
  independently validated G52 client stack. The packaging project also builds
  this source family for its JM route.
* [zenithblue-oss/panvk-kbase-android, tag panvk-kbase-v0.1.0-rc3-lowmem, commit 0183fc489f17156a6b266360b0c296cfd9109da1](https://github.com/zenithblue-oss/panvk-kbase-android/tree/0183fc489f17156a6b266360b0c296cfd9109da1):
  build/profile/notice reference used by the driver pairs. Its `NOTICE.md` separates
  MIT scaffolding, Mesa-derived patches and GPL-2.0 WITH Linux-syscall-note UAPI
  headers. The advertised JM/CSF generations do not imply support for our exact
  G52/kernel combination. No driver replacement was attempted.
* [zenithblue-oss/panplay, main 469b997a061b9bd404d32b52c9d78c5ee228c903](https://github.com/zenithblue-oss/panplay/tree/469b997a061b9bd404d32b52c9d78c5ee228c903):
  adjacent launcher, not a DroidDeck fork. README, `NOTICE.md` and component pins
  inspected; this is not a full audit of its X server. Own code MIT, vendored
  Winlator X server LGPL-2.1. It documents a tested G615 target and pinned
  Wine/FEX/DXVK/PanVK inputs rather than a demonstrated G52 Steam/CEF route.

## Architecture comparison and source evidence

**Our port.** Gamescope keeps the glibc proxy -> Unix RPC -> Bionic broker ->
system Vulkan -> G52 -> AHB/SYNC_FD -> Android path. KDE adds the private nested
Wayland server and Pixman/CPU composition in patch 0220. The optional Steam
client instead uses bundled ANGLE Vulkan -> native glibc PanVK -> `/dev/mali0`.
The latter is a separate driver domain, not a presenter replacement.

**MaliBridge.** Its generated Vulkan bridge is substantially broader than our
Gamescope-specific proxy. `client/conn.c` uses per-thread connections and statistics;
`server/server_conn.c` replays recorded command buffers. `server/server_memory.c`
supports tested memory strategies including host-pointer, dma-buf, dma-heap and
AHB sharing, with cache maintenance for cached readback. `client/wsi.c` and
`client/wsi_wayland.c` select Wayland dma-buf/gralloc or SHM copy presentation;
X11 has its own WSI. `server/server_pcache.c`, `server/server_presets.c` and the
guard/hang code address game workloads, persistent caches and driver faults.
These are different ownership/protocol contracts, not compatible replacement
headers for our proxy. See [memory implementation](https://github.com/ASEbrahim/MaliBridge/blob/224b6e369220415f0f21083322b61171ee1d14de/server/server_memory.c),
[WSI](https://github.com/ASEbrahim/MaliBridge/blob/224b6e369220415f0f21083322b61171ee1d14de/client/wsi.c)
and [connection implementation](https://github.com/ASEbrahim/MaliBridge/blob/224b6e369220415f0f21083322b61171ee1d14de/client/conn.c).

It has a stronger externally reported game matrix than our currently unproven
game path, particularly G720/G925. That is a reason to study compatibility tests,
not transfer its performance figures or broad device policy to G52. Several
compatibility choices intentionally approximate behavior: for example, its
documented dual-source/multi-viewport handling and shader-compile fallbacks.
They cannot be adopted as truthful capability support without independent
correctness work. `client/wsi.c:wait_display_release` also proceeds after a 100 ms
gralloc release wait timeout. Our quarantine invariant forbids adopting that
timeout policy. No such change was made.

**0.5 non-Adreno branch and driver pairs.** Open-source PanVK renders Steam/games
and Gamescope in the guest; the Android compositor can stay on the system driver.
The branch also carries distinct Venus and legacy bridge routes. Its
`droiddeck-session`, driver profiles and Gamescope patches 0126–0133 handle
explicit-sync availability, repeated SHM uploads, optional robustness2, memory
selection and diagnostic failures. This keeps more of normal Gamescope's client
and game machinery than our reduced Mali path. Conversely it relies on broader
driver/kernel assumptions: `patches/panvk_kbase_drm_node.py` reports kbase node
identity to satisfy Gamescope discovery. That does not create DRM PRIME/KMS or
syncobj ioctls on `/dev/mali0`. See [driver definitions](https://github.com/The412Banner/droiddeck-nonadreno-drivers/blob/f8bbe0fe02addc96a3566eed60eb1e5e646b6dad/drivers.json)
and [0.5 session implementation](https://github.com/Droid-Deck/DroidDeck/blob/f9f65719ab1bca200af4032ad65f561d2a6765c8/tools/linuxfs/overlay/usr/local/bin/droiddeck-session).

The pair recipes add Android dependency closure and glibc builds. They are useful
packaging comparisons; our raw client artifact/provenance checks already solve
part of that problem differently. Their sync-file FD leak script targets
`kbase_sync_file_wait_func`/pending waiter payloads. Those identifiers are absent
from the inspected physical-device file at **our pinned JM commit**; the script
explicitly has a no-op path for that case. Do not transplant its CSF fix or merely
raise the FD limit as a G52 fix. See [leak patch script](https://github.com/The412Banner/droiddeck-nonadreno-drivers/blob/f8bbe0fe02addc96a3566eed60eb1e5e646b6dad/patches/panvk_kbase_sync_file_fd_leak.py).

**DavidCimo Venus.** Guest Venus ICD -> vtest Unix socket -> Android
virglrenderer Venus server -> system Mali driver. This uses Venus, not virgl's GL
renderer. The documented game WSI copies GPU output through host-visible readback
and `wl_shm` into Gamescope, which uploads it again. `docs/mali-venus.md`,
`tools/venus/mesa-venus-vtest.patch`, `virglrenderer-android.patch` and
`layer/{layer,pipeline,bc,spirv}.c` document cached readback and compatibility work.
The author reports Silksong improvement on G715 after selecting cached readback
and explicitly synchronizing caches. Some layer operations are approximations
(including dropped clipping/culling cases); a working game report is not proof
that these capabilities can safely be advertised by our proxy. See
[source and measurement notes](https://github.com/DavidCimo/droiddeck-mali/blob/4f838891c9644de3ea0d3a385f0f80ae9652ed57/docs/mali-venus.md).

**wallentx system Vulkan.** `SystemVulkanDriver.java` defines separate vendor
profiles and a Mali `x11-shm` presentation policy. glibc loads Android vendor HAL
through libhybris/Bionic linker/TLS hooks, rather than our out-of-process RPC.
`tools/hybris/source.env` pins libhybris
`bc8cb3586b79d208fd21b925fa0209b3e432e008`; compatibility layers and tailored DXVK
payloads accompany it. Gamescope SHM patches 0126–0129 preserve per-commit uploads
and keep SHM/cursor textures internal. Profile-specific placed-memory code is
not a generic G52 or FEX fix. Avoid exchanging its newer 0.6 launcher wholesale
with our 0.5 behavior. See [profile implementation](https://github.com/wallentx/DroidDeck/blob/c23bfee0984da9eeff35a6f4820df14d68fe0830/app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanDriver.java)
and [PR 570](https://github.com/Droid-Deck/DroidDeck/pull/570).

**MarkisLin G925.** PanVK/DXVK renders games while software Steam/GLX and Xvfb
provide the X display; `tools/xtools/xpresent.c` uses XDamage, framebuffer mapping
and Wayland SHM presentation to the Android compositor. Gamescope is bypassed on
that route. `driver/patches/p1-watchdog-rearm.patch` changes a **CSF queue**
watchdog based on observed progress; it is not our compositor no-frame watchdog.
The Vulkan 1.4 command-alias patch likewise requires real corresponding driver
support. Its README explicitly documents software CEF and absent dmabuf/GBM export.
This is a useful game/X11 fallback study, not a simpler way to preserve our
Gamescope requirements. See [presenter](https://github.com/MarkisLin/DroidDeck-Mali/blob/1a57639c97f6835d69775f35ad0a23626da38ec3/tools/xtools/xpresent.c)
and [driver patch](https://github.com/MarkisLin/DroidDeck-Mali/blob/1a57639c97f6835d69775f35ad0a23626da38ec3/driver/patches/p1-watchdog-rearm.patch).

**Betaness G2.** The Vortek route forwards glibc Vulkan to a Bionic vendor-HAL
server using sockets/shared rings. The default client WSI uses shared pixels ->
MIT-SHM -> rootful `Xwayland -shm` -> Android compositor, bypassing Gamescope.
Its `mali-g2/vortek/src/{zero_copy,headless_swapchain,client_x11_present}.c`
and patches also contain an alternative direct sharing/acquire path. Its
resource/disconnect work is particularly useful as a test-design reference:
abrupt client exit, outstanding work, repeated launches, FD and mapped-memory
accounting. The command-streaming patch warns that deferred BC uploads can escape
their intended submit ordering on devices without native BC; do not port it to
our upload path. Its cleanup code includes recovery behavior that must not be
substituted for our proven-completion/quarantine rules. See [integration and constraints](https://github.com/Betaness/DroidDeck-G2-build/blob/ea0a528e99331d53f5059a726fe001d6e1149d9c/mali-g2/vortek/README.md)
and [headless swapchain](https://github.com/Betaness/DroidDeck-G2-build/blob/ea0a528e99331d53f5059a726fe001d6e1149d9c/mali-g2/vortek/src/headless_swapchain.c).

**Other screened projects.** bonecold24's `MaliDeviceProfile.kt`,
`MaliFallbackChain.kt`, bundled driver metadata and `docs/MALI-PIPELINE.md` are
profile/bundle/fallback work. Its fallback can select an imported driver when
the UAPI is unknown and can reject software fallback on real GPUs; neither
policy should replace our narrow validated gate and working software client.
Its pipeline document describes other implementations as well as a target;
it is not evidence every described path is implemented there. The Dimensity
repository contains host Vulkan and kernel probes plus Wine/DXVK experiments,
not a proven integrated 0.5 session. MaliDeck has no implementation to compare.

## Compositor, transport and reliability comparison

The following is a source-level comparison, not a measured speed ranking.

| Area | Our current path | Relevant external difference / applicability |
|---|---|---|
| Renderer / allocator | Custom proxy Vulkan renderer/presenter; KDE uses wlroots protocol/scene helpers, Pixman and SHM allocation. | Direct PanVK forks retain more of stock Gamescope Vulkan/wlroots. Xvfb/Vortek rootful-X routes bypass it. No interchangeable allocator was found. |
| Scene / output lifecycle | KDE scene/subsurfaces are CPU composed; headless output provides metadata, while the existing Mali loop presents to Android. | Normal Gamescope's backend/output scheduling is active in direct-driver routes. Starting a second wlroots output loop in our path could duplicate pacing; not adopted. |
| Refresh / frame pacing | Actual Android feedback is forwarded; no fabricated physical scanout timestamps. KDE first-frame grace 90 s, subsequent no-progress 15 s. | SDL refresh and wait-before-present patches are backend-specific. CSF queue watchdog extension is not a fix for this loop. Measure actual frame latency before changing pacing. |
| Damage / partial updates | Upload ring tracks modified ranges with conservative fallback. KDE's CPU scene composition is not a demonstrated end-to-end damage-only present. | XDamage presenter and external WSI present regions suggest reducing unchanged SHM work. Scene motion, resize, occlusion, cursor and dropped histories require conservative invalidation. |
| SHM / buffer import | SHM pixels upload through persistent staging; AHB-only broker objects are never advertised as dma-bufs. | Other forks fix stale reusable SHM contents by uploading on every commit. A cache keyed only by `wl_buffer` identity would regress correctness. Real dma-buf exporters must be distinguished from kbase node discovery. |
| AHB / releases | Explicit Android release acknowledgement, proven completion before reuse, timeout quarantine. | External bridges have different gralloc/implicit-sync contracts. In particular MaliBridge's bounded wait then reuse policy is incompatible with ours. |
| CPU / GPU copies | CPU SHM import/scene work, staging transport, broker write and GPU upload remain distinct costs. AHB avoids a final CPU readback. | Venus/SHM X11 paths add GPU readback then CPU copy then compositor upload. Cached readback helps that direction; it is not automatically a fix for our CPU-to-GPU upload. Direct import can remove stages only when the real producer/consumer contract exists. |
| RPC / mapping | Persistent pools, resource registry live lookup, completion watermarks, range tracking, 2 MiB bulk transport; shared mmap transport remains opt-in. | MaliBridge batches recorded commands/per-thread connections; Vortek has shared command rings/optional streaming. They change ordering, object lifetime and concurrency assumptions. No RPC reduction is claimed without counter evidence. |
| Buffer count / queue depth | Existing bounded ownership/reuse state retained. | G2 swapchain minimum-count experiments are workload/device-specific. Increasing queues can increase latency and held AHBs; no count change made. |
| Scheduling / affinity | Existing session and client policies retained. | No controlled G52 result justifies another fork's affinity/queue tuning. Off-CPU time includes all descheduling, not just a broker wait. |
| Stop / lost device | Guest drain then broker shutdown; stale socket cleanup, same-UID ownership and quarantine remain explicit. | External repeated-exit tests offer useful coverage ideas. Driver-call signal recovery, forced timeline signaling or ignoring fences cannot establish completion in our architecture. Kernel KFAULT is not repaired by restarting a userspace client alone. |
| Persistence / updates | App-managed scripts regenerate; persistent Steam home is separate from managed runtime inputs. Explicit/failed-start repair exists. | Several inspected installers also discard the first-install manifest. No competing rootfs/update implementation was proven to solve our report; one local metadata omission is fixed below. |

No inspected implementation establishes a production direct DRM/KMS path on this
unrooted G52 phone. PanVK Vulkan is not Panfrost OpenGL, and a `libvulkan_panfrost.so`
filename does not prove EGL/GBM or accelerated Xwayland. Zink and virgl/Venus are
different layers with different feature requirements. None is enabled here.

## Game support and Steam priorities

The important gap is **client game integration**, not another desktop renderer.
In our `droiddeck-session`, `BL_MALI_STEAM_UI=1` deliberately skips ordinary
ARM64 Proton provisioning/compatibility rewriting and several game helpers. The
software route remains a Steam/Xwayland compatibility route. The PanVK probe
prepares `PANVK_DXVK_COMPAT=0`, `PANVK_KBASE_DRI3=0`, the G52 stride and paired
Steam ANGLE/loader libraries. That validates a narrowly defined CEF experiment;
it does not establish DXVK/VKD3D readiness or a game-specific WSI configuration.
Steam descendants can inherit its environment, so client-only intent alone is
not proof of correct per-game driver isolation.

The non-Adreno test build and adjacent PanPlay maintain game-capable
Wine/FEX/DXVK payload selection. PanPlay's inspected component pins are FEX
`9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf`, DXVK
`b1a1c99ab52b687cf950d62c88bc2fa316b41663`, and vkd3d-proton
`2230755878b01993b4b82d0ac0c0624f3ab4d333`. A newer package/version is not evidence
that its feature requirements fit this G52. See [component pins](https://github.com/zenithblue-oss/panplay/blob/469b997a061b9bd404d32b52c9d78c5ee228c903/scripts/components.env).

For a first game, inventory **the game process's actual driver**, enabled
features, selected Proton/Wine/FEX and architecture before choosing DXVK. Start
with a small native Vulkan/SHM-present client or a simple D3D9/11 title after a
standalone DXVK capability/device test. D3D12/VKD3D is a later target. Keep the
Gamescope proxy for presentation and establish a separate explicit client-game
environment; do not expose our Gamescope-specific proxy to arbitrary games or
advertise absent features to make a newer DXVK start. No Proton/DXVK package,
per-game profile or client driver policy was changed in this audit.

Steam CEF still needs actual new-run webhelper evidence: `-cef-use-vulkan`,
ANGLE Vulkan, PanVK/Mali-G52, `ANGLE_VULKAN`, `enabled_on`, and no software active
renderer. Other forks' software CEF, Zink GL or UI screenshots do not substitute
for that proof. Our existing launcher/probe already checks substantially more
than a request flag. Ordinary software fallback, ARM64/x86-64 selection and FEX
are preserved.

## Improvements by risk

| Group / priority | Proposal | Evidence and decision |
|---|---|---|
| A / implemented | Retain the downloaded ARM64 client manifest after a successful normal install. | Local installer demonstrably discarded it, while its own `--sync` retained it. Fix below; no graphics dependency. |
| A / next | Repeatable Steam install/start/restart evidence with manifest readability, active channel/build and actual CEF renderer. | Use existing launcher/probe artifacts first; distinguish intentional update/repair from missing local metadata. No new logger added speculatively. |
| A / next | Repeated game exit/crash/Stop FD/RSS/broker-resource accounting. | Inspired by G2 leak tests and non-Adreno reports; our current ownership tests already cover several failure cases. Device soak coverage is the missing evidence, not grounds to weaken cleanup. |
| A / conditional | Persistent client pipeline/cache identity diagnostics and missing-manifest reason reporting. | MaliBridge's per-game cache work is relevant to actual games. First determine whether native PanVK/DXVK already persists its cache; do not add a second broker cache for a client that bypasses the broker. |
| B / high value to test | Scoped per-game PanVK/DXVK/Wine/FEX environment and WSI selection. | Other projects demonstrate game integration, our Mali route intentionally omits it. Must qualify real capabilities, preserve software CEF and ordinary x86/FEX paths; no automatic implementation. |
| B / high value to test | Existing shared staging OFF/ON experiment with a real client workload. | Already implemented here; require positive shared notifications/copies, reduced pixel bulk RPCs, stable ACK-before-submit and forced-fallback results. Remains default OFF. |
| B / conditional | Damage-aware SHM copy/upload and CPU scene updates. | XDamage/present-region concepts; prove less copied data with moving/resizing/occluded surfaces and repeated same-buffer commits. Reuse our conservative ring fallback; do not optimize desktop alone. |
| B / conditional | Batch non-dependent RPCs / reduce redundant mapping and waits. | Per-thread/batched bridge designs are references. Measure frame journal first; preserve upload dependencies, sequence checks, completion watermarks and ACK-before-submit. |
| C / research only | Real PanVK client dma-buf/AHB import to Gamescope; GPU nested composition; Zink/glamor; direct PanVK Gamescope; libhybris/Venus/Vortek replacement. | Different external memory, driver, kernel and lifetime contracts. No compatible G52 proof identified. No Category C implementation authorized by this audit. |

Our strongest demonstrated differences are the validated G52 system-driver
presentation path, conservative release/quarantine contract, explicit optional
client-driver isolation, completed staging/range tests, real KWin protocol/socket
handoff and bounded loss-aware diagnostics. Other projects' strongest differences
are game coverage, broader command/feature implementations and multiple client WSI
choices. Neither observation is a universal performance ranking.

## Implemented fix and provenance

Changed `tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-install`: after all
downloaded parts pass SHA-256 and extract successfully, save the exact downloaded
`steam_client_<channel>_linuxarm64` text as
`package/steam_client_<channel>_linuxarm64.manifest` for **normal and sync** installs.
Previously that copy was inside the `--sync` branch. Channel selection, readiness
stamp, retry/CDN order, repair triggers, `.installed` handling and launch arguments
are otherwise unchanged. No synthetic `.installed` receipt is created.

This is independently implemented from our own installer at the baseline commit,
under the repository's existing GPL-3.0 license. It borrows no external code. The
inspected 0.5 test branch, G2 and libhybris fork retain the same conditional-copy
pattern; this is not represented as a superior competing-fork persistence fix.

Host evidence before the fix: the new retention test failed because the manifest
did not exist, and normal-install-then-sync downloaded the client archive again.
After the fix both pass. A malformed archive with a valid download checksum
still fails extraction without publishing a manifest/ready stamp. Explicit repair
still replaces bad metadata, retains good cached packages, removes stale
`.installed` files and remains bounded by the existing repair marker behavior.

**Limits:** keeping metadata prevents this omission on future successful installs
and removes the reproduced redundant same-version sync download. It does not prove
Valve's bootstrap will never update, or that this omission was the sole cause of
the user's phone reinstall. Existing complete installations with a missing
manifest still take the existing offline ready fast path; no guessed receipt,
forced reinstall or unsolicited network repair was added. A real channel update
and explicit Repair Steam remain available.

No external concept/code was ported into runtime graphics. Future reuse requires
file-level provenance: MaliBridge's extra terms must be honored if code is used,
Vortek retains LGPL obligations, Mesa/UAPI files keep their distinct licenses, and
unlicensed packaging/research projects supply facts only. No proprietary vendor
binary was downloaded or bundled.

## Verification and remaining device work

Lightweight host checks completed:

| Check | Result |
|---|---|
| `python3 -B -m unittest tools.tests.test_steam_install` | 12 passed, including four new persistence/failure/sync regressions and updated explicit-repair assertions. |
| `run_phase3_tests.py` with pristine Gamescope 3.16.29 and Vulkan-Headers 1.3.296 | 52 tests passed; the additional executable real-wlroots fixture class was skipped because it needs public wlroots headers/library/XML. Broker ownership/cleanup, dynamic sockets, KWin wrapper/lock and previous-patch byte contracts passed. |
| `run_phase4_tests.py` with the same source/header inputs | 105 Mali + 65 parser/analyzer tests passed. |
| `run_phase5_tests.py --gamescope-source ...` | 63 client/artifact/CEF/launch + 34 artifact/performance tests passed. |
| `run_phase6_tests.py` with source/header inputs | 87 transport/ownership/regression + 31 upload tests passed. |
| Ordinary Steam/session/runtime subset | 71 passed: install, cold launch, restart cap, exit status, compatibility environment/x64, desktop launch/UI, session status/logging/assets and runtime inputs. Includes the installer tests above. |
| Syntax / whitespace / patch boundaries | Bash syntax for installer, session script and KWin wrapper; Python AST for the modified test; `git diff --check` and untracked-report whitespace check passed. All 30 Gamescope patches are byte-identical to HEAD. |

The initial sandboxed Phase 3 run failed because temporary Unix socket binding
was forbidden (`EPERM`); rerunning with socket permissions produced the results
above. Native test compilation was limited to existing small host fixtures. No
APK, Gamescope executable, NDK, Mesa or PanVK build was performed.

Recommended device sequence, in priority order:

1. Use the CI-built APK/runtime containing the installer fix. Preserve Steam data;
   record whether it takes the ready fast path, intentional repair or genuine
   channel update. On the next controlled install/repair, confirm the downloaded
   manifest survives Stop/relaunch and remains readable. Do not delete an existing
   Steam installation just to exercise this test.
2. Re-prove software Steam/Xwayland and clean Stop/relaunch with shared staging
   OFF and PanVK OFF. Then explicitly enable only the existing client PanVK path,
   retain `PANVK_ATOM_STRIDE=56`, and capture the complete new-run CEF evidence.
   Confirm Gamescope still loads the proxy rather than PanVK.
3. Establish the separately scoped game-driver/toolchain capability baseline,
   then test a simple game at a fixed resolution. Record rendering correctness,
   actual client Vulkan ICD, upload/readback path, frame times, memory/FD growth,
   game exit and Stop/relaunch. Do not conflate UI GPU acceleration with game proof.
4. Only after the baseline succeeds, compare shared staging OFF/ON and transport
   counters with journal loss checks. Dropped records invalidate exact
   decompositions; queue-submit wall time is not GPU execution, RPC receive time
   includes blocking/scheduling, release-fence timestamps are first observations,
   and Android latch is not physical scanout. Leave the shipped default OFF.

All Gamescope patches through 0221, native broker/proxy/Vulkan/AHB code, PanVK
driver/environment behavior, shared-staging behavior, Vulkan synchronization,
CPU affinity, hardware gate and Steam client selection are unchanged. The 2 MiB
fallback remains. The only runtime code change is retaining successful Steam
installation metadata. Changes remain uncommitted for review.
