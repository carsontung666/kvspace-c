# kvspace-c

[![CI](https://github.com/array2d/kvspace-c/actions/workflows/ci.yml/badge.svg)](https://github.com/array2d/kvspace-c/actions/workflows/ci.yml)

C implementation of the **KVSpace** used by kvlang — the filesystem-style key-value store that serves as kvlang's unified addressing and memory space (keys are paths, values are XValues).

This is one of two standard implementations of the KVSpace contract; the other is [kvspace-durable](../kvspace-durable). Both expose the same C ABI and the same XValue kindexpr format, so a consumer (the kvlang layout/runtime) switches between them by DSN only.

Backend: `shm://` — single file-backed mmap block (ART-tree key index + slotsboxmalloc value storage), shared across processes.

## Build

```bash
make            # → build/libkvspace-c.so
```

Dependencies: [`blockmalloc`](../blockmalloc), [`slotsboxmalloc`](../slotsboxmalloc) (header-only + `.so` dual-mode libraries).

## API

Two surfaces:

1. **Native C API** (`kvspaceShm*`) — `include/kvspace/kvspace.h`:
   `kvspaceShmOpen/Close`, `kvspaceShmGet/Set`, `kvspaceShmList`, `kvspaceShmDel/Deltree`, `kvspaceShmMkindex`, `kvspaceShmExtindex/Delextindex`, `kvspaceShmNotify/Watch`.

2. **durable C ABI** — byte-compatible with `kvspace-durable` (`src/durable_abi.c`):
   `kvspaceConnect/Close/Disconnect`, `kvspaceGet` (zero-copy borrow) `/WriteInPlace/WriteNewPlace`,
   `kvspaceListLen/ListAt/Del/DelTree/Cp/CpTree`,
   `kvspaceMkindex/MkindexExt/RmindexExt/Watch/Clear`,
   `kvspaceTlvEncode/TlvEncodeMode/DecodeHead`,
   `kvspaceNewPtr/NewChar/NewBool/NewInt64/NewFloat64`.

   A consumer (e.g. the kvlang layout) links this ABI and switches backends by DSN only, with no code change.

## XValue

kindexpr XValue uses the headlenpow wire format:

```
[pow:u8][flags:u8][a:u64le][b:u64le][langtype, padded to 2^pow][body]
```

- The low two flag bits select fixed, sized, tensor, or extension storage; bit 2 marks a pointer.
- Directory members come from physical key prefixes. `@ext` stores its locator in class 3.
- `ro` and `vid` live at `/.kvspace-meta/<hex-encoded key>`.

The codec accepts `None`, scalars, sized byte and character arrays, tensors, maps, and code values.

## Tutorial

```bash
python3 tutorial/test.py
```

Cases: `01_basic.c`, `02_cpp.cpp`, `03_python.py`, `04_rust.rs`, `05_integrity.c`, `06_multiprocess.c`.

## Language wrappers

- `py/` — Python ctypes binding.
- `rust/` — Rust FFI crate.
