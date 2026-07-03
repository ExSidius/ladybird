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

## Rust async (`rust-async/`)

Same build as `rust/`. Runs real Rust async/await on
`futures::executor::LocalPool` with a hand-rolled reactor over the host's
wakeup ABI (`dom.set_timeout` / `dom.fetch` completions wake parked futures).
Note that tokio itself cannot run on wasm32-unknown-unknown (its runtime needs
threads, epoll, and a clock); this is the same executor/reactor architecture
scaled to the browser's single-threaded, wakeup-driven constraints.

```sh
cd rust-async
RUSTFLAGS="-C link-args=--export-table" \
    cargo build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/wasm_dom_rust_async_guest.wasm ../../rust-async-guest.wasm
```

## Rust bench (`rust-bench/`)

The benchmark guest (see `../bench/RESULTS.md`). One crate, two binaries: the
default build imports from `dom` (native host interface); `--features glue`
imports from `glue` (a JS import object with the same ABI). The workload code
is identical, so the pair isolates the cost of the boundary itself.

```sh
cd rust-bench
RUSTFLAGS="-C link-args=--export-table" cargo build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/wasm_dom_rust_bench.wasm ../../bench/rust-bench-dom.wasm
RUSTFLAGS="-C link-args=--export-table" cargo build --release --target wasm32-unknown-unknown --features glue
cp target/wasm32-unknown-unknown/release/wasm_dom_rust_bench.wasm ../../bench/rust-bench-glue.wasm
# regenerate bench-glue.html (inlines the glue module as base64): see git history
```

## Zig (`zig/`)

```sh
cd zig
zig build-exe guest.zig -target wasm32-freestanding -O ReleaseSmall \
    -fno-entry --export=_start --export-table -femit-bin=../../zig-guest.wasm
```
