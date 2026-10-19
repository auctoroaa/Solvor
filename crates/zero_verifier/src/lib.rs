use wasm_bindgen::prelude::*;

/// Minimal deterministic consistency check for the starter build.
/// This is not a cryptographic proof verifier.
#[wasm_bindgen]
pub fn verify_transition(previous: u32, input: u32, claimed: u32) -> bool {
    let expected = previous.rotate_left(5) ^ input.wrapping_mul(0x9E37_79B9);
    expected == claimed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_matching_transition_and_rejects_tampering() {
        let previous = 19;
        let input = 23;
        let claimed = previous.rotate_left(5) ^ input.wrapping_mul(0x9E37_79B9);
        assert!(verify_transition(previous, input, claimed));
        assert!(!verify_transition(previous, input, claimed ^ 1));
    }
}
