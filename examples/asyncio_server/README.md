# asyncio gRPC Server

gRPC server that handles requests using **asyncio** (`grpc.aio`).
All concurrency is cooperative — the event loop interleaves coroutines but
CPU-bound work still holds the GIL while running.

Used as the baseline for profiling against the cfuture sub-interpreter server.

## Setup

```bash
# From this directory
python3 -m venv .venv
source .venv/bin/activate

pip install grpcio grpcio-tools

# Generate gRPC stubs from the shared proto
python -m grpc_tools.protoc \
    -I../proto \
    --python_out=asyncio_server/generated \
    --grpc_python_out=asyncio_server/generated \
    ../proto/worker.proto
```

## Run

```bash
# Terminal 1 — start server
python -m asyncio_server.server

# Terminal 2 — run benchmark client
python -m asyncio_server.client --batches 20 --batch-size 8
```

## Architecture

```
client → gRPC (async) → WorkerServicer.ProcessBatch()
                          └─ asyncio.gather(*coroutines)
                               └─ each coroutine runs cpu_work() synchronously
                                    └─ GIL held per coroutine — no true parallelism
```

This is the intentional baseline: asyncio is excellent for I/O-bound work but
cannot parallelize CPU-bound tasks.  Compare throughput and latency numbers
with the cfuture server to see the difference.
