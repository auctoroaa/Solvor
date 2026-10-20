# AXIOM C++20 Engine Core

## Build and test

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --parallel
ctest --test-dir engine/build --output-on-failure
```

## Implemented

- Deterministic DAG layering and stable task ordering.
- Declared tile-access conflict checks and transactional tick updates.
- Rollback when a task fails before commit.
- SHA-256 state digests and a deterministic hash-chain verifier.
- CRC32C-protected binary write-ahead log records, synced before a tick is accepted.
- CRC32C-protected genesis tile state, atomically replaced and synced.
- Restart recovery validating tick order, before/after state digests and record CRCs; invalid/torn WAL tail is truncated.
- Bounded LRU tile-cache primitive with pin-aware eviction.

## Still unfinished

The cache is a tested primitive, not yet the engine's backing store. Active state remains in an in-memory map. WAL segment rotation, a StatePager integrated with the cache, optional io_uring I/O, and broader fault-injection/concurrency testing remain unfinished. This is not yet the complete production AXIOM Reality Engine.
