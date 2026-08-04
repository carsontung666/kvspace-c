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

## C++ API

```cpp
#include "kvspace/shm.h"

kvspace::ShmOptions options;
options.max_entries = 65536; // fixed at creation
options.max_size = 1ULL << 30;
options.engine = kvspace::ShmEngine::HashBox;

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
- The region is shared and remains attachable until explicitly destroyed.
  POSIX shared memory is normally volatile across a machine restart.
- Owner-death recovery validates referenced blobs, rebuilds allocator free
  lists, repairs hash counters, and reclaims unreachable allocations. It does
  not claim disk durability or transactional rollback of a process killed in
  the middle of a multi-key semantic operation.
- All KV operations are serialized by one region-wide mutex. The fixed hash
  engine favors predictable point operations over ordered prefix scans.
- Region ABI mismatches are rejected instead of being opened with a different
  build layout.
