# kvspace-c

Linux shared-memory implementation of the
[`kvspace-go`](https://github.com/array2d/kvspace-go) KVSpace interface. It
provides a C++17 API and a C ABI without requiring an external service.

## Design

The shared format implements the four candidate layouts from `deepx-design`:

| Engine | ID | Index and allocation strategy |
| --- | ---: | --- |
| `ArtBump` | 1 | Compressed ART; four fixed node slabs; append-oriented payload arena reclaimed by `Compact` |
| `ArtBox` | 2 | Compressed ART; four fixed node slabs; reclaiming variable-size payload allocator |
| `HashBox` | 3 | Fixed-capacity open-addressed hash; reclaiming key/value allocator |
| `TrieBox` | 4 | Fixed 256-way byte trie; one fixed node slab; reclaiming value allocator |

### Choosing an engine

The engines are alternatives, not tiers of the same implementation. Their
trade-offs come from both the index and the crash-consistent commit path.

| Engine | Useful when | Main cost |
| --- | --- | --- |
| `ArtBump` | The workload is read-heavy or has a bounded lifetime with an explicit maintenance window | Replaced variable-size data remains in the active raw zone until `Compact` |
| `ArtBox` | A general-purpose starting point for path-shaped keys and sustained updates | ART traversal and copy-on-write path updates |
| `HashBox` | Point reads dominate and `max_entries` is kept small | Each mutation materializes a complete inactive table image, so writes include full-capacity table work |
| `TrieBox` | Keys are short and the fixed per-byte traversal is acceptable | Every node occupies 1032 bytes and updates copy the byte path |

`HashBox` lookup is expected constant-probe after hashing, but hashing still
reads the complete key. More importantly, each mutation clears the inactive
table, scans the complete active table, re-inserts the live entries, publishes
the selector, and then clears the old table. Those clears and scans impose an
unavoidable capacity-linear component; validation, sorting live intervals, and
collision behavior add further work. Configured capacity is therefore part of
the write-performance contract even when only a few keys are live.

No engine should be selected from point lookup latency alone. Replay the
application trace at its intended capacity, key distribution, and read/write
mix. `ArtBox` is the conservative starting point when no trace is available
because it combines compressed paths with reclaiming value storage and does
not rebuild a capacity-sized table on each write. `ShmOptions` currently
initializes `engine` to `HashBox`, so applications should set the engine
explicitly rather than rely on that initializer default.

The persisted header records the engine ID, engine ABI, and an engine-specific
layout hash. `Attach` detects the stored engine; an attach/open that requests a
different engine fails before interpreting that engine's metadata. Engine IDs
are never redirected to another implementation.

One `ShmClient` owns the public semantics shared by all four engines. The
storage layer contains:

- compressed ART grow/shrink nodes (4/16/48/256), including the 37-child
  Node256 shrink threshold and arbitrary-length compressed prefixes;
- a fixed 256-way byte trie and a fixed-capacity double-buffered hash table;
- engine-specific persistent allocators: four size-class node free lists and
  two append zones for `ArtBump`, four `FixedBlock` allocators plus one `Box`
  allocator for `ArtBox`, and one `FixedBlock` plus one `Box` allocator for
  `TrieBox`;
- a common notification Blob heap with journaled split and coalesce replay;
  recovery derives its free lists from the immutable span chain;
- a process-shared robust mutex and monotonic process-shared condition
  variable;
- a versioned region header with magic, endian, layout, and bounds checks;
- explicit Index values, including independent `/a` and `/a/` keys;
- LinkIndex, ExtIndex, and multi-value LIFO Notify/Watch semantics.

Pointers are never stored in the shared region. Persistent references are
engine-specific: `ArtBump` nodes use 64-bit absolute region offsets; `ArtBox`
nodes use 32-bit tagged allocator-local IDs and biased `Box` offsets; and
`TrieBox` uses 32-bit allocator-local node IDs and biased `Box` offsets. Other
region references are absolute offsets. Every form is independent of the
process's virtual mapping address.

The fixed-section `ArtBox` and `TrieBox` records are manually encoded
little-endian bytes rather than native C/C++ structs. `ArtBox` node payloads
are exactly 48/104/472/1048 bytes and `TrieBox` nodes are 1032 bytes; any
`FixedBlock` allocation header is outside those payloads. The current
`ArtBump` slab payload classes are 64/168/664/2072 bytes and are covered by its
engine ABI and layout fingerprint. See `DESIGN_RESOLUTIONS.md` for the complete
persistent formats and resolutions of details left open by the design drafts.

### Directory semantics

Directories remain explicit semantic records. An ordinary directory stores an
`Index` value, an `ExtIndex` stores local children plus a fallback directory,
and a `LinkIndex` can redirect directory resolution. An ART child edge is one
branching byte, not necessarily one direct child name: path compression can
place a directory boundary inside a node prefix, and an empty directory has no
child from which it could be inferred.

`List(prefix, false)` reads the local direct-child set. `List(prefix, true)`
may merge children from an `ExtIndex` fallback after link resolution. Neither
operation treats an internal ART or trie node as a directory record. Returning
`k` child names is O(k) plus the output bytes; explicit directory records avoid
a full key-space scan but do not make output construction O(1).

## C++ API

```cpp
#include "kvspace/shm.h"

kvspace::ShmOptions options;
options.max_entries = 65536; // fixed at creation
options.max_size = 1ULL << 30;
options.engine = kvspace::ShmEngine::ArtBox;

auto owner = kvspace::ShmClient::Create("example", options);
owner->Set("/jobs/one", kvspace::XValue::Str("ready"));

auto worker = kvspace::ShmClient::Attach("example");
auto value = worker->Get("/jobs/one");
worker->Notify("/done", kvspace::XValue::Int64(1));

owner->Close();
worker->Close();
kvspace::ShmClient::Destroy("example");
```

`Create` fails if the name already exists. `Attach` requires an existing
region. `Open` atomically creates or attaches. A name such as `example` or
`/example` uses POSIX shared memory; an absolute name containing another slash,
such as `/var/tmp/example.kvshm`, uses a regular mmap-backed file.

All four engines use fixed sections: creation extends and maps the backing
object to `max_size`, while `initial_size` is only a validated compatibility
lower bound. Entry and pending-notification-key limits are fixed at creation;
exceeding a limit raises `ErrCapacity` without resizing the index. A blocked
`Watch` does not reserve a shared queue slot, so a killed waiting process
cannot leak notification capacity.

`ArtBump::Compact` preflights an inactive bump zone, copies every live prefix
and value into it, clones the live ART, and publishes the cloned root with one
release-store. Owner-death recovery rolls back before that root publication and
rolls forward after it. Cleanup then resets the old bump zone, so compaction
restores contiguous allocation capacity rather than merely making individual
dead spans reusable. `Clear` is stronger—after values and notification queues
are cleared it resets the complete object heap and both bump zones.

## C ABI

The C API is declared in `include/kvspace/kshm.h`. Values crossing the ABI are
complete Go-compatible TLV frames:

```text
[1 byte kind length][kind][4 byte array length LE][4 byte raw length LE][raw]
```

`None` is the zero-byte frame and follows `kvspace-go`: Set and Notify accept
it. Get-missing and Watch-timeout also return `None`, so callers that need to
distinguish presence must use List or application-level metadata. Results
returned by `kshm_get`, `kshm_list`, and
`kshm_watch` are owned by the caller and must be released with the matching
`kshm_*_free` function. Failures return a `kshm_status_t`; diagnostic text is
available from the calling thread through `kshm_last_error()`. Initialize every
`kshm_options_t` with `kshm_options_init`; its `struct_size` field prevents a
caller and library from silently disagreeing about the options ABI.
`kshm_disconnect` wakes blocked watchers without freeing the opaque handle;
`kshm_close` disconnects and then frees it. `kshm_compact` invokes the
engine-specific maintenance operation.

## RDMA boundary

The current region is a local shared-memory ABI, not an RDMA wire protocol.
The common header contains native glibc `pthread_mutex_t` and `pthread_cond_t`
objects, and local readers rely on the region mutex while writers can reclaim
old copy-on-write objects immediately after publishing a new root or table.
A one-sided reader cannot participate in that mutex and may still hold an old
offset when the referenced object is reused. Registering the complete mapping
for remote read or write would therefore be unsafe.

A compatible RDMA design can keep one backing object while separating its
roles:

```text
local control: pthread objects, allocator journal, recovery state
RDMA data:     stable offsets, immutable payloads, version/incarnation, CRC
```

Only the data section should be registered and exposed read-only. A remote
point Get must read and validate metadata, fetch the immutable value, re-read
the version, and retry if the version, incarnation, key, or checksum changed.
Published objects need epoch-, lease-, or RCU-based delayed reclamation because
one-sided readers are invisible to the host. Host restart, compaction, or
region replacement must advance an epoch, rotate the remote key, and invalidate
cached offsets.

Mutations and semantic operations such as `List`, `DelTree`, `Link`/`ExtIndex`
resolution, `Notify`, and `Watch` should remain host RPCs. This follows the
same practical boundary used by
[Pilaf](https://www.usenix.org/system/files/conference/atc13/atc13-mitchell.pdf):
one-sided reads over a deliberately verifiable layout, with server-side writes.
[FaRM](https://www.usenix.org/system/files/conference/nsdi14/nsdi14-paper-dragojevic.pdf)
likewise shares physical regions only with explicit version and reclamation
protocols, while
[HERD](https://www.cs.cmu.edu/~dga/papers/herd-sigcomm2014-readable.pdf)
demonstrates that an RDMA request mailbox plus host execution is a valid choice
for small-key operations. A future remote view may use a fixed hash or cuckoo
projection without making that projection a second semantic source of truth.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The tests cover codec vectors, all four engines, the 13-method semantic surface,
capacity and allocator reuse, adaptive ART nodes and long prefixes, randomized
differential operations, the C ABI, independent processes, concurrent writers,
Notify/Watch, split-journal crash cut points, open/unlink races, and robust-mutex
owner recovery.

## Guarantees and limits

- Supported platform: little-endian x86_64 Linux with the GNU libc
  process-shared robust pthread ABI.
- Creation extends the backing object to the complete fixed `max_size` before
  mapping and initialization. It does not rely on accessing a mapping past the
  current end of the backing object and growing the object underneath live
  mappings.
- The backing object survives an individual process exit and remains named
  until explicitly destroyed. POSIX shared memory is normally volatile across
  a machine restart. Reusing native pthread synchronization bytes after every
  process has unmapped the object is part of the supported Linux/glibc ABI, not
  a portable POSIX guarantee; deployments that require a strict lifetime
  boundary should keep one host process attached.
- Owner-death recovery validates referenced blobs, rebuilds allocator free
  lists, repairs hash counters, and reclaims unreachable allocations. It does
  not claim disk durability or transactional rollback of a process killed in
  the middle of a multi-key semantic operation.
- All KV operations are serialized by one region-wide mutex. The fixed hash
  engine favors point reads but its current full-table image commit does not
  provide capacity-independent writes.
- `Destroy` is an offline lifecycle operation. Existing mappings can continue
  using the unlinked object, while a subsequent Create with the same name can
  create a different object.
- Region ABI mismatches are rejected instead of being opened with a different
  build layout.
