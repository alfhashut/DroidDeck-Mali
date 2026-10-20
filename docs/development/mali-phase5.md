# DroidDeck-Mali 0.5 port: Phase 5 PanVK clients

Base: Phase 4 commit `54ce11b1` on `mali-0.5-port`.
Reference: `mali-gpu-experiment`, final commit `3e1d9255`.
PanVK source pin remains `efd07bab401b263a9ac114560ecdb9a2934f5d43` from
`tools/panvk/source.json`. The Mesa options, patches, build script, strict audited
package implementation and generated ICD fixture are unchanged.

**Architecture and scope**

```text
Gamescope -> glibc Mali proxy ICD -> Android/Bionic broker -> system Vulkan -> Mali-G52
Steam/CEF -> bundled ANGLE Vulkan -> glibc PanVK -> /dev/mali0 -> Mali-G52
```

Gamescope still uses the proxy/broker path. PanVK is client-only and optional.
No global Vulkan ICD or driver search path is installed. The automatic normal
route retains ARM vendor `0x13b5`, device `0x74021000`, Vulkan >= 1.1; SteamVR
and other GPUs retain their existing routes. The receiver additionally requires
the exact cached hardware gate and a live normal Mali Steam session.

Shared CPU staging remains disabled: the broker still supplies
`MALI_VULKAN_SHARED_STAGING=0`, the outer normal launcher rejects an enabled value,
and private transport flags are stripped from clients. All 27 Gamescope patches
and 62 bridge/native/session contracts are byte-identical to Phase 4. No Vulkan
sync, AHB ownership, staging watermark, resource registry, upload-ring, 2 MiB bulk
transport, broker lifecycle or quarantine implementation changes.

**Artifact and isolated staging**

The PanVK workflow remains manual (`workflow_dispatch`), native ARM64 and raw-test
by default. It does not install PanVK into the APK, runtime image or Gamescope.
The downloadable Actions artifact remains `droiddeck-panvk-raw-test-arm64`, with
`out/droiddeck-panvk-raw-test-arm64.tar.gz` and its checksum. The existing
`audited-package` mode retains its strict dependency closure and provenance checks;
no allowlist or dependency failure is bypassed.

Raw transfer now validates all seven expected files and the exact checksum inventory,
pinned repository/commit, source-diff hash, patch hashes, configure arguments,
AArch64 ELF shared-object header, generated ICD fields and dependency evidence.
It rejects Android/vendor dependencies. These are integrity/provenance checks,
not a signature or a claim that arbitrary raw dependencies are portable.
Raw artifacts bundle no runtime dependency libraries. The existing isolated loader
preflight reports missing DSOs/ABI errors before the GPU probe runs.

The original `panfrost_icd.aarch64.json` is preserved byte for byte. A separate
`panvk-local.json` changes only its library path to `./libvulkan_panfrost.so`.
The guest copies the complete raw evidence into content-addressed
`/root/panvk-test/<sha256>/` directories. A different build never overwrites a
loaded library. The optional launcher checks driver, probe and relocated-ICD
digests before every launch. Older development configs need to be staged again
because the relocated ICD digest is now required.

Only Python client/proof helpers are added to APK runtime assets. No PanVK DSO,
proprietary Android Mali binary, dependency DSO or Android Vulkan loader is bundled.

**Optional Steam integration and 0.5 adaptations**

Without `~/.config/droiddeck/steam-client-panvk.json`, Mali Steam keeps the Phase 4
swrast/llvmpipe software client. Explicit configuration creates the reference
recoverable pointer to an ADB-staged config; normal Stop then Launch applies it.
Invalid configuration fails before client exec with a clear error. Removing the
shared config restores software on the next normal launch, even if the accelerated
client cannot start. Configuration never changes the running Gamescope environment.

The client wrapper requires the isolated Mali Xwayland shell and ARM64 architecture,
and rejects the broker/compositor environment. It constructs a child environment:

```text
ANGLE_DEFAULT_PLATFORM=vulkan
PANVK_ATOM_STRIDE=56
VK_DRIVER_FILES=<immutable target>/panvk-local.json
VK_ICD_FILENAMES=<same PanVK ICD>
LD_LIBRARY_PATH=<PanVK directory>:<bundled ANGLE directory>:<steamrtarm64>:...
```

It removes software selectors, inherited Vulkan layers/driver overrides, injection
variables and unrelated PanVK/ANGLE flags. Reference `PANVK_DXVK_COMPAT=0` and
`PANVK_KBASE_DRI3=0` remain. No version/feature spoof is added. Standalone probe
invocations now default to the known required stride 56; explicit `auto`/64 remain
diagnostic experiments and do not configure the Steam route.

Accelerated Steam receives `-cef-use-vulkan`. Its argv rejects legacy
`-cef-use-gl`/`-cef-use-angle` requests and conflicting backend flags. Ordinary
non-Mali 0.5 CEF selection is preserved; the software fallback retains its existing
OpenGL arguments. A dedicated `_bl_panvk_client` array is used only for the
accelerated launch. The existing `steam_client` array remains available to native
shutdown, mailbox/game helpers and x86/FEX routes. This preserves current 0.5
bootstrap/channel, update restart cap, exit status, desktop UI supervisor, sentinels,
storage/library handling and cleanup. The observer is stopped after each client
exit before an update restart.

No SessionService, SessionPrefs, SessionState, GPU detection, LinuxRuntime or
Gamescope patch changes are needed. Existing 0.5 broker ownership/session teardown
is reused. SessionFiles stages the wrapper and Python proof dependencies atomically.
Current build/cache, same-run Gamescope overlay, runtimeAssets preparation and
`check_session_assets.py` validation remain intact. Existing runtime-input source
fingerprints already cover `tools/mali-vulkan` and `tools/linuxfs`.

The DUMP-protected explicit receiver has no intent filter or automatic launch path.
It reuses SessionTerminal's actual prepared PRoot command/bindings/environment,
replacing only the known interactive-shell suffix. It accepts current 0.5's
`bash -c` wrapper and the direct `bash -i` form. It uses current auxiliary-process
registration/stop APIs, session identity checks and the reference 70-second bound.
Reports use `LinuxRuntime.logDir(context)` and are exported atomically to the
app-specific external files directory for release-APK ADB access. No removed
`debugLogDir()` or `run-as` requirement is restored. Download is used only for the
explicit raw-test transfer/recovery inputs, never as an environment override or
the new log destination.

**Evidence and measurement**

The existing standalone ANGLE probe still requires the audited bundled ANGLE
version, real Mali-G52/PanVK Vulkan properties, correct loaded library identities,
GPU triangle draw/readback and X11 CPU-copy/readback. Requested stride, a loaded
library, successful configuration or process startup cannot substitute for those
checks. Its PASS explicitly leaves Steam/CEF acceleration unproven.

The new live CEF proof queries the running client's local browser CDP endpoint
without starting a trace or changing renderer configuration. It records exact
Steam argv, same-UID descendant webhelper processes, CDP GPU process IDs, selected
PanVK/bundled ANGLE device/inode mappings and the GPU child's environment. It
requires all of the following for `DROIDDECK_CEF_PANVK_PASS`:

- Steam argv includes `-cef-use-vulkan` and no conflicting legacy/disable flags.
- A live descendant webhelper carries `--use-angle=vulkan`.
- CEF reports an ANGLE Vulkan PanVK Mali-G52 renderer, with no software renderer.
- `angle=vulkan`, `Display type=ANGLE_VULKAN` and `vulkan=enabled_on` are reported
  by live CEF data, rather than inferred from launch requests or old logs.
- The same CDP-identified GPU child has the selected PanVK and bundled EGL/GLES
  library identities plus stride 56, ANGLE Vulkan and the selected client ICD.
  It must not carry the Gamescope broker/normal-session environment.

Missing or contradictory evidence returns UNPROVEN and a nonzero exit. The existing
fresh-log/argv observer remains observational and never claims UI acceleration.
The ADB helper distinguishes CEF proof, static CEF audit, standalone GPU proof,
configuration and performance completion markers.

Phase 4 journal record/clock/drop semantics are unchanged. ADB performance collection
now requests the existing private journal export and selects only the requested
run's expected files from its ZIP. Missing/duplicate members or export failure are
reported, and unrelated/prior-run members are not extracted. Use the current
`tools/mali-vulkan/frame_journal_control.py` for arming; the old isolated
`tools/panvk/frame_journal.py` Download controller is not the 0.5 control path.
Submit wall time is not GPU execution, rpc_receive includes blocking/scheduling,
offCPU includes all descheduling, Android latch is not scanout, release timestamps
are first-readable observations, and dropped > 0 invalidates exact decomposition.

**Reproduction order after CI builds**

Build the CI APK from this change and manually dispatch the PanVK workflow on the
same branch. Install that APK, download the raw Actions artifact and start the
normal Mali Steam software session on the validated phone. Then:

```sh
# Isolated ANGLE/PanVK draw/readback proof; does not enable accelerated Steam.
python3 tools/panvk/stage_phone_test.py RAW_ARTIFACT.zip --serial SERIAL --atom-stride 56 --run

# Configure the optional native ARM64 Steam client; does not change this session.
python3 tools/panvk/stage_phone_test.py RAW_ARTIFACT.zip --serial SERIAL --steam-client enable --run
```

Stop normally and Launch again. Inspect the selected ICD/stride/library-path output,
then collect the live CEF proof:

```sh
python3 tools/panvk/stage_phone_test.py --serial SERIAL --cef-proof --run
# The existing read-only argv/binary audit remains separately available:
python3 tools/panvk/stage_phone_test.py --serial SERIAL --cef-diagnostics --run
```

If ANGLE pair discovery is ambiguous, supply `--angle-dir` for the installed ARM64
pair; the helper refuses to guess. Missing guest dependencies, wrong source/bundle,
unsupported GPU or failed readback must remain failures. Do not replace the system
loader, install proprietary Mali libraries or weaken feature checks to obtain PASS.

Software recovery requires neither a driver artifact nor a running session/receiver:

```sh
python3 tools/panvk/stage_phone_test.py --serial SERIAL --steam-client disable
```

Stop then Launch normally to return to the Phase 4 software client. The session
Terminal also provides `droiddeck-mali-diagnostics cef-proof`; it uses the same live
checks. Configuration and standalone success do not prove real CEF acceleration.

**Why each modified existing file changes**

| File | Required change |
| --- | --- |
| `.github/workflows/build-panvk-glibc.yml` | Run pure Phase 5 artifact/client contracts in the manual builder; preserve raw/audited output modes and source/build provenance. |
| `.github/workflows/build.yml` | Add the Phase 5 lightweight suite after current contracts; retain all APK/cache/runtimeAssets/component validation. |
| `app/src/main/AndroidManifest.xml` | Declare the explicit shell/DUMP-only PanVK client probe receiver. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionFiles.kt` | Stage the optional wrapper, standalone probe module and live CEF evidence module through current atomic asset staging. |
| `app/src/main/java/com/droiddeck/launcher/session/SessionTerminal.kt` | Build the diagnostic command from the live 0.5 runtime launch context; guard Mali Steam and known shell suffixes. |
| `tools/linuxfs/overlay/usr/local/bin/droiddeck-mali-diagnostics` | Expose the optional live `cef-proof` command. |
| `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` | Select PanVK only for explicitly configured Mali ARM64 clients; retain native shutdown and ordinary paths, preserve software recovery, and observe each accelerated launch. |
| `tools/mali-vulkan/frame_journal_control.py` | Share the existing receiver/export response parsing with the ADB performance collector; preserve CLI behavior and private storage. |
| `tools/mali-vulkan/probe_steam_angle.py` | Include steamrtarm64 in the child library path even when the selected ANGLE pair is nested. GPU validation/readback/sync code is unchanged. |
| `tools/mali-vulkan/run_panvk_phone_test.py` | Validate/copy full immutable raw provenance, bind relocated ICD identity into config, and default standalone execution to stride 56. |
| `tools/mali-vulkan/stage-diagnostics.py` | Stage the two additional pure Python probe/evidence dependencies; no DSO assets. |
| `tools/mali-vulkan/steam_client_environment.py` | Correct its description: the isolated client shell serves both software and optional accelerated Steam. Resolver behavior is unchanged. |
| `tools/mali-vulkan/test_phase3.py` | Retain the prohibition on PanVK/CEF flags in the outer Gamescope route while allowing the newly authorized client-only branch. |
| `tools/mali-vulkan/test_phase4.py` | Apply the same parent/client boundary to existing performance/session contracts. |
| `tools/mali-vulkan/test_steam_angle_probe.py` | Supply complete raw integrity fixtures, check corruption rejection, and verify the required default stride. |
| `tools/mali-vulkan/test_steam_client_panvk.py` | Include relocated ICD integrity, ARM64-only selection and tamper rejection in the imported reference wrapper tests. |
| `tools/mali-vulkan/test_steam_launch.py` | Execute optional CEF selection, update restart, native shutdown, invalid-config refusal and software recovery alongside ordinary-route checks. |
| `tools/panvk/perf_capture.py` | Replace inaccessible host-side private-path pulls with Phase 4's receiver ZIP export; retain capture identity and explicit error accounting. |
| `tools/panvk/stage_phone_test.py` | Validate raw inputs, transfer the expected source profile/dependencies, use the current app-specific exported report path, and add distinct live CEF proof mode. |
| `tools/panvk/test_perf.py` | Verify private journal export selects only the requested run and cannot turn export failure into a complete pair. |

New implementation files are the reference-based optional launcher and receiver,
the raw verifier, live CEF evidence collector and Phase 5 runner. New tests cover
raw integrity/provenance, live evidence/ancestry/mapping rejection, unchanged native
contracts and prepared terminal command behavior. The full inventory follows below.

**Lightweight results and remaining validation**

- Phase 5: 61 client/artifact/evidence/launch contracts plus 34 artifact/performance
  contracts passed. The manual builder's artifact-only mode passes 45 + 34 without
  a compiler, GPU or Gamescope source.
- Phase 4 regression: 103 final-path tests and 65 analyzers passed. The increased
  launch count includes the new optional-client/fallback cases.
- Phase 2 regression: 63 broker/proxy tests and 25 small diagnostic translation-unit
  tests passed. Phase 3: all 26 passed, including release/quarantine/restart and
  complete zero-fuzz/zero-offset patch application.
- Phase 1 regression: 101 Mali and 126 isolated PanVK/tool contracts passed. Its
  historical deferred checks remain labelled; applicable Phase 5 contracts run
  explicitly in the new suite.
- Unmodified 0.5 session regressions: 23 passed. Existing Steam cold/desktop launch,
  x86 compatibility and runtime-input checks: 34 passed.
- Python/shell syntax, Android XML, workflow YAML and all 41 embedded workflow
  shell steps pass. All-file whitespace checking includes new files through a
  temporary index; the real index is unchanged.

Tests use synthetic ELF/metadata, process trees, CDP data and local host mocks;
some existing fixtures compile small diagnostic/native units. Local Unix-socket
fixtures required execution outside the sandbox. Pristine Gamescope 3.16.29 was
downloaded and checked against the existing SHA-256 pin solely for patch tests;
public Vulkan headers support the small host fixtures. No APK, Gamescope, Mesa or
PanVK build ran locally. No device rendering, Actions run or commit is claimed.

CI must compile/package the full APK and existing Gamescope stack, run the two new
Robolectric terminal-launch tests in `testReleaseUnitTest`, check native symbols,
SONAME/component smoke tests, same-run overlays, source/output fingerprints,
runtimeAssets preparation and unchanged asset validation. The manual PanVK workflow
must build the pinned driver and retain exact generated JSON plus dependency and
source provenance. The raw artifact's required guest DSOs/symbol versions and the
installed Steam ANGLE bundle must be checked on the actual 0.5 runtime; missing
dependencies are not proven resolved by host fixtures.

On the validated Mali-G52 device, prove standalone VkDevice/draw/readback with stride
56, then prove the real accelerated Steam/CEF renderer using all live checks above.
Confirm actual library mappings, ANGLE Vulkan selection, enabled Vulkan status,
absence of an active llvmpipe renderer and usable Steam UI/sign-in/input. Verify
Gamescope retains its proxy ICD and broker socket while the client uses PanVK.
Exercise normal Stop, update restart, immediate restart, probe timeout/cancellation,
background/activity/surface loss and software disable/recovery; confirm auxiliary
cleanup, no stale sockets/groups and unchanged AHB release/quarantine behavior.
Capture Phase 4 journals during accelerated UI load without treating host submit
duration or Android latch as physical GPU/display timing. Real acceleration,
performance and device lifecycle behavior remain unverified until these tests run.

Shared CPU staging and broad Mali support remain deferred. No commit was created.

**Every added or modified file**

`A` means added in Phase 5; `M` means modified from committed Phase 4.

10 added files, 20 modified files, no deletions (30 total).

```text
A  app/src/main/java/com/droiddeck/launcher/gpu/PanvkClientProbeReceiver.kt
A  app/src/test/java/com/droiddeck/launcher/session/PanvkProbeLaunchTest.kt
A  docs/development/mali-phase5.md
A  tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-client-panvk
A  tools/mali-vulkan/panvk_raw_artifact.py
A  tools/mali-vulkan/run_phase5_tests.py
A  tools/mali-vulkan/steam_cef_evidence.py
A  tools/mali-vulkan/test_panvk_artifact.py
A  tools/mali-vulkan/test_phase5.py
A  tools/mali-vulkan/test_steam_cef_evidence.py
M  .github/workflows/build-panvk-glibc.yml
M  .github/workflows/build.yml
M  app/src/main/AndroidManifest.xml
M  app/src/main/java/com/droiddeck/launcher/session/SessionFiles.kt
M  app/src/main/java/com/droiddeck/launcher/session/SessionTerminal.kt
M  tools/linuxfs/overlay/usr/local/bin/droiddeck-mali-diagnostics
M  tools/linuxfs/overlay/usr/local/bin/droiddeck-session
M  tools/mali-vulkan/frame_journal_control.py
M  tools/mali-vulkan/probe_steam_angle.py
M  tools/mali-vulkan/run_panvk_phone_test.py
M  tools/mali-vulkan/stage-diagnostics.py
M  tools/mali-vulkan/steam_client_environment.py
M  tools/mali-vulkan/test_phase3.py
M  tools/mali-vulkan/test_phase4.py
M  tools/mali-vulkan/test_steam_angle_probe.py
M  tools/mali-vulkan/test_steam_client_panvk.py
M  tools/mali-vulkan/test_steam_launch.py
M  tools/panvk/perf_capture.py
M  tools/panvk/stage_phone_test.py
M  tools/panvk/test_perf.py
```
