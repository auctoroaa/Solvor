# AXIOM WebAssembly

A minimal, buildable starting point for the AXIOM browser deployment target.

## Build locally

Install the Rust stable toolchain, the `wasm32-unknown-unknown` target, `wasm-bindgen-cli`, and Binaryen (`wasm-opt`).

```bash
rustup target add wasm32-unknown-unknown
cargo build --workspace --target wasm32-unknown-unknown --release
mkdir -p dist/axiom dist/verifier
wasm-bindgen --target web --out-dir dist/axiom --out-name axiom_core target/wasm32-unknown-unknown/release/axiom_core.wasm
wasm-bindgen --target web --out-dir dist/verifier --out-name verifier target/wasm32-unknown-unknown/release/zero_verifier.wasm
wasm-opt dist/axiom/axiom_core_bg.wasm -O4 -o dist/axiom/axiom_core_opt.wasm
wasm-opt dist/verifier/verifier_bg.wasm -O4 -o dist/verifier/verifier_opt.wasm
```

GitHub Actions runs these steps on pushes to `main` and uploads generated files as the `axiom-wasm-dist` artifact.

## Scope

This repository provides compileable WASM entry points, generated TypeScript declarations, and an example WGSL shader. The transition function and verifier are starter implementations, **not** the full AXIOM engine or a cryptographic proof system. WebGPU shader execution is not wired into Rust yet; a browser/WebGPU host must load and dispatch the shader. Artifact checksums establish file identity, not correctness of the engine.
