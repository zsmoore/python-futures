"""
Benchmark client for the asyncio gRPC server.

Identical interface to cfuture_server/client.py so results are directly
comparable — same flags, same output format.

Usage:
    python -m asyncio_server.client [--host HOST] [--port PORT]
        [--batches N] [--batch-size N]
"""
from __future__ import annotations

import argparse
import statistics
import time

import grpc

from asyncio_server.generated import worker_pb2, worker_pb2_grpc


def run_benchmark(
    host: str,
    port: int,
    num_batches: int,
    batch_size: int,
) -> None:
    address = f"{host}:{port}"
    channel = grpc.insecure_channel(address)
    stub = worker_pb2_grpc.WorkerServiceStub(channel)

    print(f"Benchmarking asyncio server at {address}")
    print(f"  {num_batches} batches × {batch_size} requests = "
          f"{num_batches * batch_size} total requests\n")

    latencies: list[float] = []
    wall_start = time.perf_counter()

    for i in range(num_batches):
        batch = worker_pb2.BatchRequest(
            requests=[
                worker_pb2.ProcessRequest(id=i * batch_size + j, payload=f"payload-{i}-{j}")
                for j in range(batch_size)
            ]
        )
        t0 = time.perf_counter()
        response = stub.ProcessBatch(batch, timeout=30.0)
        latencies.append((time.perf_counter() - t0) * 1000)

        if (i + 1) % max(1, num_batches // 10) == 0:
            print(f"  batch {i + 1}/{num_batches} — "
                  f"server reported {response.total_duration_ms:.1f} ms")

    wall_ms = (time.perf_counter() - wall_start) * 1000
    total_requests = num_batches * batch_size
    throughput = total_requests / (wall_ms / 1000)

    print(f"\n{'─' * 50}")
    print(f"  total wall time : {wall_ms:.1f} ms")
    print(f"  throughput      : {throughput:.0f} req/s")
    print(f"  batch latency   : mean={statistics.mean(latencies):.1f} ms  "
          f"p50={statistics.median(latencies):.1f} ms  "
          f"p95={_percentile(latencies, 95):.1f} ms  "
          f"p99={_percentile(latencies, 99):.1f} ms")
    print(f"{'─' * 50}")

    channel.close()


def _percentile(data: list[float], p: int) -> float:
    sorted_data = sorted(data)
    idx = int(len(sorted_data) * p / 100)
    return sorted_data[min(idx, len(sorted_data) - 1)]


def main() -> None:
    parser = argparse.ArgumentParser(description="asyncio gRPC benchmark client")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=50052)
    parser.add_argument("--batches", type=int, default=20,
                        help="number of BatchRequest RPCs to send")
    parser.add_argument("--batch-size", type=int, default=8,
                        help="requests per batch")
    args = parser.parse_args()

    run_benchmark(args.host, args.port, args.batches, args.batch_size)


if __name__ == "__main__":
    main()
