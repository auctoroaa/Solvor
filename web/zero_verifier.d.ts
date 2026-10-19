/** Handwritten ABI declarations for the current minimal WASM exports.
 * zero_verify_u32 is a simple equality check, not cryptographic verification.
 */
export declare function zero_verifier_version(): number;
export declare function zero_verify_u32(expected: number, actual: number): number;
