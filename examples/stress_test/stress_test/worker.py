"""
Stress worker: a single thread that fires RPCs as fast as possible for a
fixed duration, recording per-request latency into a shared Metrics object.

Supports both Process (single) and ProcessBatch RPC modes.
"""
from __future__ import annotations

import time
import threading

import grpc

from stress_test.metrics import Metrics
from stress_test.generated import worker_pb2, worker_pb2_grpc


def _make_batch(worker_id: int, seq: int, batch_size: int) -> worker_pb2.BatchRequest:
    return worker_pb2.BatchRequest(
        requests=[
            worker_pb2.ProcessRequest(
                id=worker_id * 10000 + seq * batch_size + j,
                payload=f"w{worker_id}-s{seq}-{j}",
            )
            for j in range(batch_size)
        ]
    )


def run_worker(
    worker_id: int,
    address: str,
    duration_s: float,
    batch_size: int,
    metrics: Metrics,
    ready: threading.Barrier,
    rpc_timeout: float = 30.0,
) -> None:
    """
    Connect, wait at the barrier for all workers to be ready, then hammer
    the server with ProcessBatch RPCs until `duration_s` has elapsed.
    """
    channel = grpc.insecure_channel(address)
    stub = worker_pb2_grpc.WorkerServiceStub(channel)

    # Warm the channel before the timed window starts.
    try:
        stub.Process(worker_pb2.ProcessRequest(id=-1, payload="warmup"), timeout=10.0)
    except Exception:
        pass

    ready.wait()  # all workers start together
    deadline = time.perf_counter() + duration_s
    seq = 0

    while time.perf_counter() < deadline:
        batch = _make_batch(worker_id, seq, batch_size)
        t0 = time.perf_counter()
        try:
            stub.ProcessBatch(batch, timeout=rpc_timeout)
            latency_ms = (time.perf_counter() - t0) * 1000
            # Record one latency entry per *item* so QPS counts individual requests.
            per_item_ms = latency_ms / batch_size
            for _ in range(batch_size):
                metrics.record(per_item_ms)
        except Exception:
            metrics.record_error()
        seq += 1

    channel.close()
