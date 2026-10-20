# DroidDeck-Mali 0.5 Phase 6

Implementation is ready for review on `mali-0.5-port`; no commit was made.
Reference: `mali-gpu-experiment` at `3e1d9255`. Starting Phase 5 commit: `ba7695bb`.

## Result and architecture

Shared CPU staging is **default off**. A regular
`Context.getExternalFilesDir("mali-vulkan")/shared-staging.enabled` file opts in
at normal Mali session startup. With the release package this is:

```text
/sdcard/Android/data/com.droiddeck.launcher/files/mali-vulkan/shared-staging.enabled
```

A missing marker, a directory with that name, or unavailable external storage
selects `MALI_VULKAN_SHARED_STAGING=0`. Package-suffixed builds use their own
external-files directory. Stop before changing the marker, then Launch again.
No removed runtime/log API or Download-directory override is restored.
Enable/disable commands and journal collection are in
[`SHARED_STAGING.md`](../../tools/mali-vulkan/SHARED_STAGING.md).

Gamescope retains the glibc proxy ICD → Bionic broker → Android system Vulkan →
Mali-G52 → AHB/SYNC_FD Android compositor path. The shared source is CPU-only;
it is never imported into Vulkan and is not a GPU dma-buf. PanVK remains an
independent optional Steam/CEF client renderer, with its existing stride 56 and
`-cef-use-vulkan` selection. Shared staging is stripped from Steam/Xwayland and
nested clients. Neither Steam selection nor CPU affinity changes.

The **2 MiB pixel bulk-write fallback remains intact**. Unsupported capability,
registration denial, region limits, oversized maps or local mmap/alignment failure
retain bulk writes. A local map failure drops the registered region before
fallback. After registration succeeds, notification/protocol/ACK failure blocks
submission; it does not silently retry that upload through another transport.

## Final reference implementation audit

The final transport was already imported in Phase 1, and Phases 4–5 restored its
persistent-staging callers and diagnostic hooks. Direct branch comparison found
these production components byte-identical to the final reference:

- `app/src/main/cpp/malivulkan/`, including `shared_staging_commands.h`,
  `system_broker.c`, interop objects/commands and disconnect cleanup.
- `tools/mali-vulkan/shared_staging.h`, `shared_staging_types.h`, `icd_proxy.c`,
  `interop_icd.h`, `resource_registry.h`, submit/renderer integration and contracts.
- Shared notification/copy event definitions in `frame_trace.h` and
  `frame_trace_proxy.h`.

No native transport file needed replacement. In particular, the current 0.5
app-private journal path in `frame_trace.c` is preserved. The new Phase 6 digest
contract freezes 16 final reference transport files and 16 unchanged Phase 5
client/session/presentation/workflow files. Phase 5's existing guard still freezes
all 27 patches and 61 bridge files; only the launcher file being intentionally
changed is removed from that older aggregate guard.

Protocol version 7 and optional commands 92–95 remain unchanged. The broker creates
a bounded sealed memfd and keeps its read-only shared mapping. REGISTER replies
with its normal ACK followed by exactly one SCM_RIGHTS descriptor in the one-byte
`S` envelope. The proxy validates size/seals, maps writable pages and closes the
FD. Bounds remain three regions per connection, up to 16 MiB each. Pixel bounds
exclude the aligned sequence control area. Exact ranges and next-sequence checks,
release/acquire publication and a post-copy sequence check precede WRITE's ACK.
The existing connection lock and synchronous ACK keep uploads before QueueSubmit.

Unmap/free/disconnect release CPU maps explicitly, including when native GPU
allocations must remain quarantined. Re-Launch creates fresh maps/sequences.
Resource completion watermarks, reuse, upload dependencies, AHB ownership,
release-fence ordering, rendering and Vulkan synchronization are unchanged.

## Complete changed-file inventory

| Status | File | Reason |
| --- | --- | --- |
| M | `.github/workflows/build.yml` | Add the Phase 6 host runner after Phases 1–5 using the same pinned Gamescope source; retain build/cache/runtimeAssets preparation and validation. |
| M | `app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt` | Replace forced shared=0 with the app-specific regular-file opt-in at normal startup, default to 0, and log the selected transport setting. Keep existing readiness, staging, current logging and broker lease APIs. |
| A | `app/src/test/java/com/droiddeck/launcher/gpu/MaliSharedStagingTest.kt` | Two Android unit tests for unavailable/missing/directory markers, explicit enable, removal and recreation across startup decisions. |
| M | `tools/gamescope/PATCHES.md` | Document current optional client/shared routes and why no new Gamescope patch is required. |
| M | `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` | Accept exactly shared=0 or 1 in the existing Mali outer route; preserve arguments, proxy manifest, ordinary routes and client selection. |
| M | `tools/mali-vulkan/SHARED_STAGING.md` | Adapt reference enable and A/B instructions to current app-specific storage, current journal receiver/export tools and branch, without changing affinity. Retain protocol and measurement semantics. |
| M | `tools/mali-vulkan/run_phase1_tests.py` | Activate the previously deferred shared-staging gate contract; other historical Phase 1 deferrals remain labelled. |
| A | `tools/mali-vulkan/run_phase6_tests.py` | Run small native shared/bulk/registry/staging fixtures, real dispatcher exchange, launcher isolation, full patch contracts and pure journal analyzers in CI. |
| M | `tools/mali-vulkan/test_normal_launch.py` | Exercise default-off and explicit-enable Gamescope commands, preserve proxy selection, and reject invalid switch values. |
| M | `tools/mali-vulkan/test_phase3.py` | Replace historical prohibition of shared staging with the authorized explicit marker gate while preserving broker/client/presentation boundaries. |
| M | `tools/mali-vulkan/test_phase4.py` | Adapt the forced-off assertion to the marker gate; continue rejecting ownership/client overrides in general environment controls. |
| M | `tools/mali-vulkan/test_phase5.py` | Allow the Phase 6 launcher gate while retaining frozen bridge/patch protection and client-only PanVK checks. Recompute its remaining-file digest from committed Phase 5 bytes. |
| A | `tools/mali-vulkan/test_phase6.py` | Freeze final reference native/proxy behavior and unchanged Phase 5 client/session/presentation files; guard current configuration API and scope. |
| A | `tools/mali-vulkan/test_phase6_wire.py` | Exercise actual broker REGISTER/SCM_RIGHTS, close-after-map, WRITE/ACK/readback, DROP/unmap, fresh sequence, rejected sequence/disconnect and socket restart with host Vulkan mocks. |
| M | `tools/mali-vulkan/test_shared_staging.py` | Adapt the deferred old marker/patch-name contract to app-specific 0.5 storage and 0215, retaining actual memfd/FD/proxy-exchange tests. |
| M | `tools/mali-vulkan/test_steam_launch.py` | Prove shared=1 reaches only the outer proxy with identical Steam command, and both software and optional PanVK clients strip it across update restart. |
| M | `tools/panvk/test_upload_queue.py` | Prove dropped records invalidate exact shared notification/copy decomposition as they already do for bulk. No PanVK implementation changes. |
| A | `docs/development/mali-phase6.md` | This implementation, inventory, patch order, test results and validation report. |

## Final Gamescope patch order

All 27 patch files retain their Phase 5 bytes. No 0219 patch is required: the
private upload-only mapper already returns the shared pointer and existing child
environment isolation strips shared-staging controls. Application order is:

```text
0002-steamcompmgr-fallback-appid-focus.patch
0009-fix-arm64-steam-night-mode.patch
0019-steamcompmgr-arm64-virtual-white.patch
0020-color-p3-red-is-wide-gamut.patch
0100-realtime-queue-and-gamepad-cursor.patch
0101-nested-wayland-cursor-routing.patch
0110-wayland-backend-touch.patch
0111-wayland-pointer-warps-in-passthrough.patch
0112-restore-iconified-game-on-resume.patch
0114-take-override-redirect-from-mapnotify.patch
0115-limiter-paces-per-window.patch
0200-vulkan-enumerate-only.patch
0201-vulkan-capabilities.patch
0202-vulkan-create-device-test.patch
0203-vulkan-submit-test.patch
0204-vulkan-memory-ahb-tests.patch
0205-vulkan-gamescope-renderer-tests.patch
0206-vulkan-blit-input-proof.patch
0210-mali-persistent-session.patch
0211-mali-normal-wayland-session.patch
0212-mali-optimized-normal-resources.patch
0213-mali-frame-cost-attribution.patch
0214-mali-upload-ring-ranges.patch
0215-mali-steam-xwayland-client.patch
0216-mali-steam-x11-presentation-diagnostics.patch
0217-mali-frame-trace.patch
0218-mali-staging-bulk-writes.patch
```

The full stack applies to pristine pinned Gamescope 3.16.29. All Mali hunks apply
with zero fuzz and zero offset, and final protocol/private headers retain parity.

## Lightweight test results

| Suite | Passed |
| --- | --- |
| Phase 6 shared/native/session/patch contracts | 85 |
| Phase 6 upload-journal/loss/sampling analyzers | 31 |
| Phase 1 isolated Mali / PanVK/tool regressions | 102 + 126 |
| Phase 2 broker/proxy / small diagnostic translation units | 63 + 25 |
| Phase 3 ownership/presentation/Stop-restart/patch regressions | 27 |
| Phase 4 staging/resources/Steam/journal / analyzers | 104 + 65 |
| Phase 5 PanVK/client/evidence / artifact/performance | 62 + 34 |
| Unmodified 0.5 session tests | 23 |
| Unmodified Steam cold / desktop / x86 compatibility / runtime inputs | 7 + 9 + 4 + 14 |

Counts overlap across runners; they are not a count of unique tests. Existing
historical deferrals in the isolated Phase 1 runner remain explicit; their
applicable current integration checks run in subsequent-phase suites.

The transport fixtures execute actual production mapping/upload/submit/registry
functions. They cover sealed memfd creation, cross-process shared pages,
SCM_RIGHTS, CLOEXEC, malformed/missing/extra/truncated FDs, bounded registration,
range/sequence validation, completion-before-reuse, exact dirty ranges, ACK before
submit, fail-closed errors, fallback, disconnect and fresh map/restart behavior.
The 4 MiB staging fallback still produces exactly two 2 MiB writes. Real dispatcher
tests add socket-level registration, copy acknowledgement and readback against a
host Vulkan mock. These tests do not establish real GPU or Android behavior.

Host commands used the existing `/tmp/phase5-test-inputs` pinned Gamescope 3.16.29
source and Vulkan-Headers 1.3.296. No new source download was needed. Runners:

```sh
python3 -B tools/mali-vulkan/run_phase6_tests.py \
  --gamescope-source /path/to/pristine/gamescope-3.16.29 \
  --vulkan-headers /path/to/Vulkan-Headers-1.3.296/include
```

Phase 2–4 regressions use the same source/header arguments. Phase 5 takes only
`--gamescope-source`; Phase 1 needs neither. Small C/C++ native fixtures were
compiled; no APK, Gamescope, Mesa or PanVK was built locally. Unix-socket tests
required execution outside the socket-restricting sandbox. The stock session
suite's initial socket-bind failures passed on rerun with socket access.

All 11 changed/new Python files parse; session shell syntax, manifest XML, three
workflow YAML files and all 41 embedded workflow shell steps pass. The two new
Kotlin/JUnit gate tests require CI's Android unit-test classpath and were not run
through a local Gradle build.

`git diff --check` passed for all 18 changed files, including the five new files
using a temporary index; the real index remains untouched. The final diff stat is provided with the completion
response. No commit, Actions dispatch or device result is claimed.

## Exact CI and device validation still required

1. GitHub Actions: compile/package the Android native broker, glibc proxy and APK;
   run the new Phase 6 host runner plus `testReleaseUnitTest` (including both new
   gate tests). Compile the unchanged full 27-patch Gamescope stack, validate
   private symbols/diagnostic markers, same-run component overlays and source/output
   fingerprints. Preserve runtimeAssets preparation, cache behavior and existing
   `check_session_assets.py` validation. No additional Mesa/PanVK build is needed
   for this transport change.
2. On the validated ARM 0x13b5 / device 0x74021000 / Vulkan >=1.1 Mali-G52:
   verify missing marker selects 0; explicit marker selects 1 at fresh session
   startup; deletion plus Stop/Launch restores 0. Verify release-APK app-specific
   marker access and log output. Confirm actual Android kernel/SELinux memfd,
   seals, read-only broker mapping and SCM_RIGHTS behavior on the same-UID socket.
3. Verify capable=yes and qualifying transport=shared mmap. Require positive
   complete sampled `proxy_shared_rpc` and `broker_shared_copy` counts; persistent
   staging pixel `proxy_bulk_rpc` counts should disappear or approach zero.
   Aggregate legacy opcode counters exclude 92–95, so use journal spans. Both
   workers must be fresh and ready before capture; dropped >0 invalidates exact
   decomposition. Do not treat missing/unsampled spans as successful zero work.
4. A/B/A with the same workload, client renderer and affinity: compare pixels,
   text/colors, resize, scrolling and sustained slot reuse. Check submit ordering,
   native completion watermarks and unchanged producer/release fences. Registration
   refusal and mmap failure must use bulk; after-registration notification/ACK
   failure must prevent submission. Exercise process death, disconnect, normal
   Stop, immediate restart and Android surface loss; prove maps/FDs/sockets clean
   up, stale generations cannot cross sessions, and pending Android releases still
   drain or quarantine before resource destruction.
5. Repeat software Steam fallback and the existing optional PanVK/CEF evidence
   probe without changing client configuration. Gamescope must still map the proxy
   ICD and use the broker, while accelerated Steam/CEF maps PanVK separately.
   Preserve non-Mali Adreno/Turnip, x86/FEX, VR/Quest and ordinary session behavior.
   Leave shared staging disabled after validation.

Interpretation stays unchanged: QueueSubmit wall time is not GPU execution time;
rpc_receive includes blocking/scheduling; offCPU includes all descheduling;
Android latch is not physical scanout; release-fence timestamp is first observed
readability. No device performance improvement is asserted by host results.
