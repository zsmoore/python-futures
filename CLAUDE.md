# cfuture — project record

## What this project is

`cfuture` is a Python library providing CompletableFuture-style futures with
true GIL-free parallelism via CPython sub-interpreters (PEP 554 / Python 3.12+).
Each worker runs in its own sub-interpreter so CPU-bound tasks genuinely run
in parallel without contending on the GIL.

---

## Original plan vs. what shipped

The project was built as a sequence of focused PRs, each adding one layer.
Below is the original intent for each PR and what actually landed.

### PR 1 — Core sub-interpreter thread pool (`feat/core-pool`)

**Plan:** Bare-minimum pool: `ThreadPoolExecutor(workers=N)`, `pool.submit(fn)`,
`Future.result(timeout)`. C extension with a `pthread`-based worker pool where
each thread owns a `Py_NewInterpreterFromConfig` sub-interpreter.

**What shipped:**
- `ThreadPoolExecutor`, `Future`, `pool.submit`, `Future.result`
- C extension split into focused translation units from day one:
  `_cfuture_internal.h`, `_cfuture_shared_value.c`, `_cfuture_pickled.c`,
  `_cfuture_future.c`, `_cfuture_worker.c`, `_cfuture.c` (module init only)
- Unity C-level test suite (`tests/c/`) for SharedValue and future internals
- `validate_fn` rejects closures at submit time (free-variable check)

**Diverged from plan:** The multi-TU split was originally a separate planned
refactor; it was folded into PR 1 so every subsequent PR started from a clean
architecture.

---

### PR 2 — Basic Future (`feat/future-basics`)

**Plan:** `Future.completed(value)` class method, `Future.cancel()`.

**What shipped:**
- `Future.completed(value)` — pre-resolved future, useful for starting chains
- `Future.result(timeout)` with proper blocking/wakeup via condition variable
- Additional C-level refcount tests for task lifetime
- `test_futures.py` separated from `test_core_pool.py`

**Diverged from plan:** `Future.cancel()` was not implemented — cancellation
semantics for in-flight sub-interpreter tasks are complex and deferred.

---

### PR 3 — Cross-interpreter SharedValue encoding (`feat/shared-value`)

**Plan:** A tagged-union C struct (`SharedValue`) that can encode Python
scalars, lists, tuples, dicts, bytes, and custom xi-protocol objects across
interpreter boundaries without pickling.

**What shipped:**
- `SVTag` enum: `SV_NONE`, `SV_BOOL`, `SV_INT`, `SV_FLOAT`, `SV_STR`,
  `SV_BYTES`, `SV_LIST`, `SV_TUPLE`, `SV_DICT`, `SV_PICKLE`, `SV_CUSTOM`
- `sv_from_pyobject`, `sv_to_pyobject`, `sv_deep_copy`, `sv_free`
- `PickledObject` fallback for types not natively supported
- 26 Unity C tests covering all tags, deep copy independence, free safety

**Bug fixed during this work:** `SV_CUSTOM` was grouped with `SV_LIST`/`SV_TUPLE`
in both `sv_free_inline` and `sv_deep_copy_inline`, causing it to read/write
`sv->list` instead of `sv->dict`. Fixed by grouping `SV_CUSTOM` with `SV_DICT`.

---

### PR 4 — Callback chaining (`feat/callbacks`)

**Plan:** `.then(fn, deps=[])`, `.except_(fn, deps=[])`, `.finally_(fn, deps=[])`
returning new `Future` instances. Callbacks run in a worker sub-interpreter.

**What shipped:**
- All three callback methods with `deps=` passing
- Callback signature: `fn(result, deps, shared)` — 3 args (shared added in PR 6,
  but the 3-arg signature was locked in here to avoid a breaking change later)
- Use-after-free fix: `out_future` refcount management in `future_add_callback`
- C tests for out_future refcount behaviour

---

### PR 5 — Non-blocking `all_of` (`feat/all-of`)

**Plan:** `cfuture.all_of(*futures)` returns a `Future` that resolves with a
list of all results once every input future completes.

**What shipped:**
- `cfuture.all_of(*futures)` — varargs, not a list
- Internal `AllOfCallback` C type that counts down and fires when all complete
- Fixed `allof_callback_call` to accept 3-arg callback signature (`"OOO"`)

---

### PR 6 — Shared startup dependencies (`feat/shared-deps`)

**Plan:** `ThreadPoolExecutor(workers=N, shared={...})` — encode a dict once at
pool construction and inject it into every callback as the third `shared` arg.

**What shipped:**
- `shared=` keyword argument on `ThreadPoolExecutor`
- `build_shared_dict(Pool*)` — non-static, deep-copies shared templates into a
  fresh `PyDict` per callback invocation so callbacks cannot mutate each other's copy
- `fire_callbacks` passes pool (not worker) so shared dict can be built from pool
- Inline path in `future_add_callback` also builds and passes shared dict
- 5 Unity C tests for `build_shared_dict` (`tests/c/test_build_shared_dict.c`)
- 7 new Python integration tests in `test_deps.py` covering shared access,
  independence, empty-dict-when-no-shared, shared+deps together

---

### PR 7 — Custom class transfer (`feat/xi-classes`)

**Plan:** `@xi_dataclass` decorator and `__xi_encode__`/`__xi_decode__` protocol
so arbitrary Python objects can be passed via `deps=` or `shared=`.

**What shipped:**
- `cfuture/_xi.py` — home for `xi_dataclass` decorator (NOT in `lint.py`)
- `xi_dataclass` exported from `cfuture/__init__.py` as `cfuture.xi_dataclass`
- Worker resolves class by `PyImport_Import(module)` + getattr walk of `__qualname__`
- **Module-level restriction:** xi-protocol classes must be defined at module
  scope. `<locals>` in `__qualname__` cannot be navigated by the worker.
- 3 SV_CUSTOM Unity C tests: encode tag, free safety, deep copy independence
- `test_xi_classes.py` — all class definitions at module level

---

### PR 8 — CFU001/CFU002 lint rules (`feat/lint`)

**Plan:** `cfuture/lint.py` — AST-based flake8-compatible checker. CFU001:
closure capture in callback. CFU002: xi-protocol class inside a function.

**What shipped:**
- `cfuture/lint.py` with `check_file()`, `CfutureLintPlugin`, standalone CLI
- CFU001: fires when a lambda passed to `submit`/`then`/`except_`/`finally_`
  references a name not in `deps=` or the lambda's own params
- CFU002: fires when a class with `@xi_dataclass` or `__xi_encode__`/`__xi_decode__`
  is defined inside a function
- `xi_dataclass` deliberately NOT in `lint.py` — lives in `cfuture/_xi.py`
- 13 tests in `test_lint.py` (CFU001 and CFU002 cases)

---

### PR 9 — Docs and examples (`feat/docs-rewrite`)

**Plan:** README and `examples/grpc_server.py` written from scratch to reflect
the actual shipped API (previous docs were stale).

**What shipped:**
- `README.md` — full API reference: `ThreadPoolExecutor`, `submit`, `then`,
  `except_`, `finally_`, `all_of`, `Future.completed`, `xi_dataclass`,
  `deps=`, `shared=`, CFU001/CFU002, build/test instructions
- `examples/grpc_server.py` — fan-out RPC pattern with `@xi_dataclass` types,
  `shared=` config, `deps=`, `all_of(*futures)`, `with pool:` context manager

**Note:** `all_of` takes `*futures` (varargs), not a list — caught and fixed
while verifying the example ran.

---

### PR 10/12 — P1 worker fault tolerance (`feat/p1-watchdog`)

**Plan:** Watchdog thread that detects dead worker slots and respawns them,
keeping the pool at declared `workers=N` even after crashes.

**What shipped:**
- Watchdog implementation was already in `_cfuture_worker.c`:
  `watchdog_fn` (polls all slots) + `respawn_worker` (restarts a dead worker)
- `tests/test_fault.py` — 5 subprocess-based tests so real crashes cannot kill
  pytest: smoke test, exception resilience, many tasks, chained callbacks, all_of
- Branch started fresh off main (old branch had a stale merge history)

**Bugs fixed vs. original branch:**
- Lambda signatures updated to 3-arg (`lambda x, d, s:`)
- `test_many_tasks` had a CFU001 violation (`lambda: i * i` capturing loop var)
- All subprocess code strings converted to `with pool:` pattern

---

## Final state

| Metric | Count |
|--------|-------|
| Python tests (`pytest tests/`) | **98 passed** |
| C Unity tests (`tests/c/make run`) | **47 passed** (26 + 16 + 5) |
| Merged PRs | **10** (#1–#8, #11, #12) |
| C source files | 5 (+ 1 header) |
| Python source files | `__init__.py`, `_xi.py`, `lint.py` |

---

## Conventions established during development

- **Callback signature is always `fn(result, deps, shared)`** — 3 args, no exceptions.
- **`with pool:`** — always use context manager; never call `pool.shutdown()` manually in tests.
- **No closure captures in callbacks** — pass data via `deps=` (per-call) or `shared=` (pool-wide).
- **xi-protocol classes at module level** — worker cannot navigate `<locals>` in `__qualname__`.
- **`xi_dataclass` lives in `cfuture._xi`**, exported via `cfuture.__init__`. Not in `lint.py`.
- **C tests via Unity** in `tests/c/` — each binary includes only the TU it tests, no full-extension include hacks.
- **PR scope discipline** — lint changes stay out of xi PRs; xi decorator stays out of lint PRs.
