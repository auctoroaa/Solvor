// Starter WGSL shader. The browser/WebGPU implementation compiles this at runtime.
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    // Placeholder kernel: replace when AXIOM's GPU data layout is defined.
    let index = id.x;
}
