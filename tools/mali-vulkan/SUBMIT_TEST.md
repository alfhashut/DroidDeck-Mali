# Checkpoint 4C: observable core Vulkan queue execution

The separate Android action **Run Gamescope Vulkan submit test** launches actual
Gamescope 3.16.29 with `--vk-submit-test` through GuestCommand/proot and the normal
glibc Vulkan loader. Only that child enables `MALI_VULKAN_SUBMIT_TEST=1`. The 4B
`--vk-create-device-test` / button keeps its v4 device-lifecycle behavior. Older
probe, ICD, enumeration and capability actions retain their protocols.

The diagnostic selects the real ARM vendor/device IDs `13b5:74021000`, prints the
native physical API, and requires family **0** to have graphics+compute and a
queue. It creates one device with zero extensions/features and one family-0 queue
at priority 1.0. Instance API remains the implemented proxy ceiling **1.0**; the
broker's capability-query instance can use native 1.1. No version spoofing occurs.

## What succeeds

1. Create a flags-zero command pool for enabled family 0; allocate exactly one
   PRIMARY command buffer.
2. Create a normal flags-zero event and require native `VK_EVENT_RESET`.
3. Begin with flags zero, no pNext and no inheritance. Record exactly one
   **Vulkan 1.0** `vkCmdSetEvent(event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)`. End.
4. Create an unsignaled flags-zero fence. Submit one command buffer in one core
   `vkQueueSubmit`, without wait/signal semaphores or pNext.
5. `vkWaitForFences` uses **5,000,000,000 ns**. Require `VK_SUCCESS`, then native
   `vkGetFenceStatus=VK_SUCCESS` and `vkGetEventStatus=VK_EVENT_SET`.
6. Destroy fence, destroy event, free command buffer, destroy pool, destroy device,
   destroy instance; print `observable device command executed: yes` and `exit 0`.

The event is queried through the broker's **real vkGetEventStatus**; the proxy
never fabricates event/fence state or implements vkSetEvent on the host. Recording
alone cannot satisfy the observation. This checks queue execution and completion,
not rendering: no application buffers/device memory, images, shaders/pipelines,
WSI/backend, presentation, AHB/DMA-BUF transfer, Steam or DXVK is implemented.
The driver can internally allocate memory for its command pool/buffer.

Core [vkCmdSetEvent](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetEvent.html)
sets an event from device execution. A finite
[vkWaitForFences](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html)
wait distinguishes completion from timeout. Fence completion alone does not
prove our command executed, which is why the final event SET is mandatory.

## Version 5 RPC schema

`submit_protocol.h` adds v5; v1–v4 layouts/semantics remain supported. V5 inherits
v4 device creation/queue lookup/destruction and v3 native queries. Sessions pin
their version at CREATE and cannot upgrade/downgrade. Older versions cannot issue
v5 operations. All requests have the existing 16-byte header and start with a
**u32 logical-device ID**. Every other field is LE u32 except the named LE u64
nanosecond timeout. Never transmit real handles, pointers, callbacks or pNext.

| Opcode | Name | Fields after device ID | Request bytes |
| --- | --- | --- | --- |
| 17 | POOL_CREATE | family, flags=0 | 12 |
| 18 | POOL_DESTROY | pool ID | 8 |
| 19 | COMMAND_ALLOCATE | pool ID, level=PRIMARY(0), count=1 | 16 |
| 20 | COMMAND_FREE | pool ID, command ID | 12 |
| 21 | COMMAND_BEGIN | command ID, flags=0 | 12 |
| 22 | COMMAND_END | command ID | 8 |
| 23 | EVENT_CREATE | flags=0 | 8 |
| 24 | EVENT_DESTROY | event ID | 8 |
| 25 | EVENT_STATUS | event ID | 8 |
| 26 | COMMAND_SET_EVENT | command ID, event ID, stage=ALL_COMMANDS(0x10000) | 16 |
| 27 | FENCE_CREATE | flags=0 | 8 |
| 28 | FENCE_DESTROY | fence ID | 8 |
| 29 | FENCE_STATUS | fence ID | 8 |
| 30 | FENCE_WAIT | fence ID, waitAll=0/1, timeout u64 (0..5 seconds) | 20 |
| 31 | QUEUE_SUBMIT | queue ID, command ID, fence ID (0 permitted) | 16 |

Create/allocate replies have common status/VkResult/count=1 plus a u32 broker ID;
others have count=0 and no extra data. Native positive statuses `VK_NOT_READY`,
`VK_TIMEOUT`, `VK_EVENT_SET`, `VK_EVENT_RESET` and native negative errors, including
**VK_ERROR_DEVICE_LOST**, survive the v5 proxy transport exactly. Protocol failures
are distinct from driver results. Exact lengths, malformed flags/counts, invalid
IDs/types/parents and invalid states are rejected before calling Android Vulkan.
Truncated frames/disconnects trigger session cleanup.

## Ownership and supported Vulkan subset

Android owns all real objects. Each connection has up to 16 devices and, per
device, 32 pools, 32 commands, 32 events and 32 fences. A connection-wide monotonic
child-ID counter covers all four new types, preventing accidental cross-type ID
aliasing; IDs are never reused. Device/queue counters preserve 4B semantics. IDs
have meaning only within their connection. Parent lookup always includes device:

```text
device -> queues (enabled family and indices from 4B)
       -> command pools (enabled family only) -> primary command buffers
       -> events
       -> fences
command buffer -> recorded event; pending submission -> queue/fence (optional)
```

The glibc device/queue/command buffer are local loader-dispatchable objects whose
first word is VK_LOADER_DATA. Opaque pool/event/fence handles refer to local typed
objects. Only their stable IDs enter RPC payloads. Local tombstones stay allocated
until device/instance teardown to reject duplicate frees/destroys without reading
freed memory. Resource-list publication/lookup and socket exchanges use the
instance mutex; tombstone/error flags are atomic. Applications still obey Vulkan's
external synchronization and valid dispatchable-handle rules.

The 15 v5 device entrypoints are available through both ICD instance proc lookup
and device proc lookup:

```text
vkCreateCommandPool      vkDestroyCommandPool
vkAllocateCommandBuffers vkFreeCommandBuffers
vkBeginCommandBuffer     vkEndCommandBuffer
vkCreateEvent            vkDestroyEvent         vkGetEventStatus
vkCmdSetEvent
vkCreateFence            vkDestroyFence         vkGetFenceStatus
vkWaitForFences           vkQueueSubmit
```

Calls resolve to real Android device entrypoints at v5 device creation. Missing
required entrypoints destroy the newly created device and fail; v4 requires only
its existing lifecycle entrypoints. No extra device extension is enabled or claimed.

This is deliberately **not a complete Vulkan ICD**. All new allocator callbacks,
pNext, nonzero pool/begin/event/fence flags, secondary or multiple command buffers,
inheritance, other pipeline-stage masks, repeated recording, multiple event commands,
multiple submits/command buffers, semaphores and waits longer than five seconds are
rejected. Count-zero optional semaphore arrays are ignored because their Vulkan
counts make them unused; nothing is transported. Command/fence resets, host
vkSetEvent/vkResetEvent, timeline/synchronization2, idle API entrypoints and all
unrelated device commands remain unavailable. vkDeviceWaitIdle is **broker-internal
cleanup only**, never exposed as a new proxy API.

Broker command states are initial -> recording -> executable -> pending ->
completed. An end/record failure or destruction of the referenced event makes
the command invalid. Submit requires an ended command, a live event, matching
enabled queue family and an unused fence when supplied. Completion comes from a
real fence wait/status, or internal device idle during teardown. Completed commands
are not resubmitted in this subset; fences are not reset/reused. Individual pending
child frees/destroys are rejected. Pool destruction frees its non-pending commands;
device destruction frees remaining children before destroying the real device.

## Failure, timeout and pending lifetimes

Every staged failure exits nonzero and cleans only created objects. Failure to
submit never pretends the fence completed. Event RESET after a completed fence is
also a failure. `VK_ERROR_DEVICE_LOST (-4)` is named and printed exactly.

On `VK_TIMEOUT`, the diagnostic prints/flushed output immediately, retains its
pending flag and delegates child cleanup to device teardown. The broker calls
real **vkDeviceWaitIdle** before freeing pending resources, then destroys fence,
event, command buffer, pool, device and instance. Cleanup after disconnect, instance
teardown or broker stop follows the same rule, including submissions without a
fence. Teardown RPC receive timeouts are disabled in v5 so an ordinary socket
read deadline cannot race that cleanup acknowledgement. Other RPCs retain their
existing ten-second transport deadline. If the transport itself breaks, the
diagnostic fails; the broker retains ownership and drains on disconnect, so
remote cleanup may finish after the failing client has returned.

If idle returns an error other than SUCCESS/DEVICE_LOST, cleanup logs it and
retries with a short delay while retaining all potentially in-use handles. Device
loss is propagated unchanged; any other pending work is drained with internal idle
before child destruction. The
[Vulkan lost-device rules](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device)
permit cleanup after completed/lost waits, but still require explicit child destruction.

**Limitation:** the five-second fence timeout bounds the diagnostic wait, not the
whole cleanup. Vulkan's device-idle API has no timeout argument; a driver that
hangs can keep the broker/diagnostic in safe cleanup. Persistent idle allocation
errors also retain resources and delay teardown. We do not claim bounded total
exit and simultaneously free objects still used by the GPU. An Android process
kill remains platform-level recovery. Normal successful runs acknowledge that all
broker handles are gone before returning.

## Validation

```sh
python3 tools/mali-vulkan/test_probe.py
VULKAN_HEADERS=/path/to/Vulkan-Headers/include python3 tools/mali-vulkan/test_icd.py
GAMESCOPE_SOURCE=/path/to/pristine/gamescope-3.16.29 \
  VULKAN_HEADERS=/path/to/Vulkan-Headers/include python3 tools/gamescope/test_vk_enumerate_only.py
dbus-run-session -- python3 -m unittest discover -s tools/tests -p 'test_*.py'
bash -n tools/mali-vulkan/build-probe.sh tools/mali-vulkan/build-icd.sh tools/gamescope/build-in-arch.sh
actionlint -shellcheck= .github/workflows/build.yml
git diff --check
```

Checkpoint tests: **8 probe + 27 ICD/broker + 21 Gamescope = 56 passed**. All older
43 tests remain. New tests exercise actual host glibc libvulkan -> actual ICD ->
actual broker source -> explicitly labelled HOST MOCK Vulkan. The mock changes
the event only on queue completion, checks native ownership/lifetimes and balances
all child create/free counts. Coverage includes every requested object/state,
invalid/stale/wrong-type/wrong-parent ID, unenabled/incompatible queue family,
unended submit, double free, malformed/truncated and fragmented requests, older
protocol rejection, per-stage failures, missing entrypoints, timeout, device loss,
RESET-after-completion rejection, optional-fence submit, pending disconnect/stop
cleanup, repeated success and early Gamescope exit/no renderer dependencies.

Repository host suite: **296 passed, 1 skipped (297 run)** from a current temporary
copy without spaces under dbus-run-session. Two unrelated existing launcher tests
fail in this workspace's spaced path because a generated launcher invokes an
unquoted helper path; that code was not changed. Checkpoint suites pass in the
original workspace. One optional repository dependency test is skipped.

Build checks: AArch64 **glibc** ICD/loader/inventory/static probe and Gamescope's
submit translation unit cross-compile. The broker compiles and links as an actual
**Android/AArch64 shared library with NDK r27d (27.3.13750724), API 26**, -Wall
-Wextra -Werror and -Wl,-z,defs; LOAD segments are aligned to 0x4000. Host broker
syntax checks pass. The complete 27-test ICD/broker suite also passes with
the actual AArch64 glibc broker mock running under qemu-aarch64 and the real
host loader/proxy communicating across the socket. All Gamescope patches through 0116 apply/reverse with zero
fuzz; new patch source whitespace checks pass. actionlint 1.7.12, workflow YAML,
28 embedded shell scripts and embedded Python checks pass. Build/test outputs
are in /tmp, not substituted into APK assets.

The full packaged Gamescope executable, full Gradle APK and **real phone 4C** are
not built/run locally. CI performs actual packaged-binary help/missing-ICD checks
and staged/APK option-marker checks. Only real-phone acceptance proves Mali queue
execution; HOST MOCK success is not that proof.

## Exact APK build procedure

These local changes are intentionally uncommitted/unpushed. GitHub Actions uses
committed remote source, so dispatching the current remote branch cannot include
this work until the user makes the changes available there.

For a remote revision containing this work, open **Actions -> Build APK -> Run
workflow -> mali-gpu-experiment** (or use the following command). The same-run
Gamescope job rebuilds all patches through 0116, uploads gamescope-patched, and
the APK job overlays it even on a preload-cache hit. It freshly builds glibc
probe/ICD assets and the Android broker, checks all four Gamescope options in the
staged and APK binary, and emits the usual APK artifacts. Download the signed
APK for the installed package variant, retaining the compatible signing key.

```sh
gh workflow run build.yml --ref mali-gpu-experiment
gh run list --workflow build.yml --branch mali-gpu-experiment --limit 5
gh run watch RUN_ID
gh run download RUN_ID --dir /tmp/droiddeck-4c-apks
```

For a local APK that includes uncommitted work, from repository root on an AArch64
machine with Docker and the Android SDK/NDK/JDK 17 plus the repository's existing
asset/submodule prerequisites:

```sh
docker run --rm -v "$PWD:/work" -w /work menci/archlinuxarm:base-devel \
  bash tools/gamescope/build-in-arch.sh
sha256sum -c gamescope.tzst.sha256
mkdir -p app/src/main/assets/linuxfs
tar --use-compress-program=unzstd -xf gamescope.tzst -C app/src/main/assets/linuxfs
CC=aarch64-linux-gnu-gcc VULKAN_HEADERS=/usr/include tools/mali-vulkan/build-probe.sh
CC=aarch64-linux-gnu-gcc VULKAN_HEADERS=/usr/include tools/mali-vulkan/build-icd.sh
./gradlew assembleDebug --console=plain -PndkVersion=27.3.13750724
```

Use Docker sudo/chown as required on your host. An x86 host needs working ARM
container emulation or a separate ARM component builder; the component build
script expects AArch64 Arch. Debug signing must match the installed debug APK;
for an existing release installation, use the established release signing/build
process rather than uninstalling and losing the runtime. The pinned old Gamescope
release cannot run 4C, and replacing only the ICD/broker does not supply the option.

## Exact real-phone acceptance

```sh
adb install -r path/to/compatible-new.apk
adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.gpu.SystemVulkanBrokerActivity
adb logcat -v threadtime -s MaliVulkanBroker:I '*:S'
```

Replace only the package before `/` for a variant. Keep the installed Linux
runtime. Run the existing five actions (probe, ICD, Gamescope enumeration,
capabilities, 4B device test) first and retain their outputs. Press the separate
**Run Gamescope Vulkan submit test** button. Require:

```text
gamescope: Vulkan queue-submit test
physical device: Mali-G52
physical API: 1.1.131
queue family: 0 (verified graphics+compute)
queue obtained: yes
command pool created: yes
primary command buffer allocated: yes
event initial status: VK_EVENT_RESET (4)
vkBeginCommandBuffer: VK_SUCCESS (0)
vkCmdSetEvent recorded
vkEndCommandBuffer: VK_SUCCESS (0)
fence created: unsignaled
vkQueueSubmit: VK_SUCCESS (0)
vkWaitForFences: VK_SUCCESS (0)
vkGetFenceStatus: VK_SUCCESS (0)
event final status: VK_EVENT_SET (3)
observable device command executed: yes
fence destroyed
event destroyed
command buffer freed
command pool destroyed
logical device destroyed
instance destroyed
normal Gamescope rendering remains disabled
exit 0
Gamescope Vulkan submit test via Linux/proot exit code=0
```

Additional real feature/extension/queue inventory and proxy logging is expected.
Broker logcat must show device/queue/pool/command/event/fence IDs, enabled family,
event RESET status, begin, CmdSetEvent record, end, submission/result, wait/result,
fence status, event SET status, each child free/destroy, device/instance destruction
and Android loader closure. No raw driver pointers need to appear.

Repeat the new action twice; rerun the old 4B and enumeration actions; press
**Stop broker and close** and confirm socket removal and broker cleanup. A timeout,
device loss, missing stage/SET status, missing destruction, or nonzero child exit
fails 4C. Preserve complete merged output and logcat on failure. Do not open a
normal session. Manual invocation after the button stages assets, from the existing
guest shell **under the app UID**, is:

```sh
test_dir=/data/user/0/com.droiddeck.launcher/files/mali-vulkan
env VK_DRIVER_FILES="$test_dir/mali_proxy_icd.json" \
    VK_ICD_FILENAMES="$test_dir/mali_proxy_icd.json" \
    MALI_VULKAN_BROKER_SOCKET="$test_dir/broker.sock" \
    MALI_VULKAN_SUBMIT_TEST=1 VK_LOADER_LAYERS_DISABLE='*' \
    /usr/local/bin/gamescope --vk-submit-test
```

## Remaining normal Gamescope blockers

The physical Vulkan 1.1.131 API still fails the unchanged >=1.2 selection gate.
Gamescope's timeline/scalar/optional-FP16/image-format-list functionality has
Vulkan 1.1 extension equivalents supported in the captured inventory, but none
of their rendering command transport is implemented here. `shaderInt16=1` is
confirmed and `VK_KHR_image_format_list=PRESENT`; the corrected audit is in
[DEVICE_TEST.md](DEVICE_TEST.md).

`VK_EXT_robustness2` is absent, and Gamescope actually uses null descriptors.
`dynamicRendering`, `presentId` and `presentWait` were NOT QUERYABLE because their
native API/extensions were absent. Normal create-info still requests those bits.
Queried DMA-BUF export was unavailable; AHB import/export inventory does not
implement a compositor transport. Proxy rendering/resources/pipelines/semaphores,
WSI and presentation remain unimplemented. 4C changes none of those requirements
or capabilities and implements no checkpoint 4D.
