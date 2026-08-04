# Design resolutions and persistent contract

This file is the normative bridge between the four design drafts and the C/C++
implementation. A storage engine is not complete merely because a component is
compiled or unit-tested: the production engine factory must use the layout
defined here, and an attach after a process restart must read that same layout.

The reviewed source snapshots are:

- `deepx-design` `195bc36343d95669a299c6238e654d439166cab9`:
  `kvspace-c.md`, `kvspace-c-art-boxmalloc.md`, `kvspace-c-hash.md`, and
  `kvspace-c-memkv-reuse.md`;
- `kvspace-go` `95eed23cf74fe53ea474b1e899808836ad33a107`, which is
  the public KVSpace semantic oracle;
- `blockmalloc` `6bb8e16d2a9a5f755ad4ae2e711405feacc452ba` and
  `boxmalloc` `49c343477c11244fc0a8e53f6f0e3da12def7581`, which are
  algorithm references rather than a native-struct persistence ABI.

## Common semantics

- Directories are stored as explicit Go-compatible `Index` or `ExtIndex` TLV
  values. `/a` and `/a/` are independent keys.
- A stored `None` is present. It therefore wins over an `ExtIndex` fallback,
  although the public Get result for stored None and a missing key is the same
  `None` value.
- `List(path, false)` returns only local children. `List(path, true)` appends
  non-colliding fallback children after local children.
- Notify/Watch is a retained multi-value LIFO queue, matching the reviewed Go
  backend. One Watch consumes one value; timeout returns None.
- Link resolution is limited to 64 rewrites.
- `DelTree` is defined by actual stored keys, not by a possibly user-replaced
  Index children list. A directory deletes every key with that exact byte
  prefix. A non-directory path deletes the exact key and the `path + "/"`
  prefix. Every deleted key has its corresponding parent Index member cleaned.

## Persistence and failure rules

- Engine records are encoded field-by-field in little-endian order. Native
  C/C++ structs, pointers, enums, and bitfields are not an engine-record ABI.
- The process-shared robust mutex and condition variable are native platform
  objects in the common region header. A region is therefore attachable only
  by the same supported libc/platform ABI; the engine ID, engine ABI, common
  layout fingerprint, and engine layout fingerprint reject mismatches.
- This contract uses common `RegionPrefix.version=4` and the exact common
  layout-hash domain `0x434f4d4d4f4e3034` (`COMMON04`) XOR
  `(sizeof(RegionPrefix) << 1)`. Versions through 3 and any other common hash
  are rejected; there is no in-place migration. Engine ABI numbers need not
  change when only this common discriminator changes.
- Mutations are serialized by the common robust mutex. Tree updates are
  immutable path copies; a hash update uses a fixed-capacity staged table.
  The committed root or table selector is published last with release ordering.
- Every public read, enumeration, statistics read, queue operation, mutation,
  allocator operation, and recovery operation holds that same robust mutex.
  Engine stores are private implementation objects and are never callable
  without the owning `Region::Guard`. There are therefore no lock-free readers
  that can retain an old root or table after publication; publication followed
  by reclamation while the writer still holds the mutex is safe.
- Nodes and Box allocations retired by a successful mutation are reclaimed
  only after publication. If a process dies before or after publication,
  recovery treats the committed root/table plus live queue references as the
  authority and rebuilds allocator metadata. A normal successful update must
  not leak every old COW path.
- The common queue heap and engine budget are disjoint fixed sections. The
  common heap cannot allocate from, span into, or reserve any engine section.
  Individual engine keys, values, prefixes, and nodes therefore cannot
  accidentally fall back to the common Blob allocator.

## C++17 mapped-object lifetime boundary

- The C++17 build treats the language changes in
  [P0593R6](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html)
  as a retroactive defect correction. The
  [Prague 2020 WG21 minutes](https://isocpp.org/files/papers/N4870.pdf)
  record that P0593R6 was accepted as a Defect Report. This is the project's
  required object-model interpretation; it is not a claim that any supported
  GCC or Clang release implements every part of P0593R6 in every context.
- Every successful full-region mapping, whether for Create, Attach, a remap,
  or a test fixture that performs production typed access, calls
  `StartImplicitLifetimes(base, bytes)` after the mapped extent has been
  validated and before its first typed access. The helper's abstract operation
  is the same-range `memmove(base, base, bytes)`: under P0593R6, `memmove`
  implicitly creates suitable implicit-lifetime objects immediately before
  copying, while the identical source and destination preserve the
  representation. A bare
  `reinterpret_cast<T*>` is not this boundary; P0593R6 explicitly says such a
  cast alone does not trigger implicit object creation.
- `StartImplicitLifetimes` is a separately compiled translation unit and object
  target. LTO is disabled for it, it is not inlined into callers, and callers
  retain an opaque external call before typed access. On each supported
  compiler/toolchain, the helper itself is optimized to no physical access of
  the mapped range: its machine code must neither load nor store a mapped byte,
  call an out-of-line copy routine, nor walk the range. A disassembly/source
  review gate rejects a toolchain that emits physical traffic. Thus this
  abstract lifetime boundary does not read untrusted pages or race on live
  persistent bytes before the robust mutex can be acquired. GCC's documented
  [`noipa`/`noinline` attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Attributes.html)
  and Clang's documented
  [`noinline` attribute](https://clang.llvm.org/docs/AttributeReference.html#noinline)
  describe the relevant compiler controls; the separate non-LTO object remains
  mandatory even where an attribute is unavailable or spelled differently.
- A `BlobHeader` whose address did not already hold a live `BlobHeader` in the
  current mapping is explicitly begun with placement new before typed member
  access. This includes a header at a newly consumed heap tail, a split
  remainder, and a relocation or migration destination. The new object is then
  populated from checked scalar values or from a validated local codec object.
  Existing attached headers are not placement-new reconstructed: their
  lifetimes come from the mapping boundary, and reconstructing them would
  discard or render indeterminate their persisted representation.
- Recovery scratch is never used as typed `BlobHeader` storage. Its backing
  `vector<uint64_t>` elements remain alive solely to own suitably aligned
  storage, and recovery accesses their object representations through byte
  pointers. To inspect or update a virtual `BlobHeader`, it first bounds-checks
  48 bytes, copies them with `memcpy` into an automatic local `BlobHeader`
  whose lifetime has already begun, validates and updates that local object,
  and copies its representation back to the scratch bytes. It never obtains a
  `BlobHeader*` by casting a `vector<uint64_t>`, `byte[]`, or other scratch
  allocation. Persistent writes still occur only in the recovery Apply phase.
- Every persistent atomic wrapper is admitted only by compile-time gates:
  standard-layout, trivial, trivially copyable, and trivially destructible;
  exact 4-byte/4-byte or 8-byte/8-byte size/alignment; and
  `__atomic_always_lock_free` for that width. Every attached address is also
  checked at runtime for the wrapper's natural alignment before its first
  atomic operation. Values are encoded into a local wrapper and transferred
  only with the GNU/Clang `__atomic` load, store, or compare-exchange builtins;
  a runtime fallback lock is forbidden for process-shared publication. The
  [GCC atomic-builtin contract](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)
  is the implementation reference for the generic operations and the
  always-lock-free gate.
- Persisting `pthread_mutex_t` and `pthread_cond_t` bytes is an explicit
  `x86_64-linux-gnu` GNU libc ABI extension of this format, not an ISO C++ or
  portable POSIX promise. POSIX permits a `PTHREAD_PROCESS_SHARED` object to be
  used through another mapping of the same object, as specified in the
  [POSIX synchronization-object rules](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html),
  but it does not define a cross-libc, cross-architecture persistent byte ABI.
  The native size/alignment/offset golden, libc compatibility checks, mapping
  lifetime boundary, and common-layout fingerprint are therefore all required;
  passing only one of them is insufficient.
- Rejected substitutes are: direct cast-and-dereference; `memset`, which both
  overwrites the representation and is not the selected P0593 lifetime
  operation; and any physical full-region copy before locking. P1631R1
  [Object detachment and attachment](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1631r1.pdf)
  remains a proposal and is not this contract's language or library primitive.
  Placement new is required at the individual newly created or migrated
  `BlobHeader` sites above, but placement-new reconstruction of an attached
  full region is likewise not a substitute for the mapping boundary.

## Common native header ABI

- The normative supported-platform golden is little-endian
  `x86_64-linux-gnu` with the GNU libc pthread ABI, 4096-byte pages,
  `sizeof/alignof(pthread_mutex_t)=40/8`, and
  `sizeof/alignof(pthread_cond_t)=48/8`. The recorded golden was compiled with
  glibc 2.35. Another build is compatible only if its libc guarantees the same
  process-shared robust-pthread object ABI and every `sizeof`, `alignof`, and
  `offsetof` below matches; equal pthread sizes alone are not sufficient.
- `RegionPrefix` has size 64 and alignment 8, with this complete native physical
  layout. Scalar bytes are little-endian on the supported platform.

  | field | offset | bytes | alignment |
  | --- | ---: | ---: | ---: |
  | `magic[8]` | 0 | 8 | 1 |
  | `version:u32` | 8 | 4 | 4 |
  | `endian:u32` | 12 | 4 | 4 |
  | `init_state:u32` | 16 | 4 | 4 |
  | `header_size:u32` | 20 | 4 | 4 |
  | `engine_id:u32` | 24 | 4 | 4 |
  | `engine_abi:u32` | 28 | 4 | 4 |
  | `common_layout_hash:u64` | 32 | 8 | 8 |
  | `engine_layout_hash:u64` | 40 | 8 | 8 |
  | `region_size:u64` | 48 | 8 | 8 |
  | `region_max:u64` | 56 | 8 | 8 |

- `RegionHeader` has size 1472 and alignment 64, with this complete physical
  layout. `table_offset[2]` contains two consecutive `u64` values. The trailing
  outer-struct padding is reserved zero.

  | field | offset | bytes | alignment |
  | --- | ---: | ---: | ---: |
  | `prefix:RegionPrefix` | 0 | 64 | 8 |
  | `page_size:u64` | 64 | 8 | 8 |
  | `entry_limit:u64` | 72 | 8 | 8 |
  | `table_capacity:u64` | 80 | 8 | 8 |
  | `table_offset[2]` | 88 | 16 | 8 |
  | `root_offset:u64` | 104 | 8 | 8 |
  | `node_count:u64` | 112 | 8 | 8 |
  | `entry_count:u64` | 120 | 8 | 8 |
  | `tombstone_count:u64` | 128 | 8 | 8 |
  | `queue_limit:u64` | 136 | 8 | 8 |
  | `queue_capacity:u64` | 144 | 8 | 8 |
  | `queue_offset:u64` | 152 | 8 | 8 |
  | `queue_count:u64` | 160 | 8 | 8 |
  | `heap_offset:u64` | 168 | 8 | 8 |
  | `heap_top:u64` | 176 | 8 | 8 |
  | `heap_last:u64` | 184 | 8 | 8 |
  | `heap_limit:u64` | 192 | 8 | 8 |
  | `engine_offset:u64` | 200 | 8 | 8 |
  | `engine_size:u64` | 208 | 8 | 8 |
  | `engine_live_bytes:u64` | 216 | 8 | 8 |
  | `allocator_journal` | 224 | 64 | 8 |
  | `free_heads[64]` | 288 | 512 | 8 |
  | `reserved_allocator_slots[68]` | 800 | 544 | 8 |
  | `generation:u64` | 1344 | 8 | 8 |
  | `recovery_count:u64` | 1352 | 8 | 8 |
  | `active_table:u32` | 1360 | 4 | 4 |
  | `corrupt:u32` | 1364 | 4 | 4 |
  | `mutex:pthread_mutex_t` | 1368 | 40 | 8 |
  | `notify_cond:pthread_cond_t` | 1408 | 48 | 8 |
  | reserved outer padding | 1456 | 16 | 1 |

- The 64-byte `allocator_journal` at absolute header offset 224 has alignment 8
  and this complete sublayout.

  | field | relative offset | absolute offset | bytes | alignment |
  | --- | ---: | ---: | ---: | ---: |
  | `offset:u64` | 0 | 224 | 8 | 8 |
  | `original_span:u64` | 8 | 232 | 8 | 8 |
  | `requested_span:u64` | 16 | 240 | 8 | 8 |
  | `checksum:u64` | 24 | 248 | 8 | 8 |
  | `state:u32` | 32 | 256 | 4 | 4 |
  | `reserved32:u32` | 36 | 260 | 4 | 4 |
  | `reserved64[3]` | 40 | 264 | 24 | 8 |

- Prefix magic is the exact eight bytes `KVSHM01\0`; `version=4`,
  `endian=0x01020304`, ready `init_state=0x4b565231`, and
  `header_size=1472`. With `sizeof(RegionPrefix)=64`, the unchanged common hash
  is `0x434f4d4d4f4e3034 XOR 0x80 = 0x434f4d4d4f4e30b4`, stored as little-endian
  bytes `b4 30 4e 4f 4d 4d 4f 43`. This is the existing `COMMON04`
  discriminator, not a new common format.
- The build must `static_assert` every size, alignment, and offset in these
  tables, including the start and end of the generic-head array, the reserved-
  slot array, and the native pthread fields. A supported-platform fixture test
  independently checks the prefix bytes and all non-pthread header offsets,
  attaches an accepted `COMMON04`
  fixture, and rejects a changed header size, offset binding, or discriminator.
  Native mutex/condition contents are initialized by pthread and are not a
  portable hard-coded byte pattern.

## Global logical limits and fixed-section bootstrap

- ArtBump, ArtBox, HashBox, and TrieBox Create require
  `max_entries=N>0` and `max_queues=q>0` before any backing-object mutation and
  store those values exactly as `entry_limit=N` and `queue_limit=q`.
  `queue_capacity` is derived only from `queue_limit` by the checked formula in
  Common notification storage; neither limit is inferred from physical slab,
  table, Box, raw-zone, or common-heap capacity. Attach likewise requires both
  stored limits to be nonzero before traversing either logical data structure.
- For every normal engine mutation, before its first persistent engine
  allocation or write, lookup determines whether the logical key already
  exists and checked arithmetic proves the projected staged entry count is at
  most `entry_limit`. Inserting a new key requires the prior count to be less
  than the limit; replacing an existing value, including stored None, remains
  legal at the limit; deletion reduces the projected count and later reuse of
  that quota is legal. Each later internal update applies the same check before
  its own allocation/write. Physical spare slots or table cells never waive the
  logical quota. Clean Attach independently derives the authoritative logical
  entry count, requires it to equal stored `entry_count` and be at most the
  limit, and does not repair a mismatch. Owner-death recovery derives and checks
  the same count before its first repair write and overwrites the stored counter
  only after the complete recovery plan succeeds.
- These four engines support exactly runtime page size `P=4096`; Create requires
  the host page size to equal 4096 and stores exactly 4096, and Attach requires
  both the current host value and stored `page_size` to equal 4096. They are
  fixed-section engines with `region_size==region_max==R`. Before Create calls
  `ftruncate`, `R` must convert without loss to both `size_t` and nonnegative
  `off_t`, and all common and selected-engine geometry must already have been
  computed successfully using checked addition, multiplication, alignment, and
  subtraction. In particular, `page_up(x,P)` first checks `x+(P-1)`, every
  section end is checked, `R>=H` is proved before forming `R-H`, and the derived
  split satisfies `H<=E<=R` with the engine's minimum tail geometry.
- Only after that entire immutable geometry plan succeeds may Create call
  `ftruncate(fd,R)`. Only after `ftruncate` succeeds may it map exactly the full
  `[0,R)` object, and only after that full mapping succeeds may it perform the
  first persistent initialization write. Thus invalid limits, page geometry,
  representability, capacity, `ftruncate`, or mapping failure cannot expose a
  partially initialized header or access an unmapped fixed section.
- Attach first uses `fstat`; it does not initially map `R` from untrusted file
  bytes. If the file is shorter than 64 bytes it rejects without reading a
  prefix. Otherwise it obtains the 64-byte prefix with a bounded `pread` or
  equivalently bounded temporary mapping and validates, before any body access,
  magic/init state, `version`, `endian`, `header_size`, `common_layout_hash`,
  `engine_id`, the selected `engine_abi`, and its `engine_layout_hash`.
  It also requires `region_size==region_max==R` and lossless `size_t`/`off_t`
  representability. Only an accepted 1472-byte header size permits a bounded
  read of the complete common header, for which `fstat.st_size` must first be at
  least 1472; that local copy supplies the stored page and immutable geometry
  scalars. After their checked validation, Attach requires `fstat.st_size>=R`,
  maps the full `[0,R)` region, and only then may initialize/use the robust
  pthread objects or dereference any queue, common heap, engine header, slab,
  table, Box, or raw-zone byte.
- Logical-limit tests reject `N=0` and `q=0` before `ftruncate` or any header
  write and exercise both limits at 1. For each of the four engines they fill
  exactly `entry_limit`, verify a new-key limit+1 Put leaves the entire region
  byte-for-byte unchanged, verify replacement (including None transitions) at
  the limit, delete one entry, and reuse exactly that quota. Queue tests do the
  analogous fill, new-key limit+1 byte-unchanged check before any Blob
  allocation, existing-key Push at the limit, final Pop/delete, and tombstone
  reuse. Clean-Attach and owner-death fixtures independently derive 0, 1,
  exact-limit, and limit+1 counts; limit+1 is corruption before repair, even
  when physical tables, slabs, Box data, or common heap have spare capacity.
- ABI tests independently mutate `version`, `endian`, `common_layout_hash`,
  `header_size`, `engine_id`, `engine_abi`, and `engine_layout_hash`. Crossed
  pairs cover common version 4 with an old common hash and common version 3 with
  the new COMMON04 hash, plus a current engine ABI with its old layout hash and
  an old engine ABI with the current hash. Guarded and truncated legacy bodies
  prove a discriminator mismatch is returned before any engine/slab read.
  Separate bootstrap cases cover file lengths 0, 1, 63, 64, 1471, and `R-1`, a
  non-4096 host/stored page, an unrepresentable huge `R`, and injected
  `ftruncate`/mapping failure, with no SIGBUS and no persistent header write.
  Golden little-endian bytes remain necessary, but cannot by themselves prove
  the implementation avoided native record stores; acceptance also requires a
  source-level review gate confirming every manually encoded persistent prefix,
  common scalar, queue/table slot, allocator/engine header, journal, and engine
  record is loaded/stored field-by-field through explicit little-endian codecs;
  only the explicitly native pthread objects are exempt.

## Persistent allocator formats

All allocator fields below are little-endian. Magic, version, immutable
geometry, reserved-zero bytes, and the enclosing engine layout fingerprint are
validated before mutable allocator state is trusted.

### Common queue Blob heap

- Each Blob begins with this exact 48-byte header: `span:u64` at 0,
  `previous_span:u64` at 8, `next_free:u64` at 16,
  `previous_free:u64` at 24, `payload_size:u32` at 32,
  `magic:u32=0x4b56424c` at 36, `flags:u8` at 40, transient `mark:u8` at
  41, zero-reserved `u16` at 42, and zero-reserved `u32` at 44. Offsets are
  absolute; zero is the null reference. The complete valid flag byte is exactly
  0 for free or exactly 1 for allocated. Bits 1..7 are reserved zero; the
  former ArtBump bump/slab class meanings are semantically removed and any such
  bit is corruption. Split and coalesce operate only on exact free flag byte 0.
- Spans are 16-byte aligned, include the header, are at least 64 bytes, and
  form a complete boundary-tag chain from `heap_offset` to `heap_top`.
  The first span has `previous_span=0`; every later span's `previous_span`
  equals the immediately preceding span's `span`; and `heap_last` is zero only
  for an empty chain, otherwise it is the last header offset and
  `heap_last+last.span==heap_top`.
- For payload length `p`, define
  `minimum_span(p)=max(64,align_up(checked(48+p),16))`. In a clean state, an
  allocated queue Blob has `flags=1`, `mark=0`, both free links zero, valid
  magic/reserved fields, its exact logical `payload_size=p`, and
  `minimum_span(p)<=span<minimum_span(p)+64`; only the first `p` payload bytes
  are semantic and any remaining in-span slack is ignored. A clean free Blob
  has `flags=0`, `mark=0`, `payload_size=0`, valid magic/reserved and boundary-
  tag fields, and free links exactly as defined below. No other header-field
  combination is canonical.
- For every positive `u64` span, its unique bucket is
  `floor(log2(span))`, necessarily in 0..63. `free_heads[64]` contains only
  exact-flag-zero free spans from the corresponding bucket. Within a bucket,
  list order has no semantic meaning: the head may be any member, including the
  most recently inserted member in a LIFO implementation. Head
  `previous_free` and tail `next_free` are zero, every reciprocal link matches,
  and every free span occurs exactly once in exactly one list; an unused head
  is zero. Address monotonicity is not a clean-state invariant.
- Clean validation walks both the physical span chain and every free list,
  rejects cycles, duplicates, wrong buckets or reciprocal links, an allocated
  span in a list, or any mismatch between the physical free-span set and list
  membership. It also walks every authoritative queue reference and requires
  the set of all `flags=1` generic Blobs to equal that exact referenced-Blob
  set; accepted engines have no common-heap references, so an unreferenced
  allocation is a clean-state leak and is rejected. Ordinary Attach is entirely
  read-only: it validates an
  arbitrary legal order, including a legacy COMMON04 non-descending/LIFO order,
  and never rebuilds, sorts, or otherwise normalizes the lists. Owner-death recovery first
  completes the full read-only clean or active-journal-virtual boundary-chain,
  queue-reference, common-heap-journal, and scratch-resource validation and
  preflight. Only after that complete plan
  is known valid may it ignore all heads, free links, and transient marks, use
  the precomputed volatile reference set to turn every unreferenced allocation
  into canonical free state, coalesce adjacent free spans from low to high, and
  rebuild by scanning low to high and prepending. That recovery output is
  deterministically descending-address, but later valid LIFO mutations need not
  preserve that order.
- The common `RegionHeader` retains the exact physical offsets and total size
  of all fields previously used by ArtBump. The 64 `u64` slots formerly named
  `bump_free_heads` and the four `u64` slots formerly named `slab_free_heads`
  are one contiguous field named `reserved_allocator_slots[68]`, at the same
  starting offset and ending immediately before `generation`. Keeping either
  old semantic field name is nonconforming. Every slot is initialized to zero
  and must remain zero; after accepting the engine discriminator and before
  trusting allocator state, common Attach validates all 68 slots, and recovery
  repeats that validation before rebuilding the common heap. A nonzero slot is
  corruption. They are not free lists and no accepted engine may read or write
  allocator meaning into them.
  Preserving these slots, the 48-byte Blob header, and the 64-byte common heap
  journal means the common discriminator remains
  `RegionPrefix.version=4`/`COMMON04`. Existing accepted ArtBox ABI 3 and
  TrieBox ABI 4 regions created only generic queue Blobs and leave these slots
  zero, so they remain compatible. Legacy ArtBump ABI 3 and HashBox ABI 3 are
  rejected by their engine ABI/layout fingerprints before their old allocator
  metadata is interpreted. Removing or moving the physical slots, changing
  either common record size, or accepting an engine that can produce the old
  class bits would require a new common version and fingerprint.
- The common header has one exact 64-byte heap journal shared by split and
  recovery-time coalesce operations: source offset/left original span/
  operation span/checksum `u64` at 0/8/16/24, state `u32` at 32 (0 idle,
  1 split, 2 coalesce), a zero-reserved `u32` at 36, and three zero-reserved
  `u64` words at 40..63. A split checksum is exactly
  `0x4b5653504c495431 XOR source XOR left_span XOR requested_span`. A coalesce
  checksum is
  `0x4b564d4552474531 XOR source XOR left_span XOR right_span`; coalesce is
  valid only for two adjacent exact-flag-zero free spans.
- State 0 is the only idle state; state 1 and state 2 are the only active
  states and every other value is corruption. Journal payload fields are
  ignored while state is 0 except that the reserved `u32` and all three
  reserved words must be zero in every state. Before starting either operation,
  the caller must acquire-observe state 0. It writes source/span operands,
  checksum, the zero-reserved `u32`, and three zero-reserved words first, then
  release-stores the nonzero state last. Recovery acquire-loads state before
  consuming any active payload. It read-only classifies one pre-existing split
  or coalesce journal and incorporates its accepted final form into a virtual
  boundary-chain plan; it does not replay or clear the journal yet. That virtual
  chain participates in the same complete common queue/heap reference-set,
  repair, and scratch-resource preflight as an idle chain. Only after the entire plan
  succeeds may recovery replay and release-clear the journal, before applying
  the remaining planned repairs. No new split or coalesce may begin until state
  is idle.
- Before dereferencing a journal-derived address, replay requires the reserved
  `u32` and all three reserved words to be zero, the operation checksum to
  match, source and spans to be 16-byte aligned, each physical span to be at
  least 64 bytes, and every addition to be overflow-safe. Source `S` and every
  dereferenced Blob-header address must lie in `[heap_offset, heap_top)`; a
  checked terminal boundary may equal `heap_top`; and every checked span range
  must be wholly contained in `[heap_offset, heap_top)`. Span sizes `L`, `R`,
  and `Q` are lengths, not addresses. Blob headers used by replay require the
  Blob magic, zero reserved bytes, and only the contract-defined flag bits. Any
  failed predicate is corruption.
- For split state 1, let `S` be the source, `L` its journaled original span,
  `Q` the requested span, `R=L-Q`, `M=S+Q`, and `F=S+L`. Replay requires
  `Q>=64`, `R>=64`, all three lengths to be 16-byte multiples, and every
  checked range through `F` to lie in `[heap_offset,heap_top]`. The source has
  canonical unlinked-free fields (`next_free=previous_free=payload_size=0`,
  valid magic, flag/mark/reserved zero) and `span` exactly `L` or `Q`.
- Split replay anchors the source to its predecessor before writing anything.
  A read-only walk starts exactly at `heap_offset`, validates every complete
  boundary-tag header and checked span, and must reach `S` exactly rather than
  jumping past it; this proves `S` is a real physical boundary rather than a
  forged header inside payload bytes. If `S==heap_offset`, source
  `previous_span` is zero. Otherwise its prior-span value `PS` is aligned, at
  least 64, and no greater than `S-heap_offset`; the preceding walked header is
  exactly `P=S-PS`, with `P+predecessor.span==S` and
  `predecessor.span==PS`. The split never changes source `previous_span` or any
  predecessor byte.
- The exact final 48-byte remainder header at `M` is `span=R`,
  `previous_span=Q`, `next_free=0`, `previous_free=0`, `payload_size=0`, Blob
  magic `0x4b56424c`, `flags=0`, `mark=0`, and both reserved fields zero. If
  `F<heap_top`, the following header is otherwise a valid Blob and its
  `previous_span` is old `L` or final `R`; `heap_last` remains the independently
  validated physical tail. A second read-only walk begins at `F`, permits only
  that first old/final `previous_span`, then requires exact boundary tags through
  `heap_top` and requires the final walked header offset to equal `heap_last`.
  If `F==heap_top`, there is no following header and `heap_last` is old `S` or
  final `M`. `F>heap_top` is corruption. Together the prefix and suffix walks
  establish complete chain membership without reading `M` as a header until
  its accepted-stage rule permits it.
- After validating the journal, complete predecessor/source anchor, following
  header or tail, and one of the following accepted partial stages, replay has
  a complete plan. It performs no persistent write before this classification.

  | source `span` | interior `following.previous_span` / terminal `heap_last` | remainder bytes at `M` | accepted stage |
  | ---: | --- | --- | --- |
  | `L` | old `L` / old `S` | ignored and overwritten | before or during remainder write |
  | `L` | final `R` / final `M` | exact canonical remainder | links installed, source not published |
  | `Q` | final `R` / final `M` | exact canonical remainder | source boundary published |

  Every other combination is corruption. Replay idempotently writes the full
  canonical remainder, installs following `previous_span=R` or
  `heap_last=M`, release-stores source `span=Q`, and release-stores state IDLE,
  in that order. Before unlinking a clean source, the normal writer performs the
  same read-only prefix/suffix membership and old-boundary validation, while
  allowing the source's then-valid list links. It may then change only
  rebuildable free-list links while unlinking, populates and publishes the
  journal, and reruns the full unlinked-source active-state preflight before
  writing `M`; its boundary writes use the same order as replay. Only after
  state is idle is `M` inserted into its valid bucket free list (LIFO is
  permitted); owner-death recovery safely rebuilds that list if death occurs
  between those steps.
- For coalesce of a left span `L` at `S` and its right neighbor of span `R`,
  let `C=L+R`, `T=S+L`, and `F=S+C`. The normal writer performs the complete
  read-only validation described below while the pair still has valid list
  links, unlinks and normalizes both headers to canonical unlinked-free state,
  writes the journal payload, and release-publishes state 2. Active replay
  requires checked `F<=heap_top`. The left header retains its pre-existing
  valid `previous_span`, has all other canonical unlinked-free fields, and has
  `span` exactly `L` or `C`. The preserved right header at the journal-derived
  address `T` must remain byte-exact canonical unlinked-free state with
  `span=R` and `previous_span=L`, including when left `span=C` makes `T` a stale
  header inside the merged payload.
- Before writing anything, coalesce replay performs a full read-only prefix
  walk from exactly `heap_offset` to `S`. Every complete header, checked span,
  and boundary tag before `S` must be valid, the walk must reach `S` exactly,
  and the left header's `previous_span` must be zero at `heap_offset` or be
  anchored to the immediately preceding walked header exactly as for split.
  The prefix walk stops at `S`: if left `span=C`, replay locates and validates
  the preserved right header independently at journal-derived `T` rather than
  treating it as a live physical boundary.
- Replay also performs a full read-only suffix walk beginning at `F`. If
  `F<heap_top`, the following header is otherwise canonical and may have only
  old `previous_span=R` or final `previous_span=C`; after that first boundary,
  exact boundary tags must lead to `heap_top`, and the last walked header must
  equal `heap_last`. If `F==heap_top`, there is no following header and
  `heap_last` may be only old `T` or final `S`. `F>heap_top` is corruption.
  Only these three complete-state classifications are accepted:

  | left `span` | interior `following.previous_span` / terminal `heap_last` | accepted stage |
  | ---: | --- | --- |
  | `L` | old `R` / old `T` | before left publication |
  | `C` | old `R` / old `T` | left published, boundary not published |
  | `C` | final `C` / final `S` | final boundary published |

  The reverse mixed state `L` with a final boundary, and every other
  combination, is corruption. Replay performs no persistent write until the
  journal, preserved headers, complete prefix and suffix membership, stage,
  and scratch-resource plan have all passed validation. It then idempotently
  release-stores left `span=C`, release-stores following `previous_span=C` or
  `heap_last=S`, and release-stores state IDLE, in that order. Thus every crash
  point is one of the three rows above.
- Coalescing scans from `heap_offset` toward higher addresses. It uses one
  journal transaction for one adjacent generic-free pair, clears that
  transaction, then re-evaluates the same merged left span against its new
  right boundary. Only when it cannot merge does traversal advance. Absorbed
  right headers remain as normalized stale bytes inside the merged payload;
  journal payload remains stale and ignored after state 0. This fixes both
  traversal order and byte output for runs of three or more free spans.
- The valid-list-links premise above applies to a normal coalesce selected from
  clean free lists. Owner-death recovery has already ignored the old lists and,
  after its complete read-only preflight, first makes every to-be-free span
  canonical and unlinked and zeroes all free heads. Its recovery-only coalesce
  starts directly from each planned adjacent canonical-unlinked pair; it does
  not temporarily insert that pair into a list. For a run of three or more, it
  completes and clears one transaction, leaves the merged left and untouched
  next span canonical-unlinked, and immediately starts the next transaction at
  the same left boundary. No free span is inserted until all planned
  coalescing is complete. Death between transactions therefore leaves state 0
  plus stale/empty lists, which the next owner-death preflight ignores and
  deterministically plans again; death within a transaction uses the three-row
  coalesce replay table.
- Generic-Blob append, activation, and release add no persistent field or
  journal state. Before the first persistent write of any such operation, the
  normal writer, while holding the robust mutex, performs a complete read-only
  preflight of common static geometry and bounds, common-heap-journal state 0,
  the physical chain and tail, free-span/list set, every committed queue
  reference, and the caller's exact disjoint staged-allocation set. It does not
  scan any engine section: accepted engines cannot reference the common heap,
  and these operations neither read nor write engine bytes. It also
  requires the current flag-one Blob set to equal committed references union
  that staged set. It
  completes all checked size/capacity arithmetic and source-payload canonicality
  and preflights required file/mapping growth and volatile scratch allocation.
  Each append in a multi-allocation queue operation repeats this preflight with
  earlier clean staged allocations included. The append branch is used only
  after the validated free lists contain no usable span. No append, unlink,
  payload, authoritative-reference change, or allocator-metadata byte is
  written before this plan succeeds. Any ordinary error is cleaned back to a
  fully canonical state before unlocking; only owner death may expose the
  intermediate forms below.
- For append, let `H=heap_offset`, `S=heap_top`, `P=heap_last`, requested payload
  length `p`, `A=minimum_span(p)`, and `F=checked(S+A)`. Preflight requires the
  committed chain to end exactly at `S`, `P=0` iff `S=H`, otherwise `P` to be
  its derived last header, and checked `F` to fit both the mapped region and
  `heap_limit`. The complete logical payload is already assembled in volatile
  memory. At the uncommitted address `S`, the writer writes all 48 header bytes
  as `span=A`, `previous_span=0` for the first append or `P.span` otherwise,
  zero free links, exact `payload_size=p`, Blob magic, `flags=1`, `mark=0`, and
  zero reserved fields; it then copies all `p` payload bytes. Only after the
  complete header and payload exist does it release-store `heap_last=S`, and it
  release-stores `heap_top=F` last. The top store is the sole publication that
  extends the committed physical chain. A queue slot may publish `S` only
  after that top publication and any queue-specific payload validation.
- These are the only append crash rows, for both the first append (`S=H,P=0`)
  and a later append (`S>H,P!=0`):

  | completed writer step | `heap_top` | `heap_last` | committed extent |
  | --- | ---: | ---: | --- |
  | none, partial/full header, or partial/full payload | old `S` | old `P` | old chain only; bytes at and above `S` are unread garbage |
  | last publication | old `S` | staged `S` | old chain only; candidate at `S` is staging evidence, not a chain member |
  | top publication | new `F` | new `S` | extended chain through the complete new Blob |

  Owner-death recovery first acquire-loads allocator-journal state. State 1 or
  2 has priority: recovery uses the split/coalesce accepted-stage rules and
  virtual final chain/tail and does not run pending-append classification,
  because the single mutator cannot overlap append with an active journal. Only
  in state 0 does it acquire-load top and tail and walk only `[H,top)` to derive
  its last committed header `D` without consulting the loaded tail.
  `heap_last=D` is the ordinary committed form and recovery neither reads nor
  interprets any byte at or above top. The sole accepted state-0 tail mismatch
  is `heap_last==heap_top`; it represents a pending append or the byte-identical
  between-store public-Clear reset form specified below. Only then let
  `B=min(acquire(region_size),heap_limit)`: recovery first requires
  `checked(top+48)<=B` before reading the out-of-top candidate header, then
  requires its aligned span to be at least 64 and `checked(top+span)<=B` before
  treating the header as nonauthoritative staging evidence. Its
  `previous_span` must be zero when `top=H` and otherwise equal `D.span`; its
  magic and reserved fields must be exact; and it must be either the
  complete new allocated header above, including
  `span=minimum_span(payload_size)`,
  or the exact legacy COMMON04 pre-activation header (`flags=0`, zero payload
  size/free links/mark). It is never traversed, marked, counted, or exposed to
  reference validation. Any other tail relation or candidate is corruption.
  After the complete common queue/heap preflight, recovery rolls either
  producer of this exact row back by
  release-storing `heap_last=D`; `heap_top` remains unchanged and all candidate
  bytes remain meaningless outside it.
- Allocation from a free span uses `flags` as its existing one-byte publication
  and needs no new marker. After the validated unlink and, when needed, the
  completed split transaction, the chosen span is canonical unlinked free with
  its final span satisfying
  `minimum_span(p)<=span<minimum_span(p)+64`. While `flags=0` and
  `payload_size=0`, the writer copies the entire already-validated queue
  payload, then stores its exact payload size, and release-stores `flags=1`
  last. Only that final store commits an allocation,
  and only a complete activated Blob may subsequently be published by a queue
  reference. Conversely, normal release first removes the last authoritative
  reference under the queue publication contract, then release-stores
  `flags=0` before clearing payload size/mark/free links and inserting the span.
  Thus activation death leaves an unreferenced flag-zero span or a complete
  unreferenced allocation; release death leaves only an unreferenced flag-zero
  span, including the legacy COMMON04 `flags=0,payload_size!=0` window.
- Owner-death validation initially treats only span, predecessor tag, magic,
  reserved fields, exact `flags` 0/1, and the top-bounded interval as structural;
  it never follows persisted free links. In volatile scratch it builds the
  complete authoritative reference and interval set. Every referenced Blob
  must start at one exact committed header, have `flags=1`, zero free links,
  the exact reference-specific payload size and bytes, and all clean allocated
  fields except that transient `mark` is ignored and planned to zero. A
  flag-zero reference, sharing, overlap, bad payload, or reference at/above top
  is corruption. For every globally proven unreferenced Blob, regardless of
  exact flag 0 or 1, payload size, free links, mark, and payload bytes carry no
  semantic information and are not dereferenced; recovery plans that span as
  canonical free. This is the only activation/release classification and does
  not permit class bits or a structurally invalid header. Ordinary Attach never
  applies this owner-death relaxation: it still requires every header and free
  list to be fully canonical and mutually consistent without changing bytes.
- No append rollback, journal replay, header normalization, coalesce, list/tail
  rebuild, counter update, or persistent mark write occurs until the immutable
  geometry, virtual complete chain, pending-append evidence if any, all queue
  references, interval ownership, every final canonical header and
  coalesce step, and all scratch resources have passed one read-only preflight.
  Repair then rolls back a pending last-only append, replays any active
  split/coalesce, release-stores flag zero on each unreferenced allocation,
  canonicalizes all unreferenced headers and stale marks, performs the
  recovery-only coalesces above, and rebuilds tail, lists, and counters. A death
  after any repair write leaves only one of the same classified forms; the next
  owner repeats the full preflight before continuing.
  Because this protocol uses only the existing exact flag byte and top/tail
  fields and relaxes no clean Attach invariant, it does not change the COMMON04
  layout, hash, or version.
- Required tests terminate an ordinary recovery process after journal payload
  preparation, state-2 publication, left-span publication, interior following
  update, tail update, and state clear. They cover interior and tail pairs,
  three or more adjacent free spans, and a second termination while replaying
  recovery followed by successful third recovery. Split tests terminate after
  state-1 publication, partial and complete remainder writes, following/tail
  installation, source-span publication, state clear, and before free-list
  insertion; they exercise every accepted table row and reject every mixed row,
  a payload-forged source header, broken prefix/suffix membership, and bad
  predecessor/tail anchors. Coalesce tests exercise all three accepted rows,
  reject the reverse mixed row, validate the preserved stale right header, and
  reject broken prefix/suffix membership and predecessor/tail anchors.
  Append tests cover first and later appends and terminate after every partial
  and complete header/payload write, last publication, and top publication.
  With the old tail still installed, arbitrary poisoned bytes at and above top
  must remain unread; only the exact last-equals-top row permits the bounded
  staging-header inspection described above.
  Free-span reuse tests cover exact-fit and split allocations and terminate
  after unlink, every split row, partial/full payload copy, payload-size store,
  flags publication, reference publication, reference removal, flags clear,
  header cleanup, and list insertion. Each recovery repair kill point is killed
  again on the second recovery and must converge on the third. Golden legacy
  COMMON04 fixtures cover pending and committed append windows and
  `flags=0,payload_size!=0` release; corruption fixtures reject every other
  top/last relation, malformed or out-of-range pending candidate, a reference
  at or beyond top, a flag-zero or malformed referenced Blob, sharing/overlap,
  structural damage, class bits, and an otherwise canonical but unreferenced
  allocated Blob on ordinary Attach before changing one byte.
  Additional golden/corruption tests cover bucket boundaries, arbitrary/LIFO
  reciprocal free lists, a legacy COMMON04 non-descending fixture that ordinary
  Attach accepts without changing one byte, deterministic descending rebuild
  only after owner death, allocated/free canonical headers, exact queue-key
  length/bytes,
  message magic `0x4b564d53`, `16+value_len` and maximum bounds, corrupt
  state/checksum/reserved/bounds/Blob fields/flags, and rejection of old common
  versions/hashes.

### FixedBlock version 1

- Metadata is exactly 64 bytes: magic `KVBLOCK1` at 0..7, `version:u32=1` at
  8, `metadata_bytes:u32=64` at 12, `zone_bytes:u64` at 16,
  `payload_bytes:u64` at 24, `capacity:u32` at 32, mutable
  `high_water:u32` at 36, mutable `used_count:u32` at 40, mutable
  `free_head:u32` at 44, immutable FNV-1a geometry hash at 48,
  `header_width:u8` at 56, and zero bytes 57..63. `UINT32_MAX` is an empty
  free head.
- For each candidate header width `h` in `{2,4,8}`, let
  `C_h=floor(zone_bytes/(payload_bytes+h))` and let its local-capacity limit
  `L_h` be `2^14`, `2^30`, or `UINT32_MAX`, respectively. The canonical width
  is `w=min{h: 1<=C_h<=L_h}` and capacity is exactly `C_w`. Only constrained
  exact-capacity layouts such as ArtBox may simplify width selection to
  `MinimumHeaderWidth(C)`; general FixedBlock geometry must use both
  `zone_bytes` and `payload_bytes` because changing `h` also changes `C_h`.
- Every block header is an unsigned little-endian word: bit 0 `used`, bit 1
  `has_next`, and the remaining representable low bits hold `next_id` shifted
  left by two. An allocated block has no next ID; an unlinked free tail has
  `has_next=0` and zero next bits. Bits above 33 in an 8-byte header are zero.
  Payload begins immediately after the header; it has no native alignment
  requirement because codecs access it byte-by-byte.
- Normal allocate/free may leave mutable metadata torn on owner death.
  Recovery reads payload only through immutable geometry, derives the complete
  live-ID bitmap from the committed engine root/table, and then rewrites block
  headers, high-water, used-count, and the descending-ID free chain.

### Box version 1

- A Box metadata region begins with this exact 128-byte header: magic
  `KVBOXA01` at 0..7, `version:u32=1` at 8, `header_bytes:u32=128` at 12,
  `metadata_bytes:u64` at 16, canonical `data_bytes:u64` at 24, embedded
  FixedBlock metadata offset `u64=128` at 32, its block-zone offset `u64=192`
  at 40, block-zone bytes at 48, node payload bytes `u32=88` at 56, root ID
  `u32=0` at 60, root level/slot-count bytes at 64/65, zero bytes 66..71,
  immutable FNV-1a geometry hash at 72, and zero bytes 80..127. The embedded
  FixedBlock metadata occupies 128..191 and its node zone begins at 192.
- Each 88-byte metadata node is: parent ID `u32` at 0, parent slot at 4,
  level at 5, slot count at 6, zero at 7, sixteen one-byte states at 8..23,
  and sixteen child IDs `u32` at 24..87. Root parent/slot and absent child IDs
  are `UINT32_MAX`/`UINT8_MAX`; states 0/1/2/3 mean unused, child, object
  start, and object continuation. The data-region shape is exactly
  `8 * root_slots * 16^root_level`, with root slots 1..15.
- Data offsets are byte offsets relative to the separate Box data section and
  have no inline header. Allocate/free/rebuild use the version-1 16-way
  subdivision algorithm in `BoxAllocator`; recovery receives exact logical
  live intervals, rejects overlap/out-of-range intervals, resets metadata, and
  reserves the intervals in ascending offset order.

### ArtBump headerless slab version 1

- Each of the four slabs has a separate exact 64-byte metadata record: magic
  `KVASLAB1` at 0..7, `version:u32=1` at 8, payload bytes at 12, absolute zone
  offset/zone bytes at 16/24, capacity/high-water/used-count at 32/36/40,
  zero at 44, absolute free head at 48, and an immutable FNV-1a geometry hash
  at 56. A zero free head is empty. For the enclosing ArtBump geometry,
  capacity is exactly `C=4*entry_limit+1` and zone bytes are exactly
  `C*payload_bytes`; spare bytes cannot enlarge a slab.
- Every ArtBump FNV-1a hash in this contract starts at the 64-bit offset basis
  `14695981039346656037`, XORs each input byte, and multiplies modulo `2^64` by
  `1099511628211`. The slab geometry hash consumes, in this exact byte order,
  the eight ASCII bytes `KVASLAB1`, `version=1` as `LE32`, `payload_bytes` as
  `LE32`, `zone_offset` and `zone_bytes` as `LE64`, and `capacity` as `LE32`.
- Slab slots have no allocator header. An allocated slot is exactly one ART
  node record. A free slot is all zero except bytes 0..7, which contain the
  absolute next-free slot offset; zero ends the chain. In every clean state,
  `high_water<=C`, `used_count<=high_water`, and the free chain contains exactly
  every non-live slot below `high_water`, once, without cycles or duplicates.
  Its head may be any free slot and every nonzero next link is an exact free-slot
  start; chain order has no clean-state meaning. Normal free is O(1) prepend and
  allocation removes the head. Ordinary Attach validates the arbitrary/LIFO
  chain read-only and never sorts or rebuilds it. `used_count` is exactly the
  number of live slots below `high_water`.
- Owner-death recovery ignores all mutable slab fields. It derives the complete
  live absolute-offset set and the expected slab class of every offset from the
  committed tree before changing allocator bytes. The rebuilt high-water is
  zero for an empty live set or one plus the largest live slot index otherwise;
  `used_count` is the live count. Recovery zeroes every non-live slot below the
  rebuilt high-water and deterministically rebuilds a descending-offset free
  chain. This recovery normalization does not make descending order a clean
  invariant: a subsequent normal prepend may produce a non-descending chain.
  Slots in `[high_water,C)` are ignored and unvalidated; allocation must fully
  overwrite such a slot before it can become live. No allocator state is
  inferred from bytes of a slot that the committed tree says is allocated.
- This headerless slab is an ArtBump-only allocator format. It is not
  `FixedBlockAllocator` version 1 and it never has a 2-, 4-, or 8-byte block
  header before a node payload.

## Common notification storage

- With stored `queue_limit=q>0`, the queue table capacity is the smallest power
  of two `C` satisfying the checked integer inequality `10*q<=7*C`. The
  multiplication and next-power-of-two step are overflow-checked; there is no
  implicit minimum such as 8 and no physical-capacity-derived queue quota. It
  uses FNV-1a 64-bit and linear probing.
- A queue slot is exactly 32 bytes:
  `hash:u64`, `key_offset:u64`, `head_offset:u64`, `key_len:u32`, `state:u32`.
  State 0/1/2 is empty/occupied/tombstone. In a clean state, empty and tombstone
  slots have zero data fields. Queue key and head offsets are absolute
  common-heap Blob offsets; zero is empty. In a clean occupied slot both offsets
  are nonzero, `key_len<=UINT32_MAX`, the hash is FNV-1a of the exact key bytes,
  and the slot is in its canonical linear-probe position. Occupied keys are
  unique.
- Lookup probes at most exactly `C` slots, passes tombstones, remembers the
  first tombstone, and stops at an equal occupied key or the first empty slot;
  insertion reuses the remembered tombstone before that empty slot. If a full
  `C`-slot probe finds neither an equal key nor an empty slot, insertion uses
  the remembered first tombstone; absence of one means physical capacity is
  exhausted. Before allocating either a key Blob or a
  message Blob for a new logical queue, Push validates the complete lookup/probe
  result and current authoritative occupied count and requires
  `queue_count<queue_limit`. A push to an existing queue remains legal when the
  count equals the limit. Clean Attach independently derives the occupied count,
  requires it to equal stored `queue_count` and be at most `queue_limit`, and
  never repairs it. Owner-death recovery derives and checks a count at most the
  limit before its first repair write, then installs the derived count as part
  of its precomputed repair plan. Spare table cells never waive this quota.
- An occupied slot's key Blob is an allocated generic Blob with
  `payload_size=key_len` and payload equal byte-for-byte to the key, with no NUL
  terminator or semantic trailing bytes. The caller checks the `UINT32_MAX`
  bound before any allocation or persistent write; checked span/region
  exhaustion reports capacity.
- Each retained notification is one allocated generic Blob whose payload begins
  with `next_offset:u64` at 0, `value_len:u32` at 8, and exact message
  `magic:u32=0x4b564d53` at 12, followed by exactly `value_len` canonical TLV
  bytes. Its Blob `payload_size` is exactly `16+value_len`.
  `value_len<=UINT32_MAX-16` is checked before any persistent write, so both the
  message field and Blob payload size are representable. `value_len=0` is a
  retained None. `next_offset` is zero or the exact start of another allocated
  message Blob in the same queue; cycles, sharing between queues, and a key Blob
  used as a message are corruption. Push makes the new message the head and
  Watch pops the head, which defines LIFO order. One process-shared condition
  variable in the common header wakes waiters; it is not duplicated in every
  queue slot.
- Queue publication and recovery use this unique state machine while holding
  the robust mutex:
  1. Push fully allocates and writes the key Blob if needed and the message
     Blob with `next_offset=old_head`. For an existing slot it broadcasts the
     condition variable and then release-stores `head_offset`; for a new slot
     the chosen slot's original state `s` is exactly EMPTY or TOMBSTONE and
     remains `s` while the writer fills hash, key offset, head offset, and key
     length. In particular, reuse never first changes TOMBSTONE to EMPTY. It
     broadcasts and release-stores OCCUPIED state last. The broadcast-before-
     publish order is intentional:
     a waiter cannot reacquire the mutex until publication, and owner death
     after publication cannot create a retained-but-unwoken message.
  2. Pop acquire-loads the occupied state and head, validates the complete
     message, and branches on its already-validated `next_offset`. A non-final
     pop requires that next offset to be nonzero, release-stores that nonzero
     offset as the new head, and only then frees the old message. A final pop
     never publishes occupied state with a zero head. Instead it leaves the old
     hash, key offset, final-message head offset, and key length in place and
     release-stores only `state=TOMBSTONE` as the sole commit publication. It
     then frees the now-unreachable final message and key Blobs and finally
     zeroes the four data fields. Thus a crash during final cleanup exposes a
     tombstone with either old or zeroed data, never a newly produced
     occupied-plus-zero-head state.
  3. Before either release publication, newly allocated objects are
     uncommitted and recovery reclaims them, including staged key/message Blobs.
     At or after publication, the occupied slot and its complete head chain are
     authoritative. During owner-death preflight, every EMPTY or TOMBSTONE
     slot's four data fields are nonauthoritative: recovery neither validates
     nor dereferences them. Only after the complete table, common-heap ownership,
     count/limit, and scratch-resource plan succeeds does it zero those fields
     while preserving the slot state. It ignores stored queue-count and
     allocator free lists, rejects cycles, bad lengths, duplicate keys, or
     cross-linked messages, marks each authoritative occupied key/message
     exactly once, checks the derived count against the limit, and rebuilds the
     common Blob heap. Ordinary read-only Attach has no such relaxation and
     strictly requires every EMPTY and TOMBSTONE slot's data fields to be zero.
  4. COMMON04 owner-death recovery has one narrowly defined compatibility case
     for the final-pop order emitted by the legacy COMMON04 writer. It may
     accept exactly one slot with `state=OCCUPIED` and `head_offset=0` only if a
     full read-only queue-table, physical-heap, count/limit, and scratch-resource
     preflight proves all of the following before any write: its other fields
     form an otherwise canonical occupied slot; its nonzero key offset names one
     exact allocated generic Blob whose payload size and bytes equal `key_len`
     and whose FNV-1a hash equals the slot hash; the complete linear-probe path
     is valid; its key is unique and neither its Blob nor its interval is shared
     by any other reference; every other occupied slot has a nonzero, complete,
     disjoint message chain; and all common-heap Blob ownership, interval, and
     boundary-chain checks succeed. Ordinary read-only Attach does not accept
     this transient, and zero candidates take the normal path; two candidates
     or any failed predicate are corruption.
  5. The compatibility signature is unique among valid writer states: a clean
     queue cannot contain an occupied zero head, the new final-pop order never
     creates one, the robust mutex permits only one interrupted mutator, and the
     legacy final-pop writer alone published zero head before tombstone state.
     Once the complete preflight succeeds, the apply phase release-stores that
     slot's state as TOMBSTONE first, then clears exactly `hash`, `key_offset`,
     `head_offset`, and `key_len`, in that order. It never rereads those fields
     to decide ownership: the candidate key, any disconnected final-message
     allocation, and every still-authoritative queue Blob were classified in
     the precomputed disjoint ownership/interval set, from which the common heap
     is rebuilt. A second owner death after the state publication or after any
     one of the four clears therefore re-enters the ordinary nonoccupied-slot
     rule and idempotently completes the same plan. No broader occupied-zero-
     head heuristic is permitted. If these uniqueness and ownership predicates
     cannot be proved, accepting the state would require a new common ABI/version
     rather than weakening COMMON04 validation.
  6. Queue fault-injection tests cover non-final head publication and every
     final-pop crash point from tombstone publication through both frees and
     data-field zeroing, every new-slot field write for both EMPTY and TOMBSTONE
     targets, collision chains whose first reusable tombstone precedes an equal
     key or terminating empty slot, tombstone reuse, and a `q=1,C=2` fixture
     whose two distinct-home slots are both tombstones before another Push
     successfully reuses the first probed tombstone. Tests also inject a second
     owner death at every recovery apply step. A legacy COMMON04 fixture at the
     exact single occupied-zero-head window must recover to a clean tombstone;
     fixtures with two candidates, a bad probe path/hash/key Blob,
     sharing/overlap, or any
     invalid noncandidate chain must be rejected before changing one byte.

## Public Clear across all engines

- ArtBump, ArtBox, HashBox, and TrieBox implement public Clear as one operation
  under the same common robust mutex. After any mandatory owner-death recovery
  has completed, Clear requires the common heap journal and every selected-
  engine journal to be IDLE with their reserved fields valid. Before changing any queue, Blob,
  allocator, counter, journal, table, node, raw-zone, Box, or root byte, Clear
  completes one read-only preflight of the entire common queue table and common
  Blob heap and the selected engine's complete authoritative root graph or
  table. That preflight also validates all applicable journal states, immutable
  geometry, exact derived counts and logical limits, ownership/interval sets,
  every queue-heap and engine-allocator reset/rebuild plan, all checked
  arithmetic, and every volatile scratch allocation needed through completion.
  The plan is built entirely in volatile memory: neither persistent marks nor
  free-list heads may be changed and no allocator repair may begin before the
  last queue slot and last engine object have passed validation.
  A failure returns after zero persistent writes. In particular, Clear may not
  clear an early queue slot and only then discover a corrupt later slot or
  engine object. This both-sides scan is required because Clear touches both
  physically disjoint sections; it does not broaden the preflight of an
  engine-only mutation or a queue-only operation.
- On successful preflight, the queue phase runs first from immutable volatile
  plans. It broadcasts before its first queue publication. For each planned
  OCCUPIED slot, it release-stores TOMBSTONE before freeing any of that slot's
  precomputed key/message ownership and then clears `hash`, `key_offset`,
  `head_offset`, and `key_len`, in that order; it never dereferences the slot
  after tombstone publication to rediscover an offset. Existing EMPTY and
  TOMBSTONE slots are already strict-clean zero. After every slot is
  nonoccupied, it installs derived `queue_count=0` and completes the prepared
  common-heap release/coalesce plan. Owner death after any state, field-clear, release, or
  allocator-rebuild write is recoverable by the nonoccupied-field rule: all
  still-OCCUPIED slots remain authoritative, all nonoccupied data fields are
  ignored, and the next owner recomputes and idempotently applies a complete
  recovery plan for exactly that surviving occupied subset. With no Clear
  journal it does not guess that the remaining occupied queues should also be
  deleted; a later caller may invoke Clear again. Fault injection kills both
  the original Clear and a second recovery at every such step.
- Let `H=heap_offset` and let `T` be the pre-Clear `heap_top`. If `T==H`, the
  already empty pair `heap_top=H,heap_last=0` is left untouched. Otherwise,
  only after all queue references are gone, the prepared common-heap apply
  normalizes and coalesces the complete old extent into exactly one canonical
  unlinked free span `[H,T)`: its header at `H` has `span=T-H`,
  `previous_span=0`, zero free links/payload size/mark/reserved fields, valid
  Blob magic and exact free flags; all 64 free heads are zero; journal state is
  IDLE; and `heap_last=H,heap_top=T`. Clear then release-stores `heap_top=H`
  first and release-stores `heap_last=0` last. Between those stores
  `heap_last==heap_top==H`, and the still-complete header at `H` is exactly the
  already specified state-0 pending-first-append legacy pre-activation
  signature. The prepared old chain proves `T-H` is aligned and at least 64
  and that `checked(H+(T-H))=T<=heap_limit` and the mapped bound, so every
  existing out-of-top candidate-read predicate remains satisfied. Owner-death
  recovery therefore walks the empty committed extent,
  derives `D=0`, validates that exact out-of-top header plus the complete empty
  queue/ownership/count plan, and release-stores `heap_last=D`; no new journal
  state or relaxed tail relation is introduced. Ordinary Attach remains strict
  and rejects the intermediate form. `heap_last=0,heap_top=T>H`, a non-single-
  span candidate, nonzero free heads, or any other arbitrary last/top mismatch
  is corruption, not a Clear heuristic.
- Clear tests cover an initially empty heap (no top/last write), a nonempty
  first span and a multi-span heap, owner death at every release, normalize,
  coalesce, free-head-clear, `heap_top=H`, and `heap_last=0` boundary, plus a
  second death at every corresponding recovery step and convergence on the
  third owner. The exact between-store image must take only the existing
  pending-first-append rollback path; malformed lookalikes and the forbidden
  last-first reset image are rejected without a repair write.
- Only after the queue phase does Clear apply the already prepared engine-empty
  plan. It first fully materializes any required empty image/root and writes the
  engine's specified empty logical counters, then performs the release
  publication: ArtBump stores `committed_root=0`; ArtBox stores its
  `0xffffffff` empty-root sentinel; HashBox stores the selector of the canonical
  empty inactive image; and TrieBox stores the ID of a fully initialized
  allocated root with no value and no children. Only after that authority
  publication does it reclaim/reset old allocator, slab, raw-zone, or ignored-
  image state in the engine's idempotent recovery order. Owner-death recovery
  may therefore observe queues partly cleared with the old engine, or already
  empty while the engine is still old or already empty, and finishes
  the branch selected by the engine's authority token. A live observer cannot
  see an intermediate state because the mutex remains held, but Clear is not
  crash-atomic across queues and engine: only owner-death interruption may
  expose that split outcome, and no enclosing cross-section commit token exists.

## Value and Box references

- `BoxAllocator` is the persistence-stable 16-way allocator implemented in
  this repository. It follows the referenced boxmalloc allocation model while
  avoiding its compiler-dependent packed structs and bitfields.
- A Box allocation stores object bytes directly in Box data and has no per-
  object allocator header. Box offset zero is a valid allocation.
- Bytes in `[logical_length, BoxAllocator::RoundSize(logical_length))` of a live
  allocation, and every byte in unallocated Box data, are allocator slack/free
  data rather than object content. They need not be zero; readers and recovery
  ignore them, and they are excluded from persisted-key hash input and from key/
  TLV canonicality and length validation. Rounded interval bounds, alignment,
  uniqueness, and overlap checks still cover the complete live allocation.
- A stored Box reference is biased: `0` means no Box object and `N + 1` means
  Box data offset `N`. Overflow is rejected.
- Value presence is represented separately from the Box reference. Therefore:
  `has_value=false, ref=0` is missing; `has_value=true, ref=0` is stored None;
  and `has_value=true, ref>=1` is a non-None canonical TLV at `ref-1`.
- Non-None TLV logical length is recovered from its canonical TLV header and
  checked against the Box allocation and data-zone bounds. Hash key length and
  ART prefix length are stored by their owning record. Recovery supplies these
  exact logical lengths to `BoxAllocator::Rebuild`.

## ART invariants shared by ArtBump and ArtBox

- Both engines implement Node4, Node16, Node48, and Node256; leaf is a Node4
  with `has_value=true`, not a fifth node type.
- Prefixes have arbitrary length and are stored outside the fixed node record.
- Nodes grow at 5, 17, and 49 children. Node256 shrinks only at 37 children;
  Node48 shrinks at 16, and Node16 shrinks at 4. Thus a committed Node16 has
  5..16 children, Node48 has 17..48, and Node256 has 38..256. A no-value node
  with zero children is removed; a no-value node with exactly one child is
  always compacted by concatenating its prefix, edge byte, and child prefix.
  These are committed-tree invariants, including at the root; an empty tree
  uses the engine's empty-root sentinel.
- Every node record contains the explicit node kind and validates it against
  the reference or slab through which it was reached.
- Kinds are Node4=1, Node16=2, Node48=3, and Node256=4. Flags bit 0 is
  `has_value`; bits 1..7 must be zero. Node4/16 used keys are strictly sorted.
  Node48 index bytes are child slots 0..47 or `0xff` for empty. Unused keys,
  padding, and unused child slots use their engine's canonical zero/empty
  encoding and are validated.

## ArtBump (engine 1)

- The ArtBump engine ABI is exactly 4. ABI 3 and its old engine layout
  fingerprint are rejected as version mismatch without migration; engine ID 1
  is unchanged. ArtBump is a fixed-section engine: creation extends the backing
  object to `max_size`, `region_size==region_max`,
  `engine_offset==heap_limit`, and `engine_offset+engine_size==region_max`.
  The common `root_offset:u64` remains zero. The only authoritative root is the
  ArtBump engine header's `committed_root`.

### ArtBump engine header and fixed geometry

- The exact 384-byte engine header begins at the common `engine_offset=E` and
  is encoded field-by-field in little-endian order. It contains magic
  `KVABUMP1` at bytes 0..7, `version:u32=1` at 8, `header_bytes:u32=384` at 12,
  mutable `committed_root:u64` at 16, mutable `active_zone:u32` at 24, and zero
  at 28. Four 32-byte slab descriptors, ordered Node4/Node16/Node48/Node256,
  occupy 32..159; each descriptor is absolute
  `metadata_offset:u64`, `metadata_bytes:u64`, `zone_offset:u64`, and
  `zone_bytes:u64`. Zone 0 and zone 1 descriptors occupy 160..191 and
  192..223; each is immutable `begin:u64`, immutable `bytes:u64`, mutable
  `top:u64`, mutable `epoch:u32`, and a zero `u32`. The immutable geometry hash
  is at 224, `node_capacity:u32` is at 232, and the four `payload_bytes:u32`
  values 64, 168, 664, and 2072 occupy 236..251. Bytes 252..255 are zero. The
  exact compact journal occupies 256..383. Every unspecified or reserved field
  is zero and validated as zero.
- `committed_root` at header byte 16 is naturally 8-byte aligned. Normal access
  and recovery acquire-load it, and a commit release-stores a completely
  validated final root last. Root and child references are 64-bit absolute
  region offsets; zero is the empty reference.
- Creation initializes `committed_root=0`, `active_zone=0`, both zone tops to
  their respective begins, both epochs to 1, all four slabs empty with zero
  high-water/used-count/free-head, the complete compact journal to zero/IDLE,
  and all ArtBump-derived common counters to zero.
- Let `P=page_size`, `R=region_max`,
  `Q=page_up(sizeof(RegionHeader),P)`, and
  `H=page_up(checked(Q+checked(queue_capacity*32)),P)`. The common queue table occupies
  `[Q,Q+queue_capacity*32)`. The gaps `[sizeof(RegionHeader),Q)` and from the
  end of that table to `H` are zero at creation and validated as zero on
  Attach. After proving `R>=H`, let `U=checked(R-H)`,
  `QB=page_floor(U/8)`, and `E=checked(H+QB)`. The common queue
  heap is exactly `[H,E)` and the ArtBump engine section is exactly `[E,R)`;
  creation requires `QB>=P` and enough engine-tail bytes for the complete
  canonical geometry below. Every operation is overflow-checked.
- Let `N=entry_limit` and `C=checked(4*N+1)`. Creation requires
  `C<=UINT32_MAX`. Starting at `cursor=align_up(E+384,64)`, and using
  `P_i={64,168,664,2072}` in kind order, every slab is laid out exactly as
  `metadata_offset=cursor`, `metadata_bytes=64`,
  `zone_offset=metadata_offset+64`, `zone_bytes=checked(C*P_i)`, followed by
  `cursor=align_up(zone_offset+zone_bytes,64)`. Every alignment gap is zero and
  validated. Each slab's stored capacity is exactly `C`; no gap or spare tail
  participates in capacity.
- After the fourth slab, creation first requires `cursor<=R`, then lets
  `A=R-cursor` and `Z=floor(A/2)`. Creation requires `Z>0`. Zone 0 is
  `[cursor,cursor+Z)` and zone 1 is
  `[cursor+Z,cursor+2*Z)`. The terminal `A-2*Z` bytes, necessarily zero or one
  byte, are unowned: they are uninitialized, never read or written, and not
  validated. Attach recomputes all of this geometry from immutable common
  fields, page geometry, `E`, `R`, and `N` before trusting any stored engine
  descriptor.
- The engine layout fingerprint is FNV-1a 64-bit over, in this exact order, the
  eight ASCII bytes `ARTBUMP4`, then little-endian `u64` encodings of
  `sizeof(RegionHeader)`, 384, 128, 64, 64, 168, 664, 2072,
  `sizeof(QueueSlot)`, `sizeof(BlobHeader)`, `sizeof(pthread_mutex_t)`, and
  `sizeof(pthread_cond_t)`. The per-region immutable geometry hash is FNV-1a
  over the eight ASCII bytes `KVABUMP1`, then `version` and `header_bytes` as
  `LE32`, `N` as `LE64`, `C` as `LE32`, the four payload sizes as `LE32`, the
  four slab descriptors in their stored `LE64` field order, and, for zone 0
  then zone 1, immutable `begin` and `bytes` as `LE64`. It excludes
  `committed_root`, `active_zone`, both zone tops and epochs, the compact
  journal, the hash field itself, and all reserved bytes. Attach treats stored
  descriptors as untrusted, recomputes the expected geometry and both hashes,
  and requires an exact match.
- The ArtBump default-profile golden selects `engine=ArtBump` while retaining
  the numeric `ShmOptions` defaults: `initial_size=16,777,216`,
  `max_size=R=1,073,741,824`, `N=max_entries=65,536`, and
  `max_queues=4,096`, on the supported 4096-byte-page/1472-byte-header ABI.
  Fixed-section creation yields `region_size=region_max=R` despite the smaller
  compatibility `initial_size`. It has these exact derived scalars.

  | scalar | value |
  | --- | ---: |
  | `queue_capacity` | 8,192 |
  | queue-table bytes | 262,144 |
  | `Q` | 4,096 |
  | `H` | 266,240 |
  | `U=R-H` | 1,073,475,584 |
  | `QB=page_floor(U/8)` | 134,180,864 |
  | `E=H+QB` | 134,447,104 |
  | `engine_size=R-E` | 939,294,720 |
  | `C=4*N+1` | 262,145 |

- The same default golden has this canonical byte geometry. All listed ends are
  exclusive; the three alignment-gap rows are reserved zero.

  | section | offset/begin | bytes | end |
  | --- | ---: | ---: | ---: |
  | common-header page | 0 | 4,096 | 4,096 |
  | common queue table | 4,096 | 262,144 | 266,240 |
  | common queue heap | 266,240 | 134,180,864 | 134,447,104 |
  | ArtBump header | 134,447,104 | 384 | 134,447,488 |
  | Node4 slab metadata | 134,447,488 | 64 | 134,447,552 |
  | Node4 slab zone | 134,447,552 | 16,777,280 | 151,224,832 |
  | Node16 slab metadata | 151,224,832 | 64 | 151,224,896 |
  | Node16 slab zone | 151,224,896 | 44,040,360 | 195,265,256 |
  | zero alignment gap | 195,265,256 | 24 | 195,265,280 |
  | Node48 slab metadata | 195,265,280 | 64 | 195,265,344 |
  | Node48 slab zone | 195,265,344 | 174,064,280 | 369,329,624 |
  | zero alignment gap | 369,329,624 | 40 | 369,329,664 |
  | Node256 slab metadata | 369,329,664 | 64 | 369,329,728 |
  | Node256 slab zone | 369,329,728 | 543,164,440 | 912,494,168 |
  | zero alignment gap | 912,494,168 | 40 | 912,494,208 |
  | raw zone 0 | 912,494,208 | 80,623,808 | 993,118,016 |
  | raw zone 1 | 993,118,016 | 80,623,808 | 1,073,741,824 |
  | unowned terminal tail | 1,073,741,824 | 0 | 1,073,741,824 |

  Initial `heap_top=heap_offset=266,240`, `heap_last=0`, both raw tops equal
  their listed begins, both epochs are 1, and active zone is 0. The supported-
  ABI golden hashes are engine layout `0xb3d5c1220e5bba30`, region geometry
  `0xb246b66f5ac6da9d`, and Node4/16/48/256 slab geometry respectively
  `0x63a9f2b507e0dee1`, `0x7e5221948b3c88b6`,
  `0xb63348d0c06bd177`, and `0x833588ef1c55f705`.

### ArtBump nodes and raw zones

- Each allocated slab slot is exactly one manually encoded node. The common
  24-byte node header is `prefix_offset:u64`, `value_offset:u64`,
  `prefix_len:u32`, `child_count:u16`, `kind:u8`, and `flags:u8`. Node4 stores
  keys at 24..27, zero padding at 28..31, and four `u64` children at 32..63.
  Node16 stores keys at 24..39 and sixteen children at 40..167. Node48 stores
  its 256-byte index at 24..279 and 48 children at 280..663. Node256 stores 256
  children at 24..2071.
- Every nonzero root or child reference equals the start of one exact slab slot
  and the record kind agrees with that slab. Traversal rejects an out-of-range
  or misaligned offset, a cycle, or a node reachable through more than one
  parent. The shared ART kind, flag, child-count, growth, shrink, pruning, and
  prefix-compaction invariants apply.
- In Node4 and Node16, the used keys are strictly increasing, their used child
  references are nonzero, and every unused key and child is zero; Node4 padding
  is zero. In Node48, every index byte is `0xff` or a slot in 0..47, the live
  slots are exactly 0 through `child_count-1` and each occurs in the index
  exactly once, the corresponding child is nonzero, and every remaining child
  slot is zero. In Node256, exactly `child_count` children are nonzero and all
  absent children are zero.
- `prefix_len==0` if and only if `prefix_offset==0`. Every nonempty prefix is
  the exact interval `[prefix_offset,prefix_offset+prefix_len)` in the active
  raw zone's `[begin,top)` range. `has_value=false` requires `value_offset=0`.
  `has_value=true,value_offset=0` is stored None and consumes no raw bytes. A
  nonzero value offset denotes one complete nonempty canonical TLV; its exact
  logical length is decoded and bounds-checked from the TLV header. ArtBump
  stores no separate full-key copy.
- Across the committed tree, all nonempty prefix intervals and all non-None
  value intervals are pairwise disjoint. Duplicate, overlapping, overflowing,
  or out-of-zone intervals are corruption. At journal state IDLE every live raw
  reference lies in the selected `active_zone`, both zone epochs are nonzero,
  each top is in its checked `[begin,begin+bytes]` range, and the inactive
  zone's top equals its begin. Bytes outside live intervals, including stale
  bytes beyond a top, have no semantic meaning.
- A raw zone is headerless and byte-packed. An allocation of length `L>0`
  starts at the current top with no alignment, padding, object header, or
  stored full-key bytes. The writer checks the complete range, copies all `L`
  bytes, and release-stores the advanced top only after the copy. Prefix and
  non-None TLV bytes are append-only during ordinary mutation.
- On owner death with compact-journal state IDLE, the acquire-loaded committed
  root is the sole engine authority. Recovery first performs a read-only
  traversal that validates the complete node graph and pairwise-disjoint live
  raw intervals, builds all four slab live-slot plans, derives every counter,
  and computes `recovered_top` as the greater of `active.begin` and every live
  raw interval end; with no live raw interval, it is exactly `active.begin`. It also
  preflights all scratch resources before the first persistent repair write.
  Recovery then release-stores the active top to `recovered_top`, release-stores
  the inactive top to its begin, rebuilds all four slabs from the same root, and
  installs the derived counters. Dead internal raw gaps below `recovered_top`
  remain garbage until Compact, but an uncommitted tail is reclaimed.
- For an IDLE empty root, that same plan canonicalizes the stronger empty state:
  both tops equal their begins, both epochs are 1, `active_zone=0`, all slabs are
  empty, and all engine counters are zero. For a nonempty root, the validated
  active selector and both nonzero epochs are unchanged. Because no repair
  field is an authority and the root stays unchanged, death after any repair
  write repeats the complete preflight and plan idempotently.

### ArtBump mutation capacity and commit

- Before the first persistent engine write of an ordinary non-Clear,
  non-Compact public mutation, a read-only
  preflight requires compact-journal state IDLE with all reserved fields zero;
  validates the complete committed tree `Ctree` and the initial/current staged
  tree `S`, every node and raw interval, both raw-zone descriptors, all four
  slab live/free sets, exact derived counters, and `entry_limit`; and reserves
  the baseline traversal, ownership, interval, validation, and rollback scratch
  needed by the mutation. It deliberately does not scan the common queue table
  or Blob heap: ArtBump mutation touches only the physically disjoint engine
  section and engine-derived common counters and cannot reference queue Blobs.
  Before any later internal update frees or writes a persistent byte, that
  update additionally reserves its complete path-decode, replacement-build,
  staged-validation, and abort/rebuild scratch from the still-valid `Ctree` and
  current `S`. Allocation failure in either preflight leaves every persistent
  byte unchanged.
- One public mutation may perform multiple internal ART updates while retaining
  one committed tree `Ctree` and one current staged tree `S`. Before freeing a
  staged path or making any persistent node/raw allocation for an internal
  update, it validates both the current staged entry count and the projected
  next staged entry count are at most `N`; exceeding `N` reports capacity with
  no persistent allocation. After every internal update, the implementation
  reclaims every node outside
  `Ctree union S`. Before building the next replacement path `P'`, it decodes
  the complete old staged path `P` into process memory and frees every node in
  `P` that is not in `Ctree`. Every subsequently allocated persistent node must
  belong directly to the final replacement path or branch `P'`; persisted
  scratch nodes or intermediate path versions are forbidden. A prefix split's
  shortened old child, new branch, and new leaf are all final members of `P'`.
- Let `O=S minus P` and let `A` be the subset of `P'` allocated so far. During
  the build, the persistent node set is a subset of
  `Ctree union O union A`, itself a subset of `Ctree union S'`, where
  `S'=O union P'` is the final next staged tree. Each ART with at most `N`
  entries has at most `2*N-1` nodes, so the per-class peak is no greater than
  the total `|Ctree|+|S'|<=4*N-2`. Full compaction likewise holds the complete
  old tree and its complete clone, at most `4*N-2` nodes. This strict turnover
  invariant, rather than a workload class distribution, is the reason every
  class has `C=4*N+1` slots.
- Retaining the superseded staged path until its replacement is complete, or
  allocating any persistent scratch node that is not in `S'`, violates this
  contract and invalidates the `4*N+1` guarantee. If any internal update fails
  after its old staged-only path was freed, the whole public mutation aborts:
  it keeps the committed root and committed counters unchanged and rebuilds or
  reclaims all batch state from that root. It may restore the active raw top to
  its value at public-mutation entry.
- Commit first validates the complete staged tree and raw-interval set, writes
  the derived counters, and release-stores the final root last. While still
  holding the common robust mutex, it then reclaims every node slot outside the
  final-root graph. Raw intervals that became unreachable remain bump garbage
  until Compact; there is no ordinary individual raw free. Neither a node nor a
  raw interval may be treated as dead merely because a retired node referenced
  it if the final tree still references the same object. Any capacity failure
  leaves the prior committed state attachable.

### ArtBump compact journal and recovery

- The exact 128-byte compact journal at engine-header byte 256 is encoded as
  `old_root:u64` at +0, `source_top:u64` at +8,
  `operation_generation:u64` at +16, `base_checksum:u64` at +24,
  `new_root:u64` at +32, `target_top:u64` at +40, `node_count:u64` at +48,
  `entry_count:u64` at +56, `engine_live_bytes:u64` at +64,
  `ready_checksum:u64` at +72, `state:u32` at +80,
  `source_zone:u32` at +84, `target_zone:u32` at +88,
  `source_epoch:u32` at +92, `target_epoch:u32` at +96, zero `u32` at +100,
  and three zero-reserved `u64` words at +104..+127. State is exactly 0 IDLE,
  1 COPYING, or 2 READY; every other value is corruption. In IDLE, payload
  fields are stale and ignored, but all four reserved fields are zero in every
  state.
- `base_checksum` is FNV-1a over the eight ASCII bytes `ABJCOPY1`, followed by
  `old_root`, `source_top`, and `operation_generation` as `LE64`, then
  `source_zone`, `target_zone`, `source_epoch`, and `target_epoch` as `LE32`.
  `ready_checksum` is FNV-1a over the eight ASCII bytes `ABJREADY`, followed by
  `base_checksum`, `new_root`, `target_top`, `node_count`, `entry_count`, and
  `engine_live_bytes` as `LE64`. Active-state recovery validates the applicable
  checksum before using any journal address or count.
- In COPYING, `new_root`, `target_top`, `node_count`, `entry_count`,
  `engine_live_bytes`, and `ready_checksum` are ready-only staging bytes and are
  ignored completely: they are neither validated nor used or dereferenced.
  Clearing them before COPYING publication gives a deterministic initial image,
  but is not an active-state invariant because the writer must fill those fields
  one by one while state is still COPYING and release-store READY last. Only an
  acquire-observed READY state plus a valid `ready_checksum` makes the complete
  set authoritative. The mutable target-zone descriptor may likewise advance
  during copying but is not authority.
- Before its first journal-payload write, Compact acquire-validates journal
  state IDLE and all reserved fields, then preflights the complete source tree,
  the exact source node-slab live/free plan, the complete source raw-interval
  set, free slots in every node class for the full clone, target-zone bytes for
  the packed raw copy, exact derived counters, and source
  `entry_count<=entry_limit`. It also reserves, up front, all volatile clone
  traversal, source/target interval-set, source-to-clone node-map, final-clone
  validation, and rollback/slab-rebuild scratch. Compact does not scan the
  common queue table or Blob heap because it reads and writes only the
  physically disjoint ArtBump engine section and cannot reference queue Blobs.
  Any checked-capacity, validation, fault-injection, or scratch-allocation
  failure before that first journal write leaves the region byte-for-byte
  unchanged.
  It defines journal `operation_generation=G` as common `generation+1` modulo
  `2^64`; the pre-operation generation is therefore `G-1` modulo `2^64`. It
  then writes the base journal payload and checksum, clears all ready-only
  fields, and release-stores state COPYING last. `target_epoch` is the target
  zone's prior epoch plus one modulo `2^32`, skipping zero (`UINT32_MAX`
  advances to 1); Compact resets the target top to its begin and installs that
  epoch before copying.
- Compact copies every live raw interval exactly once in ascending source-offset
  order, packing the target bytes without padding, and rewrites the cloned
  references. It clones nodes postorder; every node is completely written and
  validated before a parent can reference it. It advances target top only by
  the raw-zone publication rule. After validating the complete clone and its
  derived counters, it writes all ready fields and `ready_checksum`, then
  release-stores state READY.
- A normal writer commits READY in exactly this order. `Dnew` means the
  independently validated journal `node_count`, `entry_count`, and
  `engine_live_bytes`; common `root_offset` and `tombstone_count` remain zero.
  The source epoch remains `source_epoch`.

  | order | field/action | final value |
  | ---: | --- | --- |
  | 1 | target `epoch`, then release-store target `top` | `target_epoch`, `target_top` |
  | 2 | `active_zone` | `target_zone` |
  | 3 | derived common counters | `Dnew`, `root_offset=0`, `tombstone_count=0` |
  | 4 | `generation` | `G` |
  | 5 | release-store `committed_root` | `new_root` (semantic commit point) |
  | 6 | rebuild four slabs | exact new-tree live-slot set |
  | 7 | source `epoch`, then release-store source `top` | unchanged `source_epoch`, source `begin` |
  | 8 | release-store journal `state` | IDLE, after all cleanup |

  The journal remains READY through orders 1..7. Its payload becomes stale only
  after order 8.
- While state is COPYING or READY, `active_zone`, both mutable zone descriptors,
  counters, generation, and slab mutable metadata are not authorities. The
  acquire-loaded committed root plus journal state selects exactly one
  authoritative tree. COPYING is valid only when `committed_root==old_root`;
  only `old_root` is traversed and recovery rolls back to the source tree.
  READY with `committed_root==old_root` traverses only the old tree and rolls
  back; READY with `committed_root==new_root` traverses only the new tree and
  rolls forward. If
  `old_root==new_root==committed_root==0`, READY explicitly rolls forward so an
  empty-tree Compact is unambiguous. A nonempty READY record with equal old and
  new roots, or any other state/root combination, is corruption. The
  nonauthoritative tree is never a fallback and is never traversed: rollback may
  already have reset the target top, while roll-forward may already have
  destroyed old-only slab nodes and reset the source top.
- In either active state, `source_zone` and `target_zone` are distinct values in
  0..1, their recorded epochs are nonzero,
  `source.begin<=source_top<=checked(source.begin+source.bytes)`, and READY also
  requires `target.begin<=target_top<=checked(target.begin+target.bytes)` as
  journal-record geometry checks independent of mutable descriptor tops.
  COPYING and READY selected for rollback require every authoritative old-tree
  raw reference to denote one exact nonempty interval wholly inside
  `[source.begin,source_top)`. READY selected for roll-forward instead requires
  every authoritative new-tree raw reference to denote one exact nonempty
  interval wholly inside `[target.begin,target_top)`. Partially copied target
  bytes in COPYING, the READY new tree on a rollback branch, and the READY old
  tree on a roll-forward branch are nonauthoritative and are never traversed.
- Active-state tree validation is range-before-read. For a nonempty prefix of
  length `P`, it first requires the raw offset to be at least the selected zone
  begin and strictly below the applicable recorded top, then checks the
  overflow-safe end `offset+P<=top`; only after both checks may it read prefix
  bytes. For a nonzero value offset, it first requires the start to be at least
  begin and strictly below top and checks an overflow-safe minimum-header end at
  or below top. Only then may it read that minimum header; if the tag selects a
  longer header, the complete header end is separately checked before those
  additional header bytes are read. It then decodes the logical length,
  requires it to be at least the complete canonical-header length, checks the
  overflow-safe exact end at or below top, and only then reads or canonically
  decodes the body. No raw byte is dereferenced speculatively, and all resulting
  old or new exact intervals must also pass the global disjointness and
  uniqueness checks.
- Before modifying allocator metadata during either rollback or roll-forward,
  recovery preflights only the branch-selected authoritative tree, its exact raw
  interval set, derived counters, complete slab-rebuild plan, geometry, and
  scratch resources. It never requires the other tree to remain readable.
  Rollback derives `Dold` solely from `old_root`, rather than trusting mutable
  common counters or READY-only journal fields, then uses this exact order.

  | order | rollback field/action | final value |
  | ---: | --- | --- |
  | 1 | source `epoch`, then release-store source `top` | `source_epoch`, `source_top` |
  | 2 | target `epoch`, then release-store target `top` | `target_epoch`, target `begin` |
  | 3 | `active_zone` | `source_zone` |
  | 4 | derived common counters | `Dold`, `root_offset=0`, `tombstone_count=0` |
  | 5 | `generation` | `G-1` modulo `2^64` |
  | 6 | release-store `committed_root` | unchanged `old_root` |
  | 7 | rebuild four slabs | exact old-tree live-slot set |
  | 8 | release-store journal `state` | IDLE, after the rebuild |

- Roll-forward derives its entire preflight, counters, raw intervals, and four
  slab-rebuild plans solely from `new_root`, requires those counters to equal the
  READY record, and uses this exact order.

  | order | roll-forward field/action | final value |
  | ---: | --- | --- |
  | 1 | target `epoch`, then release-store target `top` | `target_epoch`, `target_top` |
  | 2 | `active_zone` | `target_zone` |
  | 3 | derived common counters | READY `Dnew`, `root_offset=0`, `tombstone_count=0` |
  | 4 | `generation` | `G` |
  | 5 | release-store `committed_root` | unchanged `new_root` |
  | 6 | rebuild four slabs | exact new-tree live-slot set |
  | 7 | source `epoch`, then release-store source `top` | unchanged `source_epoch`, source `begin` |
  | 8 | release-store journal `state` | IDLE, after all cleanup |

  Death after any row leaves the active journal non-IDLE until the final row,
  so recovery reselects the same branch from `committed_root` and idempotently
  repeats its complete single-authority plan. In particular, death during or
  after new-tree slab rebuild or after source-top reset never reads the old
  tree; death after rollback resets target top never reads the new tree.

### ArtBump counters, Clear, errors, and conformance

- `node_count` is the number of nodes reachable from `committed_root`.
  `entry_count` is the number of reachable nodes with `has_value`, including
  stored None. `tombstone_count` and common `root_offset` are always zero.
  `engine_live_bytes` is the sum of each live node's exact slab payload size,
  every exact nonempty prefix length, and every non-None canonical TLV logical
  length. It excludes slab and engine metadata, free or unreachable slots, raw
  garbage, padding, journals, common queue data, and the terminal raw tail.
  Recovery derives and overwrites all counters; a clean attach validates their
  exact equality. Generation is diagnostic and never chooses semantic state;
  the journal operation generation only helps identify/replay one Compact.
- Under the global public-Clear contract, ArtBump's no-write preflight requires
  compact-journal state IDLE and validates the existing committed tree, raw
  intervals, slabs, counters, limits, empty rebuild, and scratch plan before
  the queue phase begins. After that queue phase completes, the engine phase
  writes zero derived counters and release-stores `committed_root=0` as the
  semantic clear commit before freeing or resetting any engine allocation. It
  then rebuilds all four slabs empty, resets both raw tops to their begins and
  both epochs to 1, selects active zone 0, and leaves journal state IDLE. A
  death before engine-root publication preserves the old tree while queues may
  already be empty; a death after root publication causes recovery to finish
  the empty engine reset. Clear need not wipe unowned terminal bytes, raw bytes
  outside live intervals, or slab slots at or above rebuilt high-water.
- Checked arithmetic, insufficient common-heap or engine-tail geometry, slab
  capacity overflow, and raw-zone insufficiency at Create or mutation report
  capacity without publishing a partial state. Engine ABI/layout-fingerprint
  mismatch reports version. Attach-time bad magic/header fields, recomputed
  geometry/hash mismatch, invalid reserved bytes, journal, allocator metadata,
  node graph, TLV, or interval set reports corruption.
- This section supersedes every earlier ArtBump draft. Dynamic `ftruncate`
  growth, node sizes 64/80/416/2112, free links stored in `children[0]`, a bump
  record containing the complete key, and treating None as leaf absence are not
  alternate encodings; ABI 4 rejects them.
- Required tests include byte-golden engine-header, compact-journal, slab, and
  all-four-node records; default and overflow-boundary geometry; adversarial
  multi-update and Compact workloads that instrument simultaneous live slots
  and prove the total, and therefore every class, never exceeds `4*N-2`.
  `high_water` is a historical slot frontier, not a live-count measurement: the
  highest previously used slot index may legally reach `4*N` and thus
  `high_water` may reach `C=4*N+1` while simultaneous live slots remain within
  `4*N-2`. Tests do not reject that state. They also cover None, arbitrary
  prefixes, canonical TLV bounds, duplicate/overlapping raw intervals, and
  empty-tree Compact and Clear. Compact additionally injects `bad_alloc` at
  every up-front scratch reservation and a fault immediately before the first
  journal-payload write and requires byte-for-byte identity; after each later
  journal/copy publication it verifies the specified rollback or roll-forward
  branch. Fault injection terminates an
  ordinary process after every journal-state publication, raw-top publication,
  raw/node copy boundary, root publication, and cleanup write, then also during
  recovery and verifies successful repeat recovery. It specifically kills an
  old-root rollback after target-top reset and kills a new-root roll-forward at
  the first, middle, and final slab-rebuild writes and after source-top reset;
  each case is killed once more during recovery and must succeed on the third
  attempt without traversing the now-nonauthoritative tree. Fault tests also
  terminate after each ready-only field and `ready_checksum` write but before
  READY publication; COPYING recovery must ignore all such partial combinations,
  roll back from the old tree, survive a second recovery death, and converge.
  A clean non-descending/LIFO ArtBump slab fixture must attach read-only without
  changing one byte, while owner-death recovery of the same live set may produce
  the deterministic descending rebuild. Separate fault injection kills an
  ordinary IDLE mutation after top publication but before root publication and
  during each IDLE repair write, then verifies the recovered top equals the
  maximum committed live-interval end. Compatibility tests reject
  common Blob class bits and nonzero preserved heads, reject legacy ArtBump ABI
  3 and HashBox ABI 3, and attach existing generic-Blob ArtBox ABI 3 and TrieBox
  ABI 4 `COMMON04` fixtures unchanged.

## ArtBox (engine 2)

- There are exactly four `FixedBlockAllocator` instances, one per ART node
  class, plus one `BoxAllocator` for prefixes and non-None values.
- A root or child is a 32-bit tagged local block reference. `0xffffffff` is
  empty; bits 31..30 encode Node4/16/48/256 as 0/1/2/3; bits 29..0 encode the
  allocator-local block ID. Local IDs above `0x3ffffffe` are rejected, so the
  empty sentinel cannot alias a Node256 reference.
- Records are manual little-endian and have exact payload sizes 48, 104, 472,
  and 1048 bytes. Their common 24-byte header is:
  `prefix_ref:u64`, `value_ref:u64`, `prefix_len:u32`, `child_count:u16`,
  `kind:u8`, `flags:u8`. Child references are the tagged 32-bit form. Padding
  bytes are zero and validated; an unused child is `0xffffffff`.
- Node4 stores keys at bytes 24..27, children at 28..43, and four zero padding
  bytes at 44..47. Node16 stores keys at 24..39 and children at 40..103.
  Node48 stores its index at 24..279 and children at 280..471. Node256 stores
  children at 24..1047.
- `prefix_ref` and non-None `value_ref` use the biased Box rule. Stored None is
  `has_value=true,value_ref=0`. FixedBlock's selected 2/4/8-byte allocation
  header is outside the exact node payload size.
- The ArtBox engine ABI is exactly 3. ABI 2 and the old ArtBox layout hash are
  rejected without migration. The exact 256-byte engine header begins at the
  common `engine_offset` and is encoded manually in little-endian order:
  magic `KVARTB01` at 0..7, version `u32=1` at 8, header bytes `u32=256` at
  12, the mutable committed tagged root `u32` at 16, zero at 20, four
  32-byte slab descriptors in Node4/16/48/256 order at 24..151, Box metadata
  offset/bytes and Box data offset/bytes as four `u64` values at 152/160/168/
  176, immutable geometry hash `u64` at 184, node capacity `u32` at 192,
  FixedBlock header width `u8` at 196, zero bytes 197..199, the four node
  payload sizes as `u32` values at 200/204/208/212, and zero bytes 216..255.
  Each slab descriptor is metadata offset/bytes and block-zone offset/bytes as
  four consecutive `u64` values. All offsets are absolute region offsets.
- The committed root word at header byte 16 is naturally 4-byte aligned.
  Creation initializes it to `0xffffffff`; normal access and recovery acquire-
  load it, and commit release-stores one completely validated final root. It
  is the only authoritative ArtBox root. The common `root_offset:u64` is not
  used by ArtBox and remains zero.
- Let `N=entry_limit` and `C=checked(5*N+1)`. Creation requires
  `C<=0x3fffffff`, so the largest possible local ID is at most
  `0x3ffffffe`. All four FixedBlock allocators have exactly capacity `C` and
  use the same `w=MinimumHeaderWidth(C)`. Let `E=engine_offset`,
  `B=page_up(E+256)`, and payloads in kind order be
  `P={48,104,472,1048}`. Starting with `cursor=B`, each slab is laid out as:
  `metadata_offset=cursor`, `metadata_bytes=64`,
  `zone_offset=metadata_offset+64`,
  `zone_bytes=C*(w+P[i])`, then
  `cursor=align_up(zone_offset+zone_bytes,64)`. The page gap before `B` and
  each alignment gap are zero and validated. No slab receives spare bytes
  from which FixedBlock could infer a capacity greater than `C`.
- The Box metadata begins at the final `cursor`. For the exact remaining
  budget `R=region_max-cursor`, creation computes
  `D=BoxAllocator::LargestFullyRepresentableDataSize(R)` and
  `M=BoxAllocator::MinimumMetadataBytesForFullExpansion(D)`.
  Box metadata bytes are exactly `M`; Box data begins immediately after it and
  has exactly `D` bytes. `D` must be nonzero and `M+D<=R`. The terminal
  `R-M-D` bytes are outside every section and are never read or written by the
  engine. Every addition, multiplication, alignment, local-ID bound, and
  conversion is checked before the backing object is initialized.
- ArtBox is a fixed-section engine: creation extends the backing object to
  `max_size`, `region_size==region_max`, `engine_offset==heap_limit`, and
  `engine_offset+engine_size==region_max`. Attach verifies the backing file is
  at least `region_max` before accessing the engine header, recomputes the
  complete geometry above from the common immutable fields, and requires every
  stored descriptor and constant to match it exactly.
- The engine layout fingerprint is FNV-1a 64-bit over, in this exact order:
  the eight ASCII bytes `ARTBOX02`, then little-endian `u64` encodings of
  `sizeof(RegionHeader)`, 256, 48, 104, 472, 1048,
  `FixedBlockAllocator::kPersistentMetadataBytes`,
  `BoxAllocator::kPersistentHeaderBytes`,
  `BoxAllocator::kNodePayloadBytes`, `sizeof(QueueSlot)`,
  `sizeof(BlobHeader)`, `sizeof(pthread_mutex_t)`, and
  `sizeof(pthread_cond_t)`. The per-region immutable geometry hash is a
  separate FNV-1a 64-bit hash over the eight `KVARTB01` bytes, then
  version and header bytes as `u32`, `C` as `u32`, `w` as `u8`, the four
  payload sizes as `u32`, the four descriptors in stored `u64` order, and the
  four Box offset/byte values as `u64`. The mutable root and all reserved bytes
  are excluded from the hash; reserved bytes are nevertheless required to be
  zero.
- One public mutation may contain several internal ART updates but publishes
  only its final root. After each successful staged update, the implementation
  reclaims every batch-created node and Box interval unreachable from both the
  still-committed root and the current staged root. Before the next update the
  union of those two trees contains at most `4*N-2` nodes. A non-inserting
  update allocates at most `N` replacement nodes. An inserting prefix split
  allocates at most `N+1`: ancestor copies, a shortened old-child clone, a new
  branch, and a new leaf. Thus the coarse peak is at most
  `(4*N-2)+(N+1)=5*N-1`, below `5*N+1`; more tightly, the staged tree before
  insertion has at most `N-1` entries. This is a checked per-class bound and
  does not rely on a workload distribution. A failed update or explicit
  rollback leaves the committed root and counters unchanged and reclaims every
  batch-created object not reachable from that root.
- Commit validates the complete staged tree and its live Box interval set,
  installs derived counters, and release-publishes the final root last. It
  then reclaims all nodes and Box intervals not reachable from the final root,
  including old committed paths and intermediate staged paths. Reclamation is
  a graph/set difference: an object is never freed merely because a retired
  node referenced it, because the final tree may reuse the same prefix or
  value reference. Any capacity error leaves a usable, attachable committed
  state.
- Owner-death recovery acquire-loads only the committed root, traverses node
  payloads through immutable FixedBlock geometry, and preflights the complete
  four live-ID bitmaps plus Box interval set, Box metadata-node capacity, and
  all five allocator scratch resources before rewriting allocator metadata. It
  rejects cycles, shared nodes, tag/kind mismatches, duplicate or overlapping
  Box intervals, noncanonical TLVs, and every nonempty prefix or non-None value
  outside its exact Box allocation. It applies the four FixedBlock rebuilds as
  one prepared node plan and then applies the prepared Box plan; interruption
  between those plans is replayable from the same committed root. It derives
  `node_count`, `entry_count`, and `engine_live_bytes` from the completed
  read-only preflight rather than a post-apply traversal; ArtBox tombstones are
  always zero. Torn allocator state and counters are never authorities, and
  repeating recovery after death during a prior recovery is valid.
- ArtBox `engine_live_bytes` is the sum of the exact fixed node payload sizes
  for every committed node plus `BoxAllocator::RoundSize(logical_size)` for
  every committed nonempty prefix and non-None value. It excludes FixedBlock
  per-block headers, all allocator metadata, section padding, and the Box
  terminal tail. An empty tree therefore has zero nodes, zero entries, an
  empty-root sentinel, and zero `engine_live_bytes`.

## HashBox (engine 3)

- The HashBox engine ABI is exactly 4. ABI 3, its legacy common-Blob format, and
  its old layout hash are rejected without migration. The common format remains
  `RegionPrefix.version=4`/`COMMON04`; engine ID 3 is unchanged.
- The table capacity is fixed at creation and never resizes. It is the smallest
  power of two `C` satisfying `10 * entry_limit <= 7 * C`, equivalently
  `C >= ceil(10 * entry_limit / 7)`. The multiplication, ceiling, power-of-two,
  table-byte, and offset arithmetic is checked, and there is no implicit
  capacity floor such as 8.
- The persisted key hash is FNV-1a 64-bit with offset basis
  `14695981039346656037` and prime `1099511628211`, wrapping modulo `2^64`.
  Its input is exactly the unsigned key bytes, with no terminator, prefix, seed,
  or path normalization. A zero hash is valid. The home slot is
  `hash & (C-1)` and probe `i` visits `(home+i) & (C-1)`.
- The engine layout fingerprint is FNV-1a 64-bit over, in this exact order, the
  eight ASCII bytes `HBOXABI4`, then little-endian `u64` encodings of
  `sizeof(RegionHeader)`, the header-byte constant 64, the slot-byte constant 32,
  `FixedBlockAllocator::kPersistentMetadataBytes`,
  `BoxAllocator::kPersistentHeaderBytes`,
  `BoxAllocator::kNodePayloadBytes`, `sizeof(QueueSlot)`,
  `sizeof(BlobHeader)`, `sizeof(pthread_mutex_t)`, and
  `sizeof(pthread_cond_t)`.
- The exact 64-byte engine header begins at the common `engine_offset` and is
  encoded manually in little-endian order: magic `KVHBOX01` at 0..7,
  `version:u32=1` at 8, `header_bytes:u32=64` at 12, absolute Box metadata
  offset/bytes and Box data offset/bytes as four `u64` values at 16/24/32/40,
  immutable geometry hash `u64` at 48, and zero bytes 56..63. It contains no
  selector, counter, table descriptor, hash seed, or allocator-mutable field.
- Let `N=entry_limit`, let `T0` and `T1` be the two canonical common-header
  table offsets, let `E=engine_offset`, and let `R=region_max`. Box metadata
  begins directly at `B=E+64`; there is no additional page-alignment gap. For
  the exact budget `A=R-B`, creation computes
  `D=BoxAllocator::LargestFullyRepresentableDataSize(A)` and
  `M=BoxAllocator::MinimumMetadataBytesForFullExpansion(D)`. The header stores
  `B`, `M`, `B+M`, and `D`; `D` must be nonzero and `M+D<=A`. The range
  `[B+M+D,R)` is an unowned terminal tail and is neither initialized nor
  validated.
- The immutable geometry hash is FNV-1a 64-bit over, in this exact order: the
  eight `KVHBOX01` bytes, `version=1` and `header_bytes=64` as LE32, then `N`,
  `C`, `T0`, `T1`, `B`, `M`, `B+M`, and `D` as LE64. The geometry-hash field
  and reserved bytes are excluded. Attach treats every stored descriptor as
  untrusted scalar bytes, recomputes this complete geometry from the common
  immutable fields, and requires all descriptors and the hash to match exactly;
  it also requires header bytes 56..63 to remain zero. Arithmetic or
  Box-geometry failure is capacity at Create and corruption at Attach. The Box
  and its embedded FixedBlock headers then apply their own version-1 immutable
  geometry validation.
- Each table image contains exactly `C` 32-byte slots. A slot is manual
  little-endian: `hash:u64` at 0, `key_ref:u64` at 8, `value_ref:u64` at 16,
  `key_len:u32` at 24, and `state:u32` at 28. State 0 is empty, 1 occupied, and
  2 tombstone. An empty slot is all zero. A tombstone has bytes 0..27 zero and
  the LE state value 2 at 28..31. No slot state is an independent publication
  token.
- In an occupied slot, `key_ref` is nonzero and uses the biased Box rule;
  `key_ref-1` names exactly `key_len` raw key bytes in Box data. Key length must
  be positive and fit `u32`. Slot occupation supplies value presence:
  `value_ref=0` is stored None, while a positive reference names a non-None
  canonical TLV at `value_ref-1`. Its exact logical length is recovered from
  the TLV header. Every addition and bias is checked; all live rounded Box
  intervals are in bounds, unique, and non-overlapping. HashBox keys and
  non-None values never allocate from or fall back to the common Blob heap.
- Exactly two same-capacity table images exist solely for whole-table
  publish-last recovery. They are two physical images of one logical map, not
  resize storage or a second index. `DelTree` scans occupied slots in the
  transaction-visible image so a user-replaced Index cannot hide physically
  stored keys.
- The naturally aligned common-header `active_table:u32` is the only HashBox
  publication token. Creation zeroes both complete table images and initializes
  it to 0; it is acquire-loaded as 0 or 1, and any other value is corruption.
  There is no duplicate selector, per-table epoch, journal, or commit checksum.
  Every read and mutation remains under the common robust mutex, so no reader
  can retain the old image across publication.
- Before a standalone Put, Erase, or other non-Clear mutation zeroes one byte of
  the inactive image or changes Box metadata/data, it read-only scans the
  complete selected active image in ascending physical-index order and
  validates slot canonicality, persisted hashes, probe reachability, unique
  keys, values, the complete Box ownership/interval set, allocator metadata,
  exact counters, and all required rebuild/rollback scratch. The same preflight
  performs the requested lookup, recomputes occupied count solely from slots,
  and proves the projected result is at most `entry_limit`; a new key requires
  the old count to be below the limit while a replacement remains legal at the
  limit. It does not scan common queues because such a mutation cannot touch or
  reference the physically disjoint common heap. Clear instead performs the
  broader global Clear preflight before any queue write and reaches this engine
  apply phase only after queue clearing succeeds. Any preflight failure changes
  zero bytes.
- After the applicable preflight, a non-Clear mutation completely zeroes the
  inactive image and re-inserts the prevalidated occupied source slots using
  their persisted hashes; active tombstones are not copied. HashBox Clear
  instead leaves that inactive image canonical empty. An over-limit selected
  image is corruption without trusting stored `entry_count`. The selected
  active image and every Box object it reaches remain immutable and allocated
  until commit publication.
- Staged lookup stops at the first empty slot. Insert remembers the first
  tombstone, continues probing for an equal key, and uses that tombstone before
  the terminating empty slot. Replacement preserves the existing key
  allocation and changes only the staged value reference. Erase writes the
  canonical tombstone encoding: state 2 with all data fields zero. Batch-created
  allocations unreachable from both the committed and current staged image may
  be reclaimed before the next staged operation. Entry-limit, Box-capacity, or
  full-probe failure leaves the committed selector and counters unchanged.
- Commit preflights the complete staged table, including canonical slot bytes,
  probe reachability, unique keys, canonical TLVs, and the complete live Box
  interval set. It independently recomputes the staged occupied count, requires
  it to be at most `N`, and treats an over-limit staged image as corruption; the
  installed counters are derived from this scan rather than any old counter.
  It then release-stores the new common selector as the last semantic
  publication action. Only after that store may it reclaim the exact set of old
  or batch-created Box intervals not reachable from the final image; an object
  is never freed merely because the old image referenced it, because the final
  image may share the same key or value allocation. The old image is non-
  authoritative and may then be zeroed, but correctness never depends on its
  pre-recovery contents.
- Rollback never changes the common selector. It reclaims all batch-created
  intervals not reachable from the still-selected image and zeroes the staged
  image; if ordinary allocator rollback is interrupted or fails, recovery
  rebuilds Box solely from the selected image. A failed mutation therefore
  leaves a usable, attachable committed state.
- Owner-death recovery acquire-loads exactly one selected image and ignores the
  other. Before rewriting allocator metadata it preflights every selected slot,
  recomputes each key hash and TLV length, checks linear-probe reachability and
  duplicate keys, validates the complete Box interval set, and recomputes the
  selected occupied count solely from those slots. Invalid selected states,
  noncanonical empty/tombstone bytes, bad occupied slots, overlap, an occupied
  count greater than `N`, or an invalid selector are corruption and are never
  repaired by converting a slot to a tombstone. Recovery then rebuilds Box from
  only those intervals, derives `entry_count`, `tombstone_count`, and
  `engine_live_bytes`, leaves `root_offset` and `node_count` zero, and zeroes the
  ignored image. Torn Box allocator metadata and old derived counters are never
  authorities; repeating recovery after death during a prior recovery is valid.
- HashBox `engine_live_bytes` is the sum of
  `BoxAllocator::RoundSize(key_len)` for every occupied key plus
  `BoxAllocator::RoundSize(logical_value_len)` for every occupied non-None
  value. It excludes both table images, the engine and allocator headers, Box
  metadata, section padding, stored None, and the unowned terminal tail.
- HashBox has no resize, persisted link cache, duplicate selector, per-image
  counter/header, or second logical index. Such mechanisms are outside this
  ABI rather than optional interpretations of it.

## TrieBox (engine 4)

- The TrieBox engine ABI is exactly 4. Its persistent engine header has magic
  `KVTRIE01`, version 1, and exactly 128 bytes, encoded as follows: magic at
  0..7; `version:u32=1` at 8..11; `header_bytes:u32=128` at 12..15;
  `committed_root:u32` at 16..19; zero at 20..23; then eight `u64` section
  fields at 24/32/40/48/56/64/72/80 in this order:
  `node_metadata_offset`, `node_metadata_bytes`, `node_zone_offset`,
  `node_zone_bytes`, `box_metadata_offset`, `box_metadata_bytes`,
  `box_data_offset`, `box_data_bytes`; immutable geometry hash at 88..95; and
  zero-reserved bytes 96..127.
- The immutable header hash is FNV-1a 64-bit over exactly: the eight magic
  bytes, `version` as LE32, the constant header length 128 as LE64, and the
  eight section fields above as LE64 in field order. The mutable committed
  root and every reserved byte are excluded. The engine layout fingerprint is
  a separate FNV-1a 64-bit hash over, in this exact order: the eight ASCII bytes
  `TRIEBOX2`, then little-endian `u64` encodings of `sizeof(RegionHeader)`, the
  header-byte constant 128, `TrieNodeCodec::kEncodedBytes`,
  `FixedBlockAllocator::kPersistentMetadataBytes`,
  `BoxAllocator::kPersistentHeaderBytes`, `BoxAllocator::kNodePayloadBytes`,
  `sizeof(QueueSlot)`, `sizeof(BlobHeader)`, `sizeof(pthread_mutex_t)`, and
  `sizeof(pthread_cond_t)`. In particular, the header length is fed as LE64;
  this format does not use a 64-byte alternative header.
- Let `E` be the common header's canonical `engine_offset`, `R` be
  `region_max`, and `P` be the host page size. The one canonical geometry is
  `B=page_up(E+128)`, `U=R-B`,
  `node_budget=page_floor(U/2)`, and
  `box_budget=U-node_budget`. The 64-byte FixedBlock metadata begins at `B`;
  its node zone immediately follows and consumes the remainder of
  `node_budget`. Box metadata begins at `B+node_budget`; its data size `D` is
  `BoxAllocator::LargestFullyRepresentableDataSize(box_budget)`, its metadata
  size `M` is
  `BoxAllocator::MinimumMetadataBytesForFullExpansion(D)`, and Box data begins
  immediately after those `M` bytes. The bytes from the end of `D` through
  `R` are an unowned terminal tail and are neither initialized nor validated.
  The alignment gap `[E+128,B)` is canonical zero padding.
- Creation computes that geometry, including canonical FixedBlock header
  width/capacity and the `INT32_MAX+1` node-capacity bound, before writing the
  backing object. Attach treats stored descriptors as untrusted scalar bytes,
  recomputes the same geometry solely from `E,R,P`, requires all eight fields
  and the immutable hash to match exactly, validates the alignment gap as
  zero, and thereafter uses only the recomputed geometry. Arithmetic or
  allocator-geometry failure is capacity at Create and corruption at Attach.
- There is exactly one `FixedBlockAllocator` for nodes and one `BoxAllocator`
  for non-None values.
- A node payload is exactly 1032 bytes. Bytes 0..7 are little-endian
  `(value_ref << 1) | has_value`; bytes 8..1031 are 256 little-endian signed
  32-bit child IDs. `-1` is empty and block ID 0 is valid.
- `value_ref` is at most `UINT64_MAX >> 1` before packing. Valid child IDs are
  0 through `INT32_MAX`, so node capacity is at most `INT32_MAX + 1`.
- `has_value=false` requires `value_ref=0`. `has_value=true,value_ref=0` is
  stored None; a positive value reference follows the biased Box rule.
- `UINT32_MAX` is only the pre-publication/no-root sentinel. A header marked
  ready must contain a valid allocated empty root, normally block ID 0; the
  sentinel is never a valid ready-state root. Published nodes are
  immutable. Erase clears the terminal value and canonically prunes every
  newly value-less, child-less non-root node on the copied path; the valid
  empty root itself is retained. A mutation batch accumulates path copies but
  publishes only its final root. After successful root publication it reclaims
  all and only node IDs and Box intervals unreachable from that final root,
  including replaced paths, pruned suffixes, intermediate staged paths, and a
  subtree made unreachable by `DelTree`. Owner-death recovery derives the
  complete live node bitmap and Box interval set from the committed root,
  rejects cycles or shared nodes, and rebuilds both allocators; it never trusts
  a partially reclaimed free list. Before either engine allocator's mutable
  metadata is written, recovery completes the node bitmap/codec traversal and
  the raw Box TLV, interval, overlap, metadata-capacity, and scratch-resource
  preflight for both allocators. It applies the node plan and then the Box plan;
  interruption between those applies is replayable and both plans are
  idempotently derived again from the same committed root.
- TrieBox has one deliberate distinction between physical allocator liveness
  and logical counters. A canonical root with no value and no children remains
  allocated and present in the FixedBlock live bitmap, but is the logical empty
  trie: `node_count=0`, `entry_count=0`, and `engine_live_bytes=0`. For any
  nonempty trie, `node_count` counts every reachable node including the root;
  `entry_count` counts every reachable `has_value`, including stored None; and
  `engine_live_bytes` is exactly `node_count*1032` plus
  `BoxAllocator::RoundSize(logical_value_length)` for every reachable non-None
  canonical TLV. `root_offset`, `tombstone_count`, and `active_table` are always
  zero for TrieBox. Clean Attach independently traverses the committed root,
  requires these exact stored values and `entry_count<=entry_limit`, and still
  requires the physical empty root to be a valid allocated block. Owner-death
  recovery derives and checks the same logical values and physical root/live
  bitmap before its first repair write, then installs the counters from its
  prepared plan. Tests cover a fresh/cleared physical root, root-only None,
  root-only non-None, deeper values, pruning back to the retained root, corrupted
  stored counters, an unallocated physical root, and recovery interrupted
  between node and Box allocator rebuilds.

## Capacity layout

- All section offsets and capacities are fixed and fingerprinted at creation;
  later mutation never moves a section.
- The fixed-section engines ArtBump, ArtBox, HashBox, and TrieBox extend the
  backing object to `max_size` at creation so every allocator section is safely
  mappable; regular files and POSIX shm use
  sparse zero-filled pages, so this reserves address/offset space rather than
  eagerly writing every page. `region_size` therefore reports `max_size` for
  these engines; `initial_size` remains a compatibility lower-bound check and
  is not a movable-arena boundary.
- ArtBox reserves `5 * max_entries + 1` slots in each of its four node classes:
  a committed ART has at most `2N-1` nodes, the union of committed and current
  staged trees has at most `4N-2`, a non-inserting update allocates at most
  `N` replacement nodes, and an inserting prefix split allocates at most
  `N+1` before the superseded staged path is reclaimable.
  ArtBump reserves exactly `4 * max_entries + 1` slots in each class under its
  normative staged-path turnover invariant; both ordinary batch mutation and
  full Compact have a per-class peak no greater than two complete trees, or
  `4N-2` nodes total. These per-class bounds deliberately cover every class
  distribution rather than guessing a workload ratio. All arithmetic and the
  ArtBox 30-bit local-ID limit are checked before creating the backing object.
  Creation fails if `max_size` cannot hold the fixed slabs plus minimum
  variable storage.
- ArtBump uses the exact post-slab `floor(remaining/2)` raw-zone geometry above;
  a possible final odd byte is unowned. ArtBox gives the remaining engine
  budget to Box metadata plus data.
- HashBox places its two fixed table images before the common queue table, then
  gives its complete tail engine budget to the 64-byte `KVHBOX01` header and
  the canonical Box metadata/data pair described above.
- TrieBox divides its engine budget equally between the one 1032-byte node
  slab and Box metadata-plus-data. `max_entries` limits stored values, not trie
  nodes; exhaustion of either fixed zone returns capacity without corruption.
- For each non-Hash fixed-section engine (ArtBump, ArtBox, and TrieBox), let
  `P=4096`, `CQ=queue_capacity`,
  `Q=page_up(sizeof(RegionHeader),P)`,
  `QT=checked(CQ*32)`, and `H=page_up(checked(Q+QT),P)`. Its common queue table
  is exactly `[Q,Q+QT)`. Creation stores `queue_offset=Q`, both
  `table_capacity=0`, `table_offset[0]=table_offset[1]=0`, and
  `active_table=0`; every byte in
  `[sizeof(RegionHeader),Q)` and `[Q+QT,H)` is zero. After the checked common-
  heap split below derives `E`, it stores `heap_offset=H`, `heap_limit=E`,
  `engine_offset=E`, and checked `engine_size=R-E`. Attach recomputes this geometry from
  accepted immutable scalars and requires every one of those fields, both zero
  table offsets, the zero selector, and both padding ranges to match exactly.
- For HashBox, let `P=4096`, `CT=table_capacity`, `CQ=queue_capacity`,
  `TB=checked(CT*32)`,
  `T0=page_up(sizeof(RegionHeader),P)`,
  `T1=page_up(checked(T0+TB),P)`,
  `Q=page_up(checked(T1+TB),P)`, and
  `H=page_up(checked(Q+checked(CQ*32)),P)`. Thus the exact order is common
  header, table 0, table 1, common queue table, and common queue heap. The gaps
  `[sizeof(RegionHeader),T0)`, `[T0+TB,T1)`, `[T1+TB,Q)`, and from the end of
  the queue table to `H` are zero at creation and validated as zero at Attach.
  Creation stores `table_offset[0]=T0`, `table_offset[1]=T1`, `queue_offset=Q`,
  `heap_offset=H`, and, after the checked split derives `E`,
  `heap_limit=engine_offset=E` and `engine_size=R-E`; Attach recomputes and
  exactly validates each field. `active_table` is the separate mutable 0/1
  HashBox publication selector and is initialized to 0, not an input to this
  geometry.
- For each fixed-section engine, `H` is the page-aligned end of its common queue
  table. After proving `R=region_max>=H`, checked subtraction forms `U=R-H`,
  and exactly `page_floor(U/8)` bytes form the common
  queue heap `[H,E)`. The remaining bytes form the tail engine section
  `[E,R)`. Creation requires at least one page in the common heap and
  enough tail bytes for that engine's minimum canonical geometry. Every engine
  has `E=checked(H+page_floor(U/8))`; HashBox begins `KVHBOX01` exactly at `E`.
  All section math
  includes container overhead, is overflow-checked and deterministic, and is
  covered by the common and engine fingerprints.
- Geometry tests independently corrupt every stored queue/table/heap/engine
  offset or size and every required-zero padding range. Boundary cases cover
  `queue_limit=1`, capacities at each next-power-of-two transition, checked
  `CQ*32`, `CT*32`, page-up, `R-H`, and `H+page_floor((R-H)/8)` overflow or
  underflow, and a tail one byte/page below each engine's minimum; every invalid
  case rejects before a persistent write or out-of-bounds dereference.
