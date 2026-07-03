# wasm-dom guests in real languages

Sources for the committed `rust-guest.wasm` / `zig-guest.wasm` fixtures. Any
language that compiles to wasm32 can target the `dom` host interface; the only
ABI requirements are:

- import functions from module `"dom"` (see `WASM_DOM_HOST_PLAN.md` at the repo
  root for the full surface),
- export linear memory as `memory`, the funcref table as
  `__indirect_function_table` (only needed for callbacks), and a `_start`
  function,
- pass strings as `(ptr, len)` and callbacks as function-table indices — on
  wasm32, a function pointer *is* its table index in both Rust and Zig.

Building the fixtures is only needed when changing the sources; the binaries
are committed so the test suite has no toolchain dependency.

## Rust (`rust/`)

```sh
cd rust
RUSTFLAGS="-C link-args=--export-table" \
    cargo build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/wasm_dom_rust_guest.wasm ../../rust-guest.wasm
```

`--export-table` makes LLD export `__indirect_function_table`; memory and
`#[no_mangle] _start` are exported by default for cdylib targets.

## Zig (`zig/`)

```sh
cd zig
zig build-exe guest.zig -target wasm32-freestanding -O ReleaseSmall \
    -fno-entry --export=_start --export-table -femit-bin=../../zig-guest.wasm
```
