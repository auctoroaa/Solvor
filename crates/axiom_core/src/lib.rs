use wasm_bindgen::prelude::*;

const SHADER_SOURCE: &str = include_str!("../../../src/shaders/compute.wgsl");

/// Build identifier for the initial AXIOM browser module.
#[wasm_bindgen]
pub fn axiom_version() -> String {
    env!("CARGO_PKG_VERSION").to_owned()
}

/// Returns WGSL source for browser-side WebGPU compilation.
#[wasm_bindgen]
pub fn shader_source() -> String {
    SHADER_SOURCE.to_owned()
}

/// Deterministic starter operation. Replace with the real AXIOM transition engine.
#[wasm_bindgen]
pub fn evaluate_transition(state: u32, input: u32) -> u32 {
    state.rotate_left(5) ^ input.wrapping_mul(0x9E37_79B9)
}
