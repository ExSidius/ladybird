# WASM-DOM host interface (prototype)

A JS-free path for a WebAssembly module to manipulate the DOM. A page loads
`<script type="application/wasm-dom" src="app.wasm">`; the module imports DOM
operations from a host interface named `dom`, implemented in C++ directly
against LibWeb — no JS import object, no JS glue, no JavaScript execution.
Any language that compiles to wasm32 gets DOM access and inherits the WASM
sandbox. This is architecturally natural in Ladybird because LibWasm and LibJS
are separate libraries and the DOM is plain C++ (in Chromium, WASM lives
inside V8 and the DOM's bindings assume it).

**Scope note (the realm question).** A realm-free DOM is impossible in LibWeb:
every DOM node *is a* `JS::Object`, node allocation resolves prototypes through
the realm's intrinsics, and the VM exists from WebContent startup. What this
prototype guarantees instead: on the wasm-dom path **no JavaScript is ever
parsed or executed** — the realm exists but is inert. `WasmDOMScript::run()`
deliberately never touches the JS execution context stack.

## Architecture

A *host interface* (a "platform" in Roc terms), not a runtime:

- `Libraries/LibWeb/WebAssembly/DOMHost/DOMHostInstance.{h,cpp}` — one GC cell
  per script: owns the `Wasm::AbstractMachine` (heap-allocated `OwnPtr`, never
  moves), the parsed module, the module instance, and the **handle table**.
- `Libraries/LibWeb/WebAssembly/DOMHost/HostFunctions.{h,cpp}` — resolves the
  module's `dom.*` imports to `Wasm::HostFunction`s allocated in the instance's
  own store (the JS-free analogue of `WebAssembly.cpp:instantiate_module`).
- `Libraries/LibWeb/WebAssembly/DOMHost/WasmDOMScript.{h,cpp}` — the
  `HTML::Script` subclass; `run()` = validate → link → instantiate → `_start`.
- Routing in `HTMLScriptElement` (`ScriptType::WasmDOM` arms), raw-bytes fetch
  in `HTML/Scripting/Fetching.cpp` (`fetch_wasm_dom_script`, classic-script
  request semantics, no text decoding).

### ABI (v0)

- wasm32 only; import module name `dom`. The guest must export `memory`,
  `_start` (called once after instantiation), and — for events —
  `__indirect_function_table`.
- **Handles**: opaque `i32`; generational (8-bit generation | 24-bit index into
  a per-instance table); `0` is null; slot 0 reserved. Stale/wrong-kind handle →
  trap. **Identity-cached and refcounted**: acquiring the same cell again returns
  the same handle (guest handle equality = object identity, the analogue of the
  engine's JS wrapper cache); `dom.release` decrements, and the slot frees (and
  the value goes stale) when acquires and releases balance. A live handle pins
  its node.
- **Handle entries are traced `GC::Ptr`s** (visited by `DOMHostInstance`), not
  `GC::Root`s: node ↔ instance cycles are ordinary same-heap GC cycles and are
  collected when the document goes away. No listener-cycle leak by design.
- **Strings** guest→host: `(ptr: i32, len: i32)` UTF-8, bounds-checked; OOB or
  invalid UTF-8 → trap. **Interning**: `dom.intern(ptr, len) -> id` stores a
  string once, pre-converted to String/FlyString/Utf16String; `(id, 0xFFFFFFFF)`
  is accepted anywhere a string parameter is expected, skipping the per-call
  copy, transcode, and FlyString hashing. Host→guest: caller-provided buffer
  `(dst_ptr, dst_cap)`; host writes `min(len, cap)` bytes and returns the full
  length; guest retries with a larger buffer if `len > cap`. ASCII-backed
  UTF-16 DOM strings are written with no transcode or allocation.
- **Callbacks** (M2): `i32` index into the guest's exported funcref table plus
  `i32 user_data`; uniform signature `(i32 arg, i32 user_data) -> ()`.
- **Errors**: expected failures return `0`/`-1`; ABI violations trap
  (`Wasm::Trap::from_string`), aborting the guest invocation. `ExceptionOr` is
  converted at the boundary; nothing C++ crosses into the machine.

### Host functions

| import | signature | notes |
|---|---|---|
| `get_element_by_id(ptr,len)` | (i32,i32)→i32 | 0 if not found |
| `create_element(ptr,len)` | (i32,i32)→i32 | HTML namespace; 0 on invalid name |
| `create_text_node(ptr,len)` | (i32,i32)→i32 | |
| `append_child(parent,child)` | (i32,i32)→i32 | −1 if rejected |
| `set_attribute(node,nptr,nlen,vptr,vlen)` | (i32×5)→i32 | element handles only (else trap) |
| `get_attribute(node,nptr,nlen,dst,cap)` | (i32×5)→i32 | M1; −1 if absent, else full length |
| `text_content_get(node,dst,cap)` | (i32×3)→i32 | M1; −1 if null |
| `text_content_set(node,ptr,len)` | (i32×3)→i32 | M1 |
| `add_event_listener(node,tptr,tlen,cb,user)` | (i32×5)→i32 | M2 |
| `event_type(event,dst,cap)` | (i32×3)→i32 | M2 |
| `release(handle)` | (i32)→() | |

All parameters/results are i32, so a signature is fully described by its arity;
declared types are checked against the table at link time.

### Events (M2)

There is no non-JS listener slot in the engine, so `DOM::DOMEventListener`
gains a second nullable field (`native_callback`) and
`EventDispatcher::inner_invoke` gets a native branch that skips the JS-only
block (realm lookup, Window current-event bookkeeping,
`call_user_object_operation`, exception reporting) and calls the guest export
via `machine.invoke`. No `prepare_to_run_callback`: the task-boundary microtask
checkpoint covers the bookkeeping. `EventTarget::add_an_event_listener` needs
null-guards (its dedup path dereferences the JS callback unconditionally).

### Async (M3, implemented)

`dom.set_timeout(ms, cb, user_data)` and `dom.fetch(url, cb, user_data)` follow
the wakeup model: start now, deliver later by invoking a guest funcref-table
callback from the event loop (never nested inside a guest invocation). The
guest's own async runtime (if any) rides inside its module; the host provides
operations and wakeups only. `dom.fetch` goes through the real Fetch stack in
CORS mode with the document as client — cross-origin reads need CORS opt-in,
and (like the fetch() API) it is blocked on file:// pages by the file-scheme
exfiltration guard, so fetch tests run via the HTTP test server
(`TestConfig.ini` `[LoadFromHttpServer]`). Responses surface as a Response
handle (`response_status` / `response_read`, buffer-retry ABI) valid only
during the completion callback.

### Real-language guests (implemented)

`Tests/LibWeb/Text/data/wasm-dom/guests/` holds a **Rust** guest (no_std,
typed Node wrapper over the interface, click counter with in-module string
formatting; 1.3 KB) and a **Zig** guest (fetch + timer via the async ABI;
0.9 KB), with committed binaries and build instructions. On wasm32 a function
pointer is its funcref-table index in both languages, so callbacks are just
`f as usize as i32` / `@intFromPtr(&f)` — the ABI needs no language-specific
support. `wasm-dom-polyglot.html` runs both modules on one page (each script
gets its own machine and handle table).

## Verification

- Text tests: `Tests/LibWeb/Text/input/Wasm/wasm-dom-*.html` with committed
  binary fixtures under `Tests/LibWeb/Text/data/wasm-dom/` (`.wat` sources
  alongside as documentation; no wasm toolchain needed to build or test).
- JS in the test pages is verification harness only — it inspects the DOM the
  wasm module built; the wasm path itself never runs JS.
- Full `test-web` + `test LibWeb` regression (script-element and event-dispatch
  edits touch every page).

## Out of scope (v0)

Async host ops, WebIDL-driven host-function codegen, JSPI/stack switching,
MIME enforcement on the module response, inline wasm (binary format), state
sharing between multiple wasm-dom scripts, cross-heap cycle work beyond the
traced-handle design.
