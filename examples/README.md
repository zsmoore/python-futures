# Examples

## Benchmark servers

Two equivalent gRPC servers implementing the same `WorkerService` proto
(`proto/worker.proto`) with different concurrency models:

| | `cfuture_server/` | `asyncio_server/` |
|---|---|---|
| Concurrency | cfuture sub-interpreter workers | asyncio event loop |
| CPU parallelism | True (GIL-free per sub-interpreter) | No (GIL held during computation) |
| Best for | CPU-bound work | I/O-bound work |
| Default port | 50051 | 50052 |

Both expose identical RPCs (`Process`, `ProcessBatch`) and identical client
interfaces so benchmark results are directly comparable.

### Quick start

```bash
# Setup both servers (run once each)
cd cfuture_server  && python3 -m venv .venv && source .venv/bin/activate \
  && pip install grpcio grpcio-tools && pip install -e ../../ && bash codegen.sh
cd ../asyncio_server && python3 -m venv .venv && source .venv/bin/activate \
  && pip install grpcio grpcio-tools && bash codegen.sh

# Run both servers in separate terminals
python -m cfuture_server.server   # port 50051
python -m asyncio_server.server   # port 50052

# Benchmark both with the same load
python -m cfuture_server.client  --batches 20 --batch-size 8
python -m asyncio_server.client  --batches 20 --batch-size 8
```

### What to look for

- **Throughput (req/s)** — cfuture should scale with CPU count; asyncio stays flat
- **Batch latency** — cfuture batches complete in `work_time / N_workers`; asyncio batches complete in `work_time * batch_size` (sequential despite `gather`)
- **p95/p99** — tail latency under load
