use wasm_bindgen::prelude::*;

/// Minimal deterministic consistency check for the starter build.
/// This is not a cryptographic proof verifier.
#[wasm_bindgen]
pub fn verify_transition(previous: u32, input: u32, claimed: u32) -> bool {
    let expected = previous.rotate_left(5) ^ input.wrapping_mul(0x9E37_79B9);
    expected == claimed
}
