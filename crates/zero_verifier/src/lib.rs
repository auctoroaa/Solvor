#![no_std]

// Minimal consistency-check entry point for the build pipeline.
// This is NOT cryptographic verification; replace with the real verifier.

#[no_mangle]
pub extern "C" fn zero_verifier_version() -> u32 {
    1
}

#[no_mangle]
pub extern "C" fn zero_verify_u32(expected: u32, actual: u32) -> u32 {
    (expected == actual) as u32
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}
