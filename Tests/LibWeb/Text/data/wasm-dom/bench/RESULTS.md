# wasm-dom benchmark results

Three variants of identical DOM workloads, all in Ladybird (release build,
macOS arm64, Apple Silicon, 2026-07-02). The Rust module is byte-for-byte the
same workload code in both wasm variants — only the import module differs.

- **JS**: plain JavaScript (`bench-js.html`), timed with `performance.now()`.
- **wasm+glue**: the Rust module with its imports implemented by a JS import
  object holding an object heap and decoding strings from wasm memory — the
  wasm-bindgen architecture, hand-rolled (`bench-glue.html`).
- **wasm-dom**: the Rust module on the native C++ host interface
  (`bench-dom.html`), timed with `dom.now` (same monotonic clock family).

Workloads: **build** = create 2,000 divs, 2 setAttribute each, append;
**text** = 4,000 textContent write+read round-trips (~34-byte strings);
**query** = 5,000 getElementById + getAttribute. 3 process runs x 5 in-page
reps = 15 samples; medians reported. Run with:
`Ladybird --headless=text --force-new-process file://.../bench-<variant>.html`

### v1 — baseline ABI (2026-07-02)

| workload | JS | wasm+glue | wasm-dom | dom vs js | dom vs glue |
|---|---|---|---|---|---|
| build | 5.90 ms | 14.00 ms | **3.44 ms** | **1.72x faster** | **4.07x faster** |
| text  | 2.70 ms |  9.00 ms | 3.38 ms | 0.80x (slower) | **2.66x faster** |
| query | 0.70 ms | 10.30 ms | 1.05 ms | 0.67x (slower) | **9.81x faster** |

### v2 — interned strings + identity-cached handles + ASCII fast path (2026-07-03)

Fixes applied to exactly the two measured gaps: `dom.intern` (strings stored
once, pre-converted to all flavors; `(id, 0xFFFFFFFF)` anywhere a string is
expected — the glue variant got the equivalent, mirroring wasm-bindgen string
caching), identity-cached refcounted handles (same element -> same i32; the
analogue of the engine's JS wrapper cache), and a zero-transcode write path
for ASCII-backed UTF-16 DOM strings.

| workload | JS | wasm+glue | wasm-dom | dom vs js | dom vs glue |
|---|---|---|---|---|---|
| build | 6.20 ms | 7.70 ms | **3.24 ms** | **1.91x faster** | **2.38x faster** |
| text  | 2.70 ms | 5.90 ms | 3.07 ms | 0.88x | **1.92x faster** |
| query | 0.70 ms | 4.90 ms | 0.79 ms | 0.89x | **6.20x faster** |

The two JS losses narrowed from 0.67x/0.80x to ~0.9x — within the noise floor
of the quantized JS clock for *query*. The residual *text* gap is the copy ABI
itself (JS hands the engine's string to script without copying; we must write
bytes into guest memory) — closing it fully needs shared/externref strings,
not a faster copy.

## Reading

1. **The glue tax is real and large.** The identical module runs 2.7–9.8x
   faster against the native host interface than through JS-glue imports in the
   same engine. Every glue crossing pays JS function dispatch, TextDecoder
   work, and heap-array indexing; the native path pays none of it.
2. **wasm-dom beats even pure JS at bulk DOM construction** (1.72x): the
   workload is dominated by engine-internal work, and the wasm path skips the
   JS bindings layer (JS value conversion, prototype lookups) entirely.
3. **The two losses to JS are exactly the predicted gaps.** *text* pays the
   string copy+UTF-8/UTF-16 transcode in both directions per iteration
   (JS strings pass through the bindings without re-encoding); *query* is a
   cheap hash lookup where per-call handle allocate/release overhead dominates.
   Interned strings and identity-cached / externref handles are the known
   fixes for precisely these two costs.

## Caveats

Micro-workloads on one machine. The glue column measures *Ladybird's* JS
boundary — a V8-class engine makes glue cheaper (and JS faster) in absolute
terms, so these ratios do not transfer to Chrome; the apples-to-apples claim
is only about the three paths within this engine. JS numbers are quantized by
performance.now resolution (0.1 ms), which matters for *query*.

---

# Extended suite (2026-07-03)

Seven benchmark families probing strengths and limitations beyond the original
boundary micro-benchmarks. Pages: `suite-*.html`, `events-*.html`,
`chain-*.html`, `app-*.html`, `startup-compile.html`, `memory-*.html`; guests:
`rust-suite`, `rust-chain`, `rust-app`, `events-bench.wat`-style module.
Medians of 9 samples (3 process runs x 3 in-page reps) unless noted. Cranelift
native compilation enabled for guest code (verified engaged: fib is 21% slower
with it disabled).

## 1. Pure compute (identical algorithms, zero crossings)

| workload | JS | wasm | wasm vs JS |
|---|---|---|---|
| fib(27), recursive calls | 6.70 ms | 25.52 ms | **0.26x — 3.8x slower** |
| matmul 64x64 f64 (x4) | 30.60 ms | 10.40 ms | **2.94x faster** |
| FNV-1a over 512 KB (x8) | 52.60 ms | 21.57 ms | **2.44x faster** |

**Finding:** LibWasm beats LibJS decisively on loop/arithmetic code but loses
badly on call-heavy code — function-call overhead is the engine's weak spot
even with the JIT. An app should keep hot paths loop-shaped (or the engine
needs call-path JIT work). This is an execution-engine property, independent
of the DOM interface.

## 2. Crossing cost by operation weight

| op (per-call cost) | JS | wasm+glue | wasm-dom |
|---|---|---|---|
| noop x100k | 12 ns | 44 ns | **22 ns** |
| setAttribute x20k | 725 ns | 860 ns | **197 ns** |
| querySelector(complex) x2k | 700 ns | 950 ns | 780 ns |

**Findings:** the raw boundary crossing is ~22 ns — only ~2x a plain JS
function call, and 2x cheaper than a glue crossing. For a real mutation
(setAttribute) the native path is **3.7x faster than JS**, because the JS
bindings layer (value conversion, dispatch) costs far more than the crossing.
For engine-dominated ops (querySelector) all paths converge — the boundary
stops mattering, as predicted.

## 3. Payload size sweep (textContent set+get, raw strings)

| size | JS | wasm+glue | wasm-dom | dom vs js |
|---|---|---|---|---|
| 16 B x8k | 5.60 | 16.90 | **4.74** | 1.18x faster |
| 256 B x4k | 3.60 | 15.10 | **2.44** | 1.48x faster |
| 4 KB x1k | 1.20 | 22.20 | 1.16 | parity |
| 64 KB x120 | 0.60 | 37.90 | 1.10 | 0.55x |
| 512 KB x16 | 0.50 | 40.00 | 1.01 | 0.50x |
| 64 KB non-ASCII x120 | 0.80 | 41.60 | 5.09 | 0.16x |

**Findings:** the copy-ABI crossover is ~4 KB. Below it the native path wins
(lower per-call overhead); above it JS wins by passing references while we
memcpy (~15 GB/s, so the loss is bounded at 2x); non-ASCII adds the UTF-16
transcode and widens the loss to 6x. Glue collapses at every size
(TextDecoder/TextEncoder per crossing). Fixes: reference-passing
(shared/externref strings) for large payloads; nothing needed below 4 KB.

## 4. Handle-table scaling (30k-node sibling walk)

| walk | JS | wasm+glue | wasm-dom |
|---|---|---|---|
| cold (allocate 30k handles) | 3.70 | 6.90 | 5.40 |
| warm (identity-cache hits) | 1.60 | 5.10 | 3.27 |
| churn (release as you go) | 1.50 | 7.40 | 4.65 |

**Finding:** pointer-chasing traversal is the boundary's worst shape — two
crossings per node, ~55 ns each even warm, vs JS property reads at ~27 ns.
The identity cache scales fine (warm 40% faster than cold at 30k entries);
the residual cost is the crossing count itself. The fix is API shape (bulk
child-list reads), not a faster crossing.

## 5. Event dispatch (5000 dispatches, minimal listener, same JS driver)

JS listener 2.30 ms; native wasm listener 2.50 ms — **parity** (~40 ns/dispatch
penalty ≈ per-dispatch event-handle allocate/release + machine.invoke entry).
Dispatch machinery dominates both paths.

## 6. Async wakeups (500 sequential zero-delay hops)

wasm dom.set_timeout chain **5.0 ms (10 µs/hop)**; JS setTimeout chain 9.7 ms
(19 µs/hop); JS microtask chain 0.10 ms (0.2 µs/hop — different mechanism, for
scale). The wakeup ABI is 2x cheaper than JS timers; guest async runtimes can
afford ~100k wakeups/sec.

## 7. Startup and memory

Module compile+validate (`new WebAssembly.Module`, same LibWasm machinery):
linear at ~70 µs/KB — 1.7 KB: 0.4 ms; 46 KB real module: 5.5 ms; 288 KB:
20.6 ms. A 100-module page adds no measurable wall clock over a 100-script JS
page (0.20 s vs 0.19 s) and **+6 MB RSS total (~60 KB per module environment,
mostly the module's own 64 KB linear memory)** — per-script machines are cheap;
the feared per-instance overhead did not materialize.

## 8. App-shaped (keyed list, 200 rows, 500 state ops, minimal updates)

JS 0.80 ms; wasm-dom 0.87 ms — **parity**. Held row handles (identity cache as
wrapper cache) + interned attribute names make the boundary invisible next to
the engine's mutation work. For realistic fine-grained UI code, language
choice is free of performance cost in either direction.

## Synthesis

Strengths: loop/arithmetic compute (2.4-2.9x), DOM mutation throughput (3.7x),
small-payload strings, async wakeups (2x), startup and memory (negligible
cost), and realistic app workloads (parity). Limitations, each now quantified
with its known fix: call-heavy wasm code (engine JIT work), large-payload
strings (reference passing), and chatty pointer-chasing traversal (bulk APIs).
Nothing measured contradicts the architecture; the two structural losses are
properties of the copy ABI and the execution engine, not of the host-interface
design.
