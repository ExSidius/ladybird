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
