# Findings: Python scripting prototype (`<script type="text/python">`)

Spike outcome: **the mechanics work and are cheap**. Ladybird's script-element
pipeline dispatches on a `ScriptType` enum with a single ignore-fallthrough for
unknown types, so a non-JS language slots in with three small arms
(classification, inline creation, execution) plus a `Script` subclass. The
feared entanglement with JS-specific machinery (realm setup, module loading)
did not materialize for the inline-script path.

## What was built

- `Libraries/LibWeb/HTML/Scripting/PythonScript.{h,cpp}` — `PythonScript : Script`
  GC cell; `run()` reuses the spec's prepare/clean-up bookkeeping
  (`prepare_to_run_script` / `clean_up_after_running_script`) around a CPython
  evaluation instead of `vm().run(...)`.
- `Libraries/LibWeb/HTML/Scripting/PythonBindings.{h,cpp}` — all CPython usage,
  isolated behind a two-function header (`initialize_interpreter`,
  `run_source`). One generic `ladybird.DOMNode` Python type
  (`PyType_FromSpec`) whose instances own a heap `GC::Root<DOM::Node>`, with
  hand-written `getElementById` / `createElement` / `createTextNode` /
  `appendChild` / `setAttribute` / `getAttribute` / `addEventListener`
  methods and a `textContent` get/set property. Scripts run in the persistent
  `__main__` namespace with a `document` global injected per run.
- Routing in `HTMLScriptElement` (`ScriptType::Python` + three `#ifdef
  LADYBIRD_ENABLE_PYTHON` arms), interpreter bootstrap in
  `Services/WebContent/main.cpp`, CMake plumbing behind
  `ENABLE_PYTHON_SCRIPTING` (default ON; `find_package(Python3 ... Development.Embed)`).
- Event listeners: there is no non-JS listener slot in the engine, so the
  binding replicates the engine-internal pattern (see
  `Streams/AbstractOperations.cpp`): the Python callable is captured by a
  `JS::NativeFunction`, boxed as `WebIDL::CallbackType` →
  `DOM::IDLEventListener`, and registered via
  `add_event_listener_without_options`. The callable receives a dict
  `{"type": str, "target": DOMNode|None}`.
- Exceptions: uncaught Python errors (script or callback) are formatted with
  the pre-imported `traceback` module and `dbgln`'d; the error indicator is
  always cleared and nothing propagates to C++.

## Acceptance criteria status

| # | Criterion | Status |
|---|-----------|--------|
| 1 | `text/python` routes to CPython, not LibJS | Implemented (`HTMLScriptElement.cpp` classification/creation/execution arms) |
| 2 | `getElementById`, `appendChild`, `textContent` get/set, `setAttribute` | Implemented; exercised by `Tests/LibWeb/Text/input/python/python-dom-basics.html` and verified against real libpython via `embed-smoke.cpp` |
| 3 | `addEventListener` callback fires on `click` | Implemented; text test clicks the button from the JS harness (harness only — no engine-level JS/Python interop) |
| 4 | Uncaught Python exceptions logged, renderer survives | Implemented; `python-exception.html` runs a raising script followed by a working one |
| 5 | In-repo demo page | `Meta/PythonDemo/demo.html` |

## Verification

**Update (2026-07-02): fully verified locally** on macOS arm64 (AppleClang 21,
Python 3.13.7). Smoke harness, full flag-on build, both text tests (hand-written
expectations matched exactly, no rebaseline), demo page driven via WebDriver
(6 sequential clicks, away-and-back navigation, second tab, `print()` on
stdout), exception traceback logged with the renderer surviving, full test-web
suite (7611 pass / 0 fail / 0 crash), LibWeb unit tests, and a flag-off
LibWeb+WebContent build. Two fresh-configure build bugs were found and fixed
in a follow-up commit: cmake_options must be included before
check_for_dependencies (ENABLE_PYTHON_SCRIPTING was undefined at
find_package time, breaking generation), and PythonBindings.cpp was missing
the generated <LibWeb/Bindings/Document.h> include for ElementCreationOptions.
Probing the shared-namespace caveat: globals (including DOMNode wrappers)
leak into subsequently loaded documents as documented; reading a leaked
node's textContent after its document is gone works (GC::Root keeps it
alive) — no crashes.

**Full builds could not be run where this was developed** (a cloud container
whose egress policy blocks vcpkg's source downloads — GitHub release assets,
codeload tarballs, and third-party mirrors all return 403). Everything
CPython-facing was instead validated with `embed-smoke.cpp` in this directory:
a standalone harness that mirrors the binding code line-for-line against a
mock DOM and runs the actual page scripts. All scenarios pass against
libpython3.12, including exception logging, callback dispatch surviving a
raising handler, and cross-script namespace persistence.

```sh
g++ -std=c++23 -Wall -Wextra $(python3-config --includes) \
    Meta/PythonDemo/embed-smoke.cpp $(python3-config --ldflags --embed) -o smoke && ./smoke
```

On a machine with normal network access:

```sh
./Meta/ladybird.py build                     # flag is ON by default
./Meta/ladybird.py run test-web -f Text/input/python/python-dom-basics.html
./Meta/ladybird.py run test-web -f Text/input/python/python-exception.html
./Meta/ladybird.py run ladybird -- --headless text file://$PWD/Meta/PythonDemo/demo.html
```

The expected files under `Tests/LibWeb/Text/expected/python/` were written by
hand from the deterministic `println` output; if the runner's serialization
differs, regenerate with `test-web --rebaseline -f Text/input/python`.
A flag-off sanity build is `cmake ... -DENABLE_PYTHON_SCRIPTING=OFF`.

## What broke / what surprised us

1. **`print()` output silently vanished.** The interpreter is never finalized
   (it lives as long as WebContent), so Python's buffered stdout was never
   flushed — `print()` appeared to do nothing. Found by the smoke harness;
   fixed with `config.buffered_stdio = 0`. This would have been mystifying to
   debug inside the full browser.
2. **The sandbox dictates interpreter lifetime.** WebContent applies a
   Landlock filesystem sandbox at startup (`Services/RendererSandboxLinux.cpp`)
   that would block CPython from reading its stdlib, and plain
   `Py_Initialize()` **aborts the process** on failure. Hence: eager
   pre-sandbox init with `Py_InitializeFromConfig` (which returns a status),
   plus pre-importing `traceback`. Post-sandbox `import` of on-disk stdlib
   modules in page scripts fails with an ImportError (logged, non-fatal); a
   Landlock read-only path for the Python stdlib would lift that if wanted.
3. **No non-JS event listener slot.** `EventDispatcher` only invokes WebIDL
   callbacks, so "a Python listener" is really a `JS::NativeFunction`
   wrapping a `PyObject*`. Fine for a spike; a real integration would want a
   listener abstraction that isn't JS-shaped.
4. **HTML indentation vs Python.** Top-level Python cannot be indented, so
   inline scripts must start at column 0 — pretty-printed HTML breaks them.
   A `textwrap.dedent`-style pass in `run_source` would fix this.
5. **GIL is a non-issue at this scope.** Everything runs on the WebContent
   main thread, which holds the GIL from init and never releases it.
6. **The ignore-fallthrough made routing trivial** — the risk called out in
   the ticket ("execution hook entangled with JS-specific assumptions") was
   unfounded for inline scripts. External (`src=`) scripts, however, are
   fetched through `fetch_classic_script`, which is genuinely classic-script
   shaped; they were left out of scope as planned.

## Effort actuals vs estimate

Estimate was 850–2,000 LOC. Actual engine-side code is **~640 LOC**
(bindings 457 + `PythonScript` 99 + routing/bootstrap/CMake diffs ~60 +
header/forward ~30), plus ~100 lines of demo/tests and a 460-line
throwaway-quality validation harness. Wall-clock effort was on the order of a
day, dominated by reading LibWeb internals (script pipeline, GC rooting,
listener plumbing), not by writing code.

## Known limitations (deliberate, spike-scoped)

- One shared `__main__` namespace per WebContent process — state leaks across
  documents and navigations. Per-document namespaces are the obvious next step.
- Listener callables are intentionally leaked; wrappers are not
  identity-cached (`getElementById(x) is getElementById(x)` is `False`).
- `<script type="text/python" src=...>` is inert (inline only).
- CSP inline-script checks upstream of classification still apply, but no
  Python-specific CSP thought was given.
- Only `Meta/PythonDemo/embed-smoke.cpp` and the two text tests exercise the
  code; no fuzzing, no lifetime stress tests.

## Follow-up (per ticket, not this spike)

A WebIDL-driven Python binding generator and a sandboxing story remain
order-of-magnitude larger efforts. Two additional data points from this spike:
the per-method binding cost is small and mechanical (strong codegen
candidate), and object-lifetime plumbing (`GC::Root` per wrapper) is the part
that most needs a designed solution (identity-cached wrappers with weak
references) before any real-world use.
