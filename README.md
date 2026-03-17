# cfuture

CompletableFuture-style futures with true GIL-free parallelism via CPython
sub-interpreters (PEP 554 / Python 3.12+). Each worker runs in its own
sub-interpreter so CPU-bound tasks genuinely run in parallel without
contending on the GIL.

## Installation

```sh
python3 setup.py build_ext --inplace
```

This compiles the C extension in-place, producing `cfuture/_cfuture.*.so`.

## Quick start

```python
import cfuture

with cfuture.ThreadPoolExecutor(workers=4) as pool:
    f = pool.submit(lambda: 42)
    print(f.result(timeout=5.0))   # 42
```

> **Note:** On `main` today, `submit` takes a zero-argument callable (data
> flows via `deps=` on `.then()`).  The `feat/stress-test` branch extends this
> to `submit(fn, *args, **kwargs)` — see [In development](#in-development-featstress-test) below.

## API reference

### `ThreadPoolExecutor`

```python
pool = cfuture.ThreadPoolExecutor(workers=N, shared={...})
```

- `workers`: number of sub-interpreter worker threads
- `shared`: optional dict of read-only data injected into every callback as the third argument

Use as a context manager (`with pool:`) to guarantee shutdown on exit.

### `pool.submit(fn)` (current main)

Submits `fn` (a zero-argument callable) to the pool. Returns a `Future`.

`fn` must not be a closure — it cannot capture variables from an enclosing scope.
Pass data via `deps=` instead (see below).

### `Future.then(fn, deps=[])`

Chains a callback to run when the future resolves successfully.

```python
f.then(lambda result, deps, shared: result + deps[0], deps=[offset])
```

- `fn` receives `(result, deps, shared)`:
  - `result`: the value produced by the preceding step
  - `deps`: list of values passed via `deps=`
  - `shared`: dict built from the pool's `shared=` argument
- Callback arity is flexible: `fn` may accept 1, 2, or 3 positional arguments
- Returns a new `Future`

### `Future.except_(fn, deps=[])`

Chains a callback to run if the future raises an exception.

```python
f.except_(lambda exc, deps, shared: -1, deps=[])
```

`fn` receives `(exception, deps, shared)`. Returns a new `Future`.

### `Future.finally_(fn, deps=[])`

Chains a callback that always runs, regardless of success or failure.

```python
f.finally_(lambda result, deps, shared: cleanup(deps[0]), deps=[resource])
```

Returns a new `Future`.

### `cfuture.all_of(*futures)`

Returns a `Future` that resolves with a list of all results when every input
future has completed.

```python
futures = [pool.submit(lambda: i) for i in range(4)]
all_f = cfuture.all_of(*futures)
results = all_f.result(timeout=10.0)
```

### `Future.result(timeout=None)`

Blocks until the future completes and returns its value. Raises on error.

### `Future.completed(value)`

Class method. Returns a pre-resolved `Future` — useful for starting a chain.

```python
cfuture.Future.completed(0).then(lambda x, d, s: x + 1, deps=[])
```

### `@xi_dataclass` and the xi-protocol

Worker sub-interpreters cannot share Python objects directly. The xi-protocol
serialises objects at the call site and deserialises them inside the worker.

Use `@cfuture.xi_dataclass` on any `@dataclasses.dataclass` to opt in:

```python
import dataclasses
import cfuture

@cfuture.xi_dataclass
@dataclasses.dataclass
class Point:
    x: float
    y: float
```

The decorator adds `__xi_encode__` and `__xi_decode__` methods automatically.
You can also implement them manually on any class.

**Important:** xi-protocol classes must be defined at module level, not inside
functions. The worker resolves the class by importing its module and walking its
`__qualname__`, so `<locals>` in the qualname will cause a runtime error.

## Passing data to callbacks

### `deps=` — per-call data

Pass a list of values that should be available inside a specific callback:

```python
multiplier = 3

with cfuture.ThreadPoolExecutor(workers=2) as pool:
    f = pool.submit(lambda: 10).then(
        lambda x, d, s: x * d[0],
        deps=[multiplier],
    )
    print(f.result(timeout=5.0))  # 30
```

### `shared=` — pool-wide read-only data

Pass a dict to the pool constructor for data that every callback should see.
It is encoded once at startup and injected as the `shared` argument:

```python
config = {"timeout": 30, "retries": 3}

with cfuture.ThreadPoolExecutor(workers=4, shared=config) as pool:
    f = pool.submit(lambda: "ok").then(
        lambda result, d, s: f"{result} (timeout={s['timeout']})",
        deps=[],
    )
    print(f.result(timeout=5.0))  # ok (timeout=30)
```

## Lint rules

Install the flake8 plugin or run standalone:

```sh
python3 -m cfuture.lint myfile.py
```

### CFU001 — closure capture

```
CFU001 callback captures 'x' from outer scope — pass via deps=[x] instead
```

Fires when a lambda passed to `submit`, `then`, `except_`, or `finally_`
references a name from an outer scope that is not in `deps=`.

### CFU002 — xi-protocol class inside a function

```
CFU002 xi-protocol class 'Point' defined inside a function — move to module level so the worker can resolve it
```

Fires when a class decorated with `@xi_dataclass` (or implementing
`__xi_encode__`/`__xi_decode__`) is defined inside a function.

## In development (`feat/stress-test`)

The following features are implemented on the `feat/stress-test` branch and
under evaluation before merging to main.

### Pythonic `submit(fn, *args, **kwargs)`

```python
def process(records, config):
    return transform(records, config)

with cfuture.ThreadPoolExecutor(workers=4) as pool:
    f = pool.submit(process, my_records, my_config)
    print(f.result(timeout=5.0))
```

Arguments are encoded as `SharedValue` — no pickle, no closure capture needed.
The legacy `submit(fn)` + `.then(callback, deps=)` API remains supported.

### `own_gil=True` — true GIL-free parallelism

```python
pool = cfuture.ThreadPoolExecutor(workers=4, own_gil=True)
```

Each worker gets its own GIL via `Py_NewInterpreterFromConfig`. CPU-bound
pure-Python tasks run in genuine parallel.

### Transparent C-extension fallback

When `own_gil=True`, some C extension modules (e.g. `json`, `_json`) cannot
load in sub-interpreters. cfuture detects the `ImportError` and transparently
reroutes the task to the main interpreter:

1. Worker catches the import failure (two catch points: module import and `PyObject_Call`)
2. Task is written to `main_pipe` and picked up by the main thread
3. A per-pool module cache (`main_only_modules`) avoids repeated failed imports

This is invisible to user code — the same `pool.submit(fn, *args)` call works
regardless of whether `fn` uses C extensions.

**Benchmark results (gRPC server, JSON serialization workload, 4 workers):**

| Metric | cfuture v2 (own_gil + fallback) | multiprocessing | Ratio |
|--------|-------------------------------|-----------------|-------|
| QPS | 68,190 | 28,347 | **2.41x** |
| mean latency | 0.12 ms | 0.28 ms | 2.3x |
| p99 latency | 0.35 ms | 0.95 ms | 2.7x |
| boot time | 62 ms | 148 ms | 2.4x |

Even with all tasks falling back to main (json C ext can't load in
sub-interpreters), cfuture wins because it avoids pickle serialization
overhead.

**When fallback helps vs. hurts:**
- Wins on C extensions that **hold the GIL** (json, protobuf) — cfuture
  avoids pickle overhead even when running on main
- Loses on C extensions that **release the GIL** (Rust-backed tokenizers) —
  fallback serializes work through main, losing the parallelism the extension
  already provides via GIL release

### `pool.map(fn, iterable)`

Generator-based parallel map, analogous to `concurrent.futures` map:

```python
with cfuture.ThreadPoolExecutor(workers=4) as pool:
    results = list(pool.map(process, items))
```

### asyncio integration (`done_fd`, `main_fd`)

File descriptors for zero-polling integration with asyncio event loops:

```python
loop = asyncio.get_running_loop()
loop.add_reader(pool.done_fd, pool._drain_done_callbacks)
loop.add_reader(pool.main_fd, pool._drain_main_tasks)
```

### Stress test suite

Full gRPC benchmark infrastructure in `examples/stress_test/`:

- `cfuture_server.py` — cfuture v1 (deps API) gRPC server
- `cfuture_server_v2.py` — cfuture v2 (Pythonic submit + fallback) with `--mode json|pure`
- `mp_server.py` — multiprocessing baseline with `--mode json|pure`
- `asyncio_server.py` — asyncio baseline
- `runner.py` — concurrent gRPC stress tester with QPS/latency reporting
- `prompt_bench.py` — 1k-item HuggingFace tokenizer prompt-construction benchmark
- `submit_bench.py` — submit API comparison (v1 deps vs v2 Pythonic vs mp)

## Testing

### Python tests

```sh
python3 -m pytest tests/ -v
```

Expected: **142 passed** (main), **147 passed** (feat/stress-test).

### C-level Unity tests

Direct tests of internal C logic:

| Binary | What it tests |
|--------|---------------|
| `test_shared_value` | `SharedValue` encode/decode/copy/free, `PickledObject`, `SV_CUSTOM` |
| `test_future_internals` | `task_new`/`task_free`, `validate_fn`, refcount behaviour |
| `test_build_shared_dict` | `build_shared_dict` with null/empty/populated pools |

```sh
cd tests/c && make run
```

Expected: **47 Tests 0 Failures 0 Ignored** (26 + 16 + 5 across the three binaries).

### Running everything

```sh
python3 setup.py build_ext --inplace
python3 -m pytest tests/ -v        # 142 passed
cd tests/c && make run             # 47 Tests 0 Failures 0 Ignored
```
