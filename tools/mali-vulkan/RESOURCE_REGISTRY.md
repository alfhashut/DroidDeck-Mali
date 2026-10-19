# CP7P live resource registry

Phone attribution established that the old proxy registry's retained history
was on the normal frame's hot path: last-submit records grew from 574 to 14,452
while live objects stayed at 76. Lookup average grew from 257 to 12,503 visited
records, descriptor wall time from 0.490 to 62.386 ms/frame, and FPS fell from
41.66 to 10.01. The Android compositor still reported zero-copy, no pool drops,
and ~1–1.5 ms render_scene. These are supplied phone observations before this
fix; performance after the fix still requires phone validation.

## Before

`proxy_instance.logical` owns devices. Each `proxy_logical.resources` was a
singly linked owning chain of every published `proxy_resource`, newest first.
`submit_new`, `interop_publish` (also used by `renderer_new`, descriptor-set
batches and AHB triples) appended to that chain only after native success.
Successful destruction set `live=0`; it did not unlink the record. Only
`proxy_free_resources` at device/instance cleanup freed records and mirrors.

The same chain served unrelated purposes:

| Operation | Old traversal | New traversal |
| --- | --- | --- |
| `submit_find` | Every historical record until matching live address/type | Device-local live pointer-hash bucket |
| `UpdateDescriptorSets` | Repeated `submit_find` for sets, buffers, views and samplers | Same validation and descriptor payload; each lookup uses live index |
| Coherent upload discovery | All historical records, filtering live mapped memory | Live active list only, same coherent/staging selection |
| Successful render submit staging-arm reset | All historical records | Live active list only |
| SYNC/AHB token lookup | All historical records, filtering live token/type | Live active list only |
| Command/descriptor pool destruction | All historical records for child pool ID | Live active list, retiring matching live children |
| Individual destruction | Linear handle lookup, then `live=0` after ACK | Live index lookup, then index/active removal after the same ACK |
| Device/instance cleanup | Owning chain | Owning chain, including retired records |

With H historical records and L live records, hot handle lookup was O(H), and
each whole-registry submit/token/child scan was O(H). Retired records therefore
affected descriptor CPU even though descriptors themselves remained persistent.

## Structures and complexity after

`resource_registry.h` retains the original resource kinds, metadata, atomic
`live`, loader word at offset zero, and pointer-handle representation. It adds
intrusive links for two independent live structures. `proxy_logical.registry`
contains:

- `owned`: the original `next` chain, for storage ownership/cleanup only.
- `active`: a doubly linked list of live objects. Whole-set operations are
  O(L), independent of historical record count.
- `index[256]`: live objects hashed by mixed pointer bits. Lookup never
  dereferences an incoming handle. Type and exact pointer equality are checked
  in a device-local table, preserving cross-device rejection. Expected cost is
  O(1 + L/256), worst case O(L), independent of H.
- Separate owned, indexed and active counts. Retired count is owned minus
  active. Insertion and individual retirement are O(1); index back-links avoid
  a second bucket search on removal.

The table costs 2 KiB/device on the 64-bit path. Chaining imposes no object
limit, resizing allocation, per-frame table allocation or new publication
failure. The supplied normal workload's ~76 live objects has low bucket load.
Four extra pointers per record keep ownership, active traversal and index
membership separate.

## Lifetime and ordering

Records remain allocated until the existing device/instance cleanup. Vulkan
handles are their addresses, and `BeginCommandBuffer`, `EndCommandBuffer`,
`interop_command` and recording entrypoints directly read command records.
`submit_find` also returns a pointer after releasing the mutex. Immediate frees
would make such references unsafe and could recycle an old handle address.
Retaining storage preserves these existing assumptions. Retired handles stay
unresolvable through the index, and direct command access still sees atomic
`live=0`. IDs/addresses are not recycled by this change.

Proxy descriptors and command recording serialize broker IDs; they do not
retain additional client resource pointers in proxy submission/descriptor
caches. Local call variables can retain pointers; their storage remains valid.
AHB/sync tokens and command/descriptor parent associations also store broker
IDs. The broker's separate native command/resource tables and references are
unchanged. Its pending-use, descriptor/image-reference, mapped/bound-memory
and ownership guards still decide whether native destruction is legal.

All publication, retirement, lookup and active traversal use the existing
connection mutex. Successful native destruction is the only trigger for
logical retirement, including sync close/AHB release. A rejected operation,
pending use or failed acknowledgment retains live membership and follows the
existing error path. Command/descriptor pool acknowledgments retire their live
children too. Partial/failed creation remains unpublished and locally freed;
multi-object results are validated before any publication, as before.

The submit upload walk now holds the connection mutex over active discovery
and uploads, since retirement mutates active links. The existing upload code
has a caller-locked entrypoint plus an ordinary mapping wrapper. Range checks,
bulk limits, payload bytes, full coherent ranges, per-chunk ACKs and errors are
unchanged. No new resource snapshot allocation is needed. The mutex is released
before the existing queue-submit RPC helper acquires it. Every required upload
must still succeed before submit; failure unlocks and returns without submit.
The post-submit arm reset also traverses active objects under the same mutex.

No GPU resources are freed earlier, no completion is inferred, no timeline or
producer fence is changed, and no RPC is added or removed. Three mapped writes
and 34 in-frame RPCs/frame remain the normal workload expectation. Pacing,
staging mappings/pool, output wait, AHB ownership/releases and zero-copy are
unchanged. Diagnostic v5/v6 paths share the corrected registry; their wire and
Vulkan behavior stay unchanged.

Owning storage still grows with successful creation history, intentionally.
This pass removes that history from hot operations; it does not implement
record reclamation. Final cleanup frees all live and retired records/mirrors
exactly once, then clears every registry head and count.

## Profiling and validation

The existing two-second `MaliPerf proxy-lifetime` row keeps its write/lookup/
submit fields and adds snapshot-device gauges:

```text
owned-records=... live-index=... active-traversal=... retired-records=...
```

`nodes-visited` and `lookup-avg/max` now count only live hash probes.
`last-submit-nodes/live/mapped` now counts active nodes actually traversed,
live nodes and live mapped allocations. Gauge collection copies existing
counts under the local snapshot's existing lock; no query RPC or extra owning
chain scan. These fields remain outside the private snapshot/wire ABI.

Small tests compile only registry/lifecycle and extracted mapped-upload/submit
helpers with in-memory RPC/Vulkan stubs:

```sh
cd tools/mali-vulkan
python3 -B -m unittest -v test_registry test_bulk test_frame_cost test_perf test_wait test_staging
```

- 10,000-frame lookup stress: 76 stable handles, 20,000 retired/replacement
  records. Every stable handle's probe count remains exactly its pre-churn
  count. Wrong kind/device and destroyed/arbitrary handles fail safely.
- Deliberate hash collisions verify head/middle/tail removal and repeated
  retirement. Simulated broker ID reuse cannot revive an old pointer handle.
- 5,000-frame real upload/submit-helper stress: 20,000 retired records, five
  live records including three mapped allocations. Every submit visits five
  nodes; armed staging and ordinary coherent uploads preserve three writes per
  U/R pair; idle staging and retired mappings are never uploaded.
- Actual lifecycle helpers cover pool children, retained raw references,
  failed destroy ACKs, mapped-memory free rejection, SYNC/AHB token retirement,
  allocation/native failures, invalid IDs and partial descriptor/AHB creation.
  Allocation/free accounting balances at cleanup, including repeated cleanup.
- Existing bulk error/order/bounds, staging/watermark, wait and perf/ABI tests
  remain applicable. Gamescope patches are unchanged and parse without builds.

After CI builds matching artifacts, run normal Launch at 60 Hz for at least
two minutes. Verify 34 RPC/frame, three writes/frame, the same single forwarded
output wait, zero-copy and zero pool drops. Live-index/active counts should
remain near the prior live baseline while owned/retired counts can grow.
Lookup probes and submit active visits must remain stable; compare descriptor
and compositor CPU across early/late warmed windows and confirm sustained FPS
no longer collapses. No particular FPS is promised by host tests.
