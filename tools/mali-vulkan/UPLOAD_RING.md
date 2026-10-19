# CP7P explicit compositor upload-ring ranges

The source audit and implementation below concern the existing normal Mali
session only. The previous registry optimization is phone-validated beyond two
minutes: live traversal/lookup remained bounded despite over 20,000 retained
records, about 36 FPS sustained, 34 frame RPCs, zero-copy, no pool drops.
This range optimization still needs CI and phone validation; no FPS estimate
follows from the byte reduction.

## Source audit before changing uploads

The audit used Gamescope 3.16.29 `rendervulkan.cpp/.hpp` with the current carried
patch stack through 0122 applied. The reconstructed files matched the prior
source cache byte for byte. Patch 0123 adds the hooks described here.

`CVulkanDevice::createScratchResources` creates one Mali 512 KiB
`TRANSFER_SRC | UNIFORM_BUFFER`, binds at memory offset zero, and maps the whole
host-visible/coherent allocation. The allocation can include driver padding.
It is separate from every persistent SHM staging-pool allocation.

The normal loop in `mali_normal_main.cpp` imports the committed SHM buffer with
`vulkan_create_texture_from_wlr_buffer`, then constructs a single-layer frame
and calls the existing connector's Present. The importer copies/converts the
complete source into its safe staging slot and records a buffer-to-image copy
from **that staging buffer**, then submits U. This branch neither calls
`uploadBufferData` nor records a uniform descriptor referencing the ring.
There are **zero new compositor-ring stores before steady-state U**.

`vulkan_mali_normal_present` calls `vulkan_composite` with the existing AHB
output override. The Mali gate rejects multi-layer, YCbCr, effects, upscaling,
blur and color-management combinations. The accepted BLIT branch calls
`uploadConstants<BlitPushData_t>` once, between U and R. The template reserves
`sizeof(data)`, sets the descriptor offset/size, and copies the complete record.
For this ABI, the packed record has eight layers of scale (8), offset (8),
opacity (4), CTM (48) bytes, followed by ten four-byte fields:
`8 * (8 + 8 + 4 + 48) + 10 * 4 = 584` bytes. The implementation reports and
marks **`sizeof(data)`**, not a hardcoded protocol size.

In a steady normal frame the returned offset is **0**: the preceding frame's
existing wait on the latest R resets `m_uploadBufferOffset` to zero. U does not
advance it. Thus the only normal ring store is **R: [0, sizeof(BlitPushData_t))**.
Startup/helper uploads have their own stores/submissions and are not assumed
clean or included in this steady-state prediction.

`uploadBufferData` aligns the offset to the existing Mali alignment
`max(16, minUniformBufferOffsetAlignment)`, validated as a power of two at
renderer initialization. If a reservation would cross the 512 KiB end, the
existing `waitIdle(false)` proves the latest submission complete and resets the
offset before allocation. A record is never split across the physical end.
Alignment gaps are not stores. Mali's scratch uniform descriptor has exactly
`offset=m_renderBufferOffset, range=m_renderBufferSize`; the GPU's logical UBO
range is the copied structure, not its padded allocator stride.

All four uses of `uploadBufferData`, and all accesses to the mapped ring pointer,
were inspected. Writers are:

| Writer | Full logical extent stored and subsequently consumed |
| --- | --- |
| `CVulkanCmdBuffer::uploadConstants` | `sizeof(PushData)` at returned offset, consumed by its uniform descriptor. Generic non-Mali effects can write several records per submission. |
| `vulkan_update_luts` | Two contiguous memcpy calls: `[base, base+lut1d_size)` and `[base+lut1d_size, base+lut1d_size+lut3d_size)`, consumed by two buffer-to-image copies. Marked as one combined range after both stores. |
| `vulkan_create_flat_texture` | All `width * height * 4` bytes, filled in a BGRA loop, then consumed by a buffer-to-image copy. |
| `vulkan_create_texture_from_bits` | All `width * height * DRMFormatGetBPP(format)` bytes via memcpy, then consumed by a buffer-to-image copy. |

The latter three helpers submit and wait through their existing paths. All
four receive reservation/completion hooks, including initialization helpers.
Other renderer modes use the same functions but never register the private
opt-in, so their coherent transport is unchanged. There are no other direct
writers through `m_uploadBufferData` in the audited source.

U does not consume previous constants. R consumes its newly stored record;
older submitted regions remain in real broker memory after their acknowledged
upload. Not retransmitting unchanged bytes does not discard them. The existing
R completion wait precedes next-frame reuse, and wrap retains its existing real
wait. No additional claim of GPU completion is made by a range marker or ACK.

## Private local contract and epochs

`vkDroidDeckUploadRingMALI`, exposed in both ICD instance and device lookup,
provides PROBE, REGISTER, RESERVE, MODIFIED and SEAL. Gamescope registers only
when both `m_maliDiagnostic` and `mali_normal_active()` are true. Registration
validates the live memory and buffer identities, acknowledged binding at zero,
real transfer/uniform usage, buffer length, whole coherent mapping and lack of
staging opt-in. It is an explicit assertion by the instrumented exclusive
writer, never automatic recognition of arbitrary coherent memory.

The allocator reports RESERVE before returning the CPU pointer; each of the
four writers reports MODIFIED after its full stores. A missing/mismatched
completion marker leaves uncertain or unfinished state. `submitInternal` seals
the current generation for the actual command, including the intentionally
empty U epoch. All operations are client-local and issue no RPC.

The proxy merges overlapping and adjacent extents. Gaps remain separate: the
current workload needs no gap-merging heuristic, and sending padding is not
necessary. Wrapped tail/head extents remain individually bounded. Thirty-two
pending/merged extents bound bookkeeping storage, **not allocation size**;
exceeding bookkeeping capacity falls back to the complete mapped upload.
Offset/length checks use subtraction to avoid overflow, reject zero lengths,
and bound both the logical ring and actual allocation/mapping. A saturated
epoch counter also conservatively keeps full fallback.

QueueSubmit uses the existing synchronous bulk MB_MEMORY_WRITE transport for
each merged extent, including its range checks, chunk limits and exact ACK
checks. It sends the real submission only after every required upload succeeds.
Dirty state retires only after **both** upload ACKs and successful QueueSubmit,
and only if its generation has not changed. A newer modification preserves
the entire older set conservatively as well. Failed writes, partial transfers,
bad ACKs or failed submissions retain the required state and existing errors.

Registration starts uncertain and requires one successful full-map baseline.
Missing seals, unfinished stores, invalid bounds, unknown operations or range
capacity exhaustion use the old full-map path. A successful sealed full upload
can restore certainty when there are no unfinished reservations. If the API
is missing/unavailable, Gamescope logs that once and leaves the map ordinary.
Unmap invalidates the opt-in only after successful upload/unmap; failed cleanup
retains it. Device cleanup frees the private tracking allocation along with
the existing retained resource records. No lifetime/ownership waits change.

Ordinary mappings always retain full coherent upload-before-submit, initial
coherent downloads, and the existing flush/invalidate/unmap behavior. The proxy
cannot observe arbitrary CPU stores, and no optimization assumes otherwise.

## Expected transport and profiler

| Steady-state transfer | Before | Expected after |
| --- | ---: | ---: |
| Ring U | 524,288 bytes, 1 RPC | 0 bytes, 0 RPCs |
| SHM U | 230,400 bytes, 1 RPC | 230,400 bytes, 1 RPC |
| Ring R | 524,288 bytes, 1 RPC | 584 bytes, 1 RPC |
| Total | 1,278,976 bytes, 3 writes | **230,984 bytes, 2 writes** |

Expected savings: **1,047,992 bytes/frame**, with **33 instead of 34** frame
RPCs solely because an explicitly sealed clean U needs no ring transfer.
The initial full baseline, driver allocation padding, other workloads and safe
fallbacks are excluded from this steady-state prediction.

Existing two-second memory-write rows preserve last offset/length, min/max,
RPC/ACK counts, requested/acknowledged bytes and wire bytes. New `ring-ranges`
rows separately attribute U/R logical store bytes, marked/merged range counts,
precise/full-fallback epochs, transmitted/requested bytes and bytes avoided
against a whole mapped upload. An empty sealed U is printed even with zero
write calls. Logical bytes count stores (overlap can count twice); merged
lengths count their union. Epoch/avoided counters describe attempted uploads;
the separate ACK bytes distinguish partial/failed transfers.

Fallback reasons aggregate as baseline/bounds/capacity/unfinished/unsealed.
No per-range RPC or per-frame log is added. Existing opcode RTT, broker
CPU/service, staging, waits and registry instrumentation remain intact.

## Lightweight checks and remaining validation

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_upload_ring test_bulk test_registry test_frame_cost test_perf test_wait test_staging
```

These compile only tiny extracted helpers with stub transport/Vulkan, not the
project. Tests cover real device API resolution/availability and registration
guards, initial full baseline, distinct U/R stores and no U resend, empty U,
overlap/adjacency/bridging/gaps/wrap, zero/bounds/overflow, incomplete/unknown
state, bounded bookkeeping fallback, partial-write/bad-ACK/submit failures,
newer-epoch retention, unmap failure/success, arbitrary full maps, unchanged
230,400-byte SHM uploads and the old three-write fixture. Existing bounded
registry stress and staging/wait/ownership regression helpers remain active.
The new private header is also compiled in isolation as C++20/no-exceptions.

Patch parsing and zero-fuzz application to the reconstructed renderer are
required locally. Full Gamescope/proxy packaging remains for CI. The next
phone run should confirm the startup availability line, warm-up fallback only,
zero U ring bytes, actual R extent, 33 RPCs/frame, unchanged forwarded output
wait, 230,400 SHM bytes, zero-copy/no drops, bounded registry/descriptor CPU for
at least two minutes, no corruption and safe Stop/relaunch. Measure sustained
FPS; no output fence, ownership or synchronization optimization is included.
