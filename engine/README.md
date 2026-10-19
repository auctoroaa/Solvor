# AXIOM C++20 Core

Build and test:

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --parallel
ctest --test-dir engine/build --output-on-failure
```

This target implements deterministic DAG layering, stable task ordering, tile-access conflict checks, transactional state updates, rollback on task or persistence failure, SHA-256 state digests, a hash-chain verifier, and fsync-backed commit records.

Scope note: this is a working C++ core, not a claim that every planned subsystem is complete. A bounded LRU pager, full WAL replay/torn-write repair, and an io_uring backend remain to be integrated and tested. The existing Rust/WASM scaffold is preserved separately.
