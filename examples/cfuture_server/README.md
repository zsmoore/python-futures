# cfuture gRPC Server

gRPC server that handles requests using **cfuture** sub-interpreter parallelism.
Each RPC handler fans work out to a `ThreadPoolExecutor`, letting CPU-bound
processing run in true parallel without the GIL.

## Setup

```bash
# From this directory
python3 -m venv .venv
source .venv/bin/activate

pip install grpcio grpcio-tools
pip install -e ../../          # install cfuture from the repo root

# Generate gRPC stubs from the shared proto
python -m grpc_tools.protoc \
    -I../proto \
    --python_out=cfuture_server/generated \
    --grpc_python_out=cfuture_server/generated \
    ../proto/worker.proto
```

## Run

```bash
# Terminal 1 — start server
python -m cfuture_server.server

# Terminal 2 — run benchmark client
python -m cfuture_server.client --requests 100 --batch-size 10
```

## Architecture

```
client → gRPC → WorkerServicer.ProcessBatch()
                  └─ cfuture.ThreadPoolExecutor(workers=N)
                       └─ sub-interpreter per worker (true GIL-free parallelism)
                            └─ CPU work runs in parallel
```
