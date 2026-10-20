# Development CPU shared staging transport

This replaces pixel-bearing RPCs for newly mapped persistent Gamescope SHM
staging slots. The original broker Vulkan allocations, GPU copy commands,
synchronization, rendering and AHB presentation remain. PanVK/Steam are
unchanged. Phone correctness and performance are not yet established.

## Audit and choice

Existing call path: `MaliStagingBackend::create` → private
`vkDroidDeckMapStagingMALI` → `interop_map_memory(download=0)` → private CPU
mirror returned to Gamescope. Gamescope copies/converts SHM pixels directly
into that pointer and arms the slot with `vkDroidDeckStagingMALI`.
`renderer_QueueSubmit` holds the connection lock and uploads eligible coherent
mirrors before sending its original submit RPC. The existing pool proves GPU
completion before slot reuse, resize or destruction.

The mirror itself can therefore become shared before returning the pointer.
There is no arena or additional proxy copy. Only the private whole-allocation
staging mapper qualifies. Ordinary MapMemory, noncoherent mappings, larger
allocations and already existing mappings keep their original behavior.
Dirty ranges and ordering are unchanged; each selected range gets a synchronous
notification. The broker's mapped Vulkan memory remains a separate allocation.

The existing AF_UNIX/SOCK_STREAM transport supports SCM_RIGHTS with its unchanged
same-app-UID SO_PEERCRED check. The broker creates a memfd, retains a read-only
MAP_SHARED mapping and sends one CLOEXEC FD. The proxy verifies size/seals,
maps it read/write and closes the FD. No guest-supplied file is mapped by the
broker. Android kernel/SELinux acceptance still requires phone validation;
memfd creation denial declines registration and retains bulk transport.

## Protocol, bounds and lifetime

Version 7 gains optional opcodes; existing packets are unchanged:

| Opcode | Request body | Result |
| --- | --- | --- |
| 92 PROBE | Empty, disposable connection | Prefix + version 1, max bytes, max regions; close connection |
| 93 REGISTER | device:u32, memory:u32, bytes:u64 | Prefix ACK, then byte `S` with exactly one SCM_RIGHTS FD |
| 94 WRITE | device:u32, memory:u32, offset:u64, bytes:u64, sequence:u64 | Prefix ACK after CPU copy |
| 95 DROP | device:u32, memory:u32 | Prefix ACK; remove CPU mapping, preserve Vulkan mapping |

All integers use existing little-endian helpers. Raw FD numbers never appear
in packet bodies. Missing, extra or truncated descriptors fail the connection;
all delivered descriptors are closed. Both created/received FDs use CLOEXEC.
A one-byte ancillary envelope avoids partial stream writes resending rights.

Limits: three regions per connection across all logical devices, matching the
existing pool; 1 byte–16 MiB per region. File size rounds to 64 bytes plus a
64-byte sequence area outside pixels. F_SEAL_GROW/SHRINK/SEAL prevent resizing.
The existing 16-client limit makes the absolute shared pixel bound 768 MiB;
one normal compositor uses at most 48 MiB. Registration must match an existing
whole mapped host-visible/coherent allocation on that connection. Pending GPU
use is rejected as before. Range validation uses subtraction to avoid overflow.

Three 4 MiB slots use approximately 12 MiB + 3 pages (4 KiB-page example),
replacing the old 12 MiB private mirrors. Both processes map the same pages.
Native Vulkan storage is unchanged. Fallback request scratch may still be
required by other maps.

Ordering, under the existing connection lock:

1. Gamescope finishes writing the staging pointer.
2. The proxy release-stores the next lock-free 64-bit sequence outside pixels,
   then sends the exact range and sequence.
3. The broker validates identity, range, map/coherence, pending-use state and
   next sequence, and checks publication with an acquire-load.
4. It memcpy's that range into original mapped Vulkan memory, rechecks the
   sequence, and ACKs only after copying completes.
5. Only an exact successful ACK permits the existing QueueSubmit.

No asynchronous work or GPU wait is introduced. Callers must continue to avoid
concurrent source mutation during synchronous uploads. Sequence overflow or
notification/protocol/ACK failure prevents submission; it never sends pixels
through bulk RPC on an already registered mapping.

FDs close after registration; mmaps own subsequent lifetime. Existing guarded
Unmap releases both mappings. Local mmap/alignment failure sends DROP before
allocating a private fallback. Disconnect/Stop releases CPU source maps even
if existing GPU teardown quarantines native allocations: GPUs never reference
the CPU source map. Proxy cleanup is idempotent; re-Launch creates fresh regions
and sequences.

## Copies, traffic and fallback

Before: SHM → mirror; mirror → request buffer; broker request → mapped Vulkan
memory, plus socket/kernel copies. After: SHM → shared mirror; shared mirror →
mapped Vulkan memory. The GPU's staging-to-texture operation is unchanged.

A contiguous 4 MiB staging upload currently needs two 2 MiB writes, totaling
4 MiB + 128 bytes of request/reply traffic. Shared mode needs one 32-byte range
body: 48-byte request + 28-byte ACK = **76 bytes**, no pixel payload. The
unchanged submit adds 72 bytes: 4 MiB + 200 versus 148 bytes for these operations.
One-time probe/registration and unrelated RPCs are excluded. Multiple dirty
ranges/slots produce multiple small notifications.

Default is disabled. At Mali normal-session launch the APK uses
`Context.getExternalFilesDir("mali-vulkan")` and checks
`/sdcard/Android/data/com.droiddeck.launcher/files/mali-vulkan/shared-staging.enabled`, setting
`MALI_VULKAN_SHARED_STAGING=1` for the proxy session. An absent marker, a directory
with that name, or unavailable external storage selects `0`. The marker is a regular
file; its contents are ignored. Package-suffixed builds use their own external-files
directory. Steam-child environment
explicitly strips it. Change the marker while normally stopped; mappings are
never switched under callers.

A disposable capability probe prevents old-broker unknown-opcode rejection
from breaking the actual session. Unsupported capability, memfd denial, region
limit, oversized allocation or local mmap/alignment failure keep existing
2 MiB staging writes. Ambiguous transport failure after registration stops
upload instead of hiding corruption. Remove the marker and Stop/Launch to
restore the original path.

## Journal and interpretation

Two scopes extend the existing unchanged every-sixteenth detail sampler:

- `proxy_shared_rpc_begin/end`: synchronous range notification and ACK
  validation, existing client/attempt/scope, memory object and range offset.
- `broker_shared_copy_begin/end`: actual memcpy into mapped Vulkan memory,
  existing native connection scope, memory object and offset.

Existing mirror, rpc_*, broker receive/dispatch/reply, native vk_submit and
publication events remain. WRITE's 32-byte body uses the existing identity
classification. Analyzer namespace anchors and unique complete synchronous RPC
containment accept either bulk or shared scopes. No new cross-process ID or
timestamp-proximity guessing. Interval union, loss checks and completeness
rules remain. API time is not GPU execution time.

`stage_spans` summarizes both new scopes with wall/p95, CPU samples and paired
off-CPU. `upload_queue_decomposition.per_queue` includes shared notification/copy
counts. Expected for one contiguous shared slot: 1 shared RPC, 1 shared copy,
0 bulk RPCs, 0 payload mapped copies, 1 native submit. Other maps may still bulk.
The legacy 92-opcode aggregate snapshot ABI is unchanged and excludes opcodes
92–95: use the journal for shared RPC counts/time, not its legacy aggregate.
Memory-write role accounting records copied bytes; shared notifications do not
increase the legacy socket pixel upload byte count.

## CI and device A/B procedure

Build on GitHub Actions after the change has been reviewed and pushed to
`mali-0.5-port`. Use the APK and Gamescope component from the same source SHA/run.
The matched broker/proxy must both contain the final optional opcodes. No additional
Gamescope patch is needed: the existing private persistent-staging mapper returns
the shared pointer, and 0215 already strips transport controls from child processes.

Use the release package `com.droiddeck.launcher` below (adjust for a package suffix).
Stop the session before changing the marker or arming the journal. This affects
new sessions and newly mapped slots; there is no live transport switch.

For baseline A, remove the marker:

```sh
adb -s SERIAL shell rm -f /sdcard/Android/data/com.droiddeck.launcher/files/mali-vulkan/shared-staging.enabled
python3 -B tools/mali-vulkan/frame_journal_control.py arm --serial SERIAL
```

Launch the normal Mali session. Keep the same workload, window size and Steam
client choice for both captures. In the running session's Terminal:

```sh
droiddeck-mali-diagnostics journal --seconds 10
```

After capture completion, on the host:

```sh
python3 -B tools/mali-vulkan/frame_journal_control.py pull --serial SERIAL --output shared-A.zip
python3 -B tools/mali-vulkan/frame_journal_control.py disarm --serial SERIAL
```

Stop normally. Enable B explicitly, then arm before launching again:

```sh
adb -s SERIAL shell mkdir -p /sdcard/Android/data/com.droiddeck.launcher/files/mali-vulkan
adb -s SERIAL shell touch /sdcard/Android/data/com.droiddeck.launcher/files/mali-vulkan/shared-staging.enabled
python3 -B tools/mali-vulkan/frame_journal_control.py arm --serial SERIAL
```

Require the startup log to select `MALI_VULKAN_SHARED_STAGING=1`, proxy
`capable=yes`, and qualifying mappings with `transport=shared mmap`. Capability
or registration fallback is safe but does not prove successful shared operation.
Capture the same workload using `journal --seconds 10`, pull as `shared-B.zip`,
then disarm. This procedure does not change CPU affinity or PanVK configuration.

Each export retains previous captures. Extract the Gamescope and Android JSONL
pair for exactly one capture into its own directory. Do not combine runs:

```sh
python3 -B tools/panvk/analyze_frame_journal.py /path/to/one/capture > shared-summary.json
```

Require both footers to report `dropped=0`, `complete=true`, readiness before
start, positive joined frames and positive complete selected/broker samples.
For complete sampled queues, inspect
`upload_queue_decomposition.per_queue[*].client_stage_span_counts.proxy_shared_rpc`
and `broker_stage_span_counts.broker_shared_copy`: B must have positive counts,
with persistent-slot `proxy_bulk_rpc` and `broker_mapped_copy` absent or approaching
zero. A must retain pixel bulk writes. Unrelated mappings can still bulk-upload.
Dropped records invalidate exact decomposition; a zero count from an unsampled or
incomplete queue is not evidence that work disappeared.

Verify pixels, text, colors, resizing, scrolling, repeated pool reuse, Stop/Launch
and disconnect cleanup. Keep PanVK optional client selection unchanged; if it was
enabled, repeat the Phase 5 CEF proof independently. Queue-submit wall time is not
GPU execution time, rpc_receive includes blocking/scheduling, offCPU includes all
descheduling, Android latch is not physical scanout, and release-fence time is the
first observed readability.

Finally Stop, remove the marker, Launch and repeat A to prove restoration of the
2 MiB fallback. Leave shared staging disabled and the journal disarmed after tests.
Only device evidence can establish Android memfd/SELinux compatibility and speedup.
