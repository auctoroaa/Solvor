# AXIOM C++20 Engine Core

## Build and test

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --parallel
ctest --test-dir engine/build --output-on-failure
```

## Implemented and tested

- Deterministic DAG layering and stable task ordering.
- Declared tile-access conflict checks and transactional tick updates.
- Rollback when a task fails before commit.
- SHA-256 state digests and a deterministic hash-chain verifier.
- CRC32C-protected binary write-ahead log records with fsync before a tick is accepted.
- CRC32C-protected, atomically replaced genesis state for initial tiles.
- Restart recovery that validates tick order, before/after state digests, record CRCs, and truncates an incomplete or invalid WAL tail.
- Bounded LRU `TileCache` primitive with pin-aware eviction and tests.

## Remaining integration work

The cache is currently a tested primitive, not yet the engine's backing store. The engine still keeps its active state in an in-memory map. WAL segment rotation, a complete StatePager that pages state through the bounded cache, optional io_uring I/O, and broader fault-injection/concurrency testing remain unfinished. Do not treat this as the complete production AXIOM Reality Engine yet.
