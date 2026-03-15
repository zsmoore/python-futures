# cfuture

**CompletableFuture-style futures with true GIL-free parallelism via Python sub-interpreters.**

Requires Python 3.12+.

---

## The Problem: Python's GIL

Python's Global Interpreter Lock (GIL) is a mutex that protects access to Python objects, preventing multiple native threads from executing Python bytecodes simultaneously. This means that CPU-bound work running in multiple threads does not benefit from multiple CPU cores — they take turns holding the lock.

The standard workarounds each have significant trade-offs:

- **`threading`** — GIL prevents true CPU parallelism for Python code
- **`multiprocessing`** — True parallelism, but high IPC overhead and awkward state sharing
- **`asyncio`** — Concurrency without parallelism; single-threaded cooperative multitasking
- **`concurrent.futures.ProcessPoolExecutor`** — Parallelism via processes, but pickle round-trips for every call

Python 3.12 introduced **sub-interpreters** — isolated Python interpreter instances that can run in the same process without sharing the GIL. Each sub-interpreter has its own GIL, own object space, and own module state. This enables **true parallel execution in multiple threads within a single process**.

`cfuture` builds a thread pool backed by sub-interpreters and a **CompletableFuture**-style API inspired by Java's `java.util.concurrent.CompletableFuture`.

---

## Architecture

```
Main Thread (Python caller)
      |
      |  pool.submit(fn)
      v
 ┌──────────────────────────────────────────────────┐
 │               ThreadPoolExecutor                 │
 │                                                  │
 │  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
 │  │ Worker 0 │  │ Worker 1 │  │ Worker N │  ...  │
 │  │ (interp) │  │ (interp) │  │ (interp) │       │
 │  └────┬─────┘  └────┬─────┘  └────┬─────┘       │
 │       │             │             │              │
 │  ┌────┴─────────────┴─────────────┴──────────┐  │
 │  │              Task Queue (linked list)      │  │
 │  └────────────────────────────────────────────┘  │
 │                                                  │
 │  ┌──────────────────────────────────────────┐    │
 │  │     Watchdog Thread (fault tolerance)    │    │
 │  └──────────────────────────────────────────┘    │
 └──────────────────────────────────────────────────┘
      |
      v
 SharedValue (neutral-heap representation)
      |
      |  sv_from_pyobject()     sv_to_pyobject()
      v
 ┌──────────────────────────────────────────────────┐
 │  SV_NONE | SV_BOOL | SV_INT | SV_FLOAT           │
 │  SV_STR  | SV_BYTES | SV_LIST | SV_DICT | SV_TUPLE │
 │  SV_CUSTOM (__xi_encode__/__xi_decode__)          │
 │  SV_PICKLE (opt-in via cfuture.pickled())         │
 └──────────────────────────────────────────────────┘
```

The key insight: Python objects cannot be shared between sub-interpreters (they have separate object heaps). `cfuture` solves this by serializing values to a **neutral C heap representation** (`SharedValue`) before crossing interpreter boundaries, then deserializing them in the destination interpreter.

---

## Installation

```bash
pip install cfuture
```

Or from source:

```bash
git clone https://github.com/example/cfuture
cd cfuture
pip install -e .
```

Build the C extension:

```bash
python setup.py build_ext --inplace
```

---

## Quick Start

```python
import cfuture

# Create a thread pool with 4 workers
pool = cfuture.ThreadPoolExecutor(workers=4)

# Submit work — returns a Future immediately (non-blocking)
future = pool.submit(lambda: expensive_computation())

# Block for result (with optional timeout)
result = future.result(timeout=30.0)

pool.shutdown()
```

---

## Full API Reference

### `cfuture.ThreadPoolExecutor(workers=4, shared=None)`

Creates a thread pool backed by `workers` threads.

**Parameters:**
- `workers` (int): Number of worker threads. Default: 4. Range: 1–256.
- `shared` (dict): Optional dictionary of named values to make available to all workers at startup. Values must be serializable via `SharedValue` encoding.

**Methods:**
- `submit(fn)` → `Future`: Submit a zero-argument callable. Returns a `Future`.
- `shutdown(wait=True)`: Shut down the pool. If `wait=True`, blocks until all running tasks complete.
- `__enter__` / `__exit__`: Context manager support (calls `shutdown()` on exit).

**Example:**

```python
with cfuture.ThreadPoolExecutor(workers=8, shared={"config": my_config}) as pool:
    futures = [pool.submit(lambda: do_work()) for _ in range(100)]
    results = [f.result(timeout=60.0) for f in futures]
```

---

### `cfuture.Future`

A lazy, chainable future value.

**Instance Methods:**

- `result(timeout=None)` → value: Block until the future resolves and return its value. Raises `TimeoutError` on timeout, `RuntimeError` on task failure.
- `done()` → bool: Return `True` if the future has completed (successfully or with error).
- `cancelled()` → bool: Return `True` if the future was cancelled.
- `cancel()` → bool: Cancel the future if it has not yet started. Returns `True` if cancellation succeeded.
- `then(fn, deps=[])` → `Future`: Register a success callback. `fn` is called as `fn(result, deps_tuple)`. Returns a new `Future` for the callback's return value.
- `except_(fn, deps=[])` → `Future`: Register a failure callback. `fn` is called as `fn(error_str, deps_tuple)`.
- `finally_(fn, deps=[])` → `Future`: Register a callback that fires regardless of success or failure.

**Class Methods:**

- `Future.completed(value=None)` → `Future`: Create an already-resolved future with the given value.
- `Future.failed(message="failed")` → `Future`: Create an already-failed future.

---

### `cfuture.all_of(*futures)` → `Future`

Non-blocking combinator. Returns a `Future` that resolves to a list of results when all input futures complete.

- Does not block any worker threads while waiting.
- If called with zero arguments, returns an immediately-resolved future with `[]`.
- All arguments must be `Future` instances.

```python
f1 = pool.submit(lambda: fetch_data("source_a"))
f2 = pool.submit(lambda: fetch_data("source_b"))
f3 = pool.submit(lambda: fetch_data("source_c"))

combined = cfuture.all_of(f1, f2, f3).then(
    lambda results, d: merge(results),
    deps=[],
)
```

---

### `cfuture.pickled(obj)` → `Pickled`

Wraps an arbitrary Python object for pickle-based cross-interpreter transfer. Use this as an escape hatch for objects that don't support the native `SharedValue` encoding.

```python
import datetime

dt = datetime.datetime.now()
future.then(
    lambda x, d: process(d[0]),
    deps=[cfuture.pickled(dt)],  # datetime is not natively transferable
)
```

**Warning:** Pickle is slower than native SharedValue encoding and requires objects to be pickle-able. Prefer native types or `@xi_dataclass` when possible.

---

### `@cfuture.xi_dataclass`

Decorator that auto-implements `__xi_encode__` and `__xi_decode__` for `dataclasses.dataclass` types, enabling them to be transferred across interpreter boundaries without pickle.

```python
import dataclasses
import cfuture

@cfuture.xi_dataclass
@dataclasses.dataclass
class Config:
    host: str
    port: int
    timeout: float

cfg = Config("localhost", 8080, 30.0)
future.then(lambda x, d: connect(d[0]), deps=[cfg])
```

---

## Cross-Interpreter Data Transfer

When passing values across interpreter boundaries (as task results or `deps`), `cfuture` encodes them to a neutral C heap representation. Here is what is supported natively:

| Python Type | SharedValue Tag | Notes |
|-------------|-----------------|-------|
| `None` | `SV_NONE` | |
| `bool` | `SV_BOOL` | Checked before `int` |
| `int` | `SV_INT` | 64-bit signed |
| `float` | `SV_FLOAT` | Double precision |
| `str` | `SV_STR` | UTF-8 encoded |
| `bytes` | `SV_BYTES` | Raw copy |
| `list` | `SV_LIST` | Recursive encoding |
| `tuple` | `SV_TUPLE` | Recursive encoding |
| `dict` | `SV_DICT` | Recursive encoding (str/int/float keys) |
| Custom (with `__xi_encode__`) | `SV_CUSTOM` | Via encode/decode protocol |
| Anything (opt-in) | `SV_PICKLE` | Via `cfuture.pickled()` |

**Not supported (raises `TypeError` at registration time):**

- File handles, sockets, database connections
- Lambda closures with free variables
- Any object without `__xi_encode__` and not wrapped with `cfuture.pickled()`

---

## Callback `deps` Model

The `deps` parameter in `.then()`, `.except_()`, and `.finally_()` is analogous to the dependency array in React's `useEffect` hook — it declares which values from the outer scope the callback needs, and those values are snapshot-copied at registration time.

```python
# WRONG — captures 'threshold' from outer scope (CFU001 lint error)
threshold = 100
future.then(lambda x, d: x > threshold)

# CORRECT — declare 'threshold' in deps
threshold = 100
future.then(lambda x, d: x > d[0], deps=[threshold])
```

The no-free-variables rule is enforced at runtime by `cfuture` (checking `co_freevars`), and at lint time by the `CFU001` rule in `cfuture.lint`.

**Why this matters:** Sub-interpreters have separate object heaps. A lambda that closes over a Python object from the main interpreter cannot safely access that object from a worker interpreter. The `deps` mechanism ensures all required values are explicitly encoded and transferred.

---

## CFU001 Lint Rule

`cfuture` ships a flake8 plugin and standalone checker that detects closure captures in cfuture callbacks.

**CFU001:** `callback captures 'name' from outer scope — pass via deps=[name] instead`

### Standalone usage:

```bash
python -m cfuture.lint myfile.py
```

### flake8 integration:

The plugin registers automatically via the entry point. Run:

```bash
flake8 --select=CFU myfile.py
```

### Example violation:

```python
# mycode.py
scale = 2.0
pool.submit(lambda: 0).then(lambda x, d: x * scale, deps=[])
#                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
# CFU001: callback captures 'scale' from outer scope — pass via deps=[scale] instead
```

---

## Comparison: cfuture vs Standard Alternatives

| Feature | `cfuture` | `asyncio` | `threading` | `multiprocessing` |
|---------|-----------|-----------|-------------|-------------------|
| True CPU parallelism | Yes | No | No (GIL) | Yes |
| In-process | Yes | Yes | Yes | No |
| IPC overhead | Zero (C heap) | N/A | N/A | High (pickle/pipe) |
| Callback chaining | Yes (.then) | Yes (await) | No | No |
| Non-blocking combinator | Yes (all_of) | Yes (gather) | No | No |
| Fault tolerance | Yes (watchdog) | Manual | Manual | Partial |
| Lint tooling | Yes (CFU001) | No | No | No |
| Python version | 3.12+ | 3.4+ | Any | Any |

---

## gRPC Server Example

See `examples/grpc_server.py` for a full demonstration of integrating cfuture with a gRPC-style server:

```python
import cfuture

pool = cfuture.ThreadPoolExecutor(
    workers=8,
    shared={"config": {"timeout": 5.0}},
)

def handle_request(request_id: str) -> cfuture.Future:
    # Fan out to multiple services in parallel
    auth  = pool.submit(lambda: call_auth_service())
    data  = pool.submit(lambda: call_data_service())
    cache = pool.submit(lambda: call_cache_service())

    # Compose results non-blocking
    return (
        cfuture.all_of(auth, data, cache)
        .then(lambda results, d: compose(results, d[0]), deps=[{"id": request_id}])
        .except_(lambda err, d: error_response(err, d[0]), deps=[{"id": request_id}])
    )

# Main thread never blocks — pipeline is wired, not executed
pipeline = handle_request("req-001")
response = pipeline.result(timeout=10.0)
```

---

## Fault Tolerance (P1 Watchdog)

`cfuture` includes a watchdog thread that monitors all workers. If a worker thread dies (e.g., from a signal, OS error, or unrecoverable exception), the watchdog:

1. Detects the dead worker via its `WORKER_DEAD` atomic state flag
2. Re-queues the in-flight task (if any) back to the task queue
3. Spawns a replacement worker thread

This ensures the pool remains operational even if individual workers crash.

**Note:** Exceptions raised inside task functions are caught, captured as strings, and delivered to the `Future` as failures — they do not kill the worker.

---

## Signal Handling

Worker threads properly release the GIL while waiting for tasks (`PyEval_SaveThread` / `PyEval_RestoreThread`), ensuring that signal handlers in the main thread continue to run normally.

---

## Python 3.12+ Requirement

`cfuture` requires Python 3.12 or later because:

1. **Sub-interpreter per-interpreter GIL** — Python 3.12 introduced `Py_TFLAGS_ISOLATED_INTERPRETERS` and the `_Py_NewInterpreterFromConfig` API for creating truly isolated sub-interpreters with their own GIL.
2. **`PyInterpreterState_Get()`** — stable API for accessing the current interpreter state
3. **`PyErr_GetRaisedException()`** — cleaner exception retrieval API introduced in 3.12

---

## Development

```bash
# Install dev dependencies
pip install -e ".[dev]"

# Build extension
python setup.py build_ext --inplace

# Run tests
pytest tests/ -v

# Run lint checker
python -m cfuture.lint cfuture/ tests/ examples/

# Run flake8 with cfuture plugin
flake8 --select=CFU tests/
```

---

## License

MIT License. See `pyproject.toml`.
