# cfuture

CompletableFuture-style futures with true GIL-free parallelism via sub-interpreters.
Requires Python 3.12+.

## Building

```sh
python3 setup.py build_ext --inplace
```

This compiles the C extension in-place, producing `cfuture/_cfuture.*.so`.

## Testing

There are two independent test suites.

### Python tests (pytest)

Covers the full public API: `Future`, `ThreadPoolExecutor`, `all_of`, `pickled`.

```sh
python3 -m pytest tests/test_core_pool.py -v
```

Expected result: **35 passed**.

### C-level Unity tests

Tests internal C logic directly, without going through the Python extension
machinery. Two test binaries are built and run:

| Binary | File | What it tests |
|--------|------|---------------|
| `test_shared_value` | `_cfuture_shared_value.c`, `_cfuture_pickled.c` | `SharedValue` encode/decode/copy/free, `PickledObject` |
| `test_future_internals` | `_cfuture_future.c` | `task_new`/`task_free`, `validate_fn` |

`_cfuture_worker.c` has no C-level tests — `fire_callbacks` requires live
Python threads and is fully covered by the pytest suite.

```sh
cd tests/c
make run
```

Expected result: **31 Tests 0 Failures 0 Ignored** (23 + 8 across both binaries).

The Makefile auto-detects Python headers and the framework path via `python3 -c "import sysconfig ..."`.
Run `make clean` to remove the compiled test binary.

### Running everything

```sh
python3 setup.py build_ext --inplace  # 35 passed
python3 -m pytest tests/test_core_pool.py -v
cd tests/c && make run                # 31 Tests 0 Failures 0 Ignored
```
