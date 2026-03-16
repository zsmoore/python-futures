# gRPC Stress Tester

Concurrent stress tester for the cfuture and asyncio gRPC servers.

Spins up N client threads, each firing `ProcessBatch` RPCs as fast as possible
for a fixed duration, then reports QPS and per-request latency percentiles.
Can target both servers in sequence and print a side-by-side comparison.

## Setup

```bash
cd examples/stress_test
python3 -m venv .venv && source .venv/bin/activate
pip install grpcio grpcio-tools
bash codegen.sh
```

## Usage

```bash
# Single server
python -m stress_test.runner --address localhost:50051 --label cfuture

# Side-by-side comparison (both servers must be running)
python -m stress_test.runner \
    --address localhost:50051 --label cfuture \
    --compare localhost:50052 --compare-label asyncio \
    --concurrency 8 --duration 15 --batch-size 8
```

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--address` | `localhost:50051` | Primary server address |
| `--label` | `server` | Label for primary in reports |
| `--compare` | _(none)_ | Optional second server address |
| `--compare-label` | `compare` | Label for second server |
| `--concurrency` | `8` | Concurrent client threads |
| `--duration` | `15` | Seconds to fire RPCs |
| `--batch-size` | `8` | Items per `ProcessBatch` RPC |

## Example output

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  cfuture
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  requests completed :  12,480
  errors             :      0
  wall time          :  15.42s
  QPS                :    809 req/s
  latency (per req)  :
    min  =  3.12 ms
    mean =  7.84 ms
    p50  =  7.21 ms
    p95  = 13.40 ms
    p99  = 18.92 ms
    max  = 45.11 ms
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```
