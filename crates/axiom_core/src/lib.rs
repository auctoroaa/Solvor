#![no_std]

// Minimal compilable WASM entry points. This is a build-target scaffold,
// not the complete AXIOM engine or a cryptographic verifier.

#[no_mangle]
pub extern "C" fn axiom_core_version() -> u32 {
    1
}

#[no_mangle]
pub extern "C" fn axiom_transition(state: u32, event: u32) -> u32 {
    state.rotate_left(5) ^ event.wrapping_mul(0x9E37_79B9)
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}
