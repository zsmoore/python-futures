"""
cfuture gRPC server.

Uses cfuture.ThreadPoolExecutor to process each batch with true GIL-free
parallelism across sub-interpreters.  The ProcessBatch RPC fans work out to N
workers, collects results via all_of, then returns the BatchResponse.
"""
from __future__ import annotations

import dataclasses
import os
import time
from concurrent import futures

import grpc

from cfuture import Future, ThreadPoolExecutor, all_of, xi_dataclass

# Generated stubs — created by running codegen.sh
from cfuture_server.generated import worker_pb2, worker_pb2_grpc
from cfuture_server.work import cpu_work

_WORKERS: int = int(os.environ.get("CFUTURE_WORKERS", os.cpu_count() or 4))
_PORT: str = os.environ.get("CFUTURE_PORT", "50051")


# ── xi-protocol types ─────────────────────────────────────────────────────────
# Must be at module level so sub-interpreter workers can resolve them.

@xi_dataclass
@dataclasses.dataclass
class Item:
    request_id: int
    payload: str


# ── callback — runs inside a sub-interpreter worker ───────────────────────────

def handle_item(x: int, d: list) -> tuple:
    """
    Worker callback: receives (unused_int, deps=[Item]).
    Returns (request_id, result_hex, duration_ms).
    """
    import time as _time
    import hashlib as _hashlib

    item: Item = d[0]
    start = _time.perf_counter()
    data = item.payload.encode()
    for _ in range(2000):
        data = _hashlib.sha256(data).digest()
    duration_ms = (_time.perf_counter() - start) * 1000
    return (item.request_id, data.hex()[:16], duration_ms)


# ── gRPC servicer ─────────────────────────────────────────────────────────────

class WorkerServicer(worker_pb2_grpc.WorkerServiceServicer):
    """
    Accepts a long-lived ThreadPoolExecutor created at startup.
    Sub-interpreter workers are booted once and reused across all RPCs —
    no per-request startup cost.
    """

    def __init__(self, pool: ThreadPoolExecutor) -> None:
        self._pool = pool

    def Process(
        self,
        request: worker_pb2.ProcessRequest,
        context: grpc.ServicerContext,
    ) -> worker_pb2.ProcessResponse:
        start = time.perf_counter()
        result = cpu_work(request.payload)
        duration_ms = (time.perf_counter() - start) * 1000
        return worker_pb2.ProcessResponse(
            id=request.id,
            result=result,
            status=200,
            duration_ms=duration_ms,
        )

    def ProcessBatch(
        self,
        request: worker_pb2.BatchRequest,
        context: grpc.ServicerContext,
    ) -> worker_pb2.BatchResponse:
        batch_start = time.perf_counter()

        items = [
            Item(request_id=r.id, payload=r.payload)
            for r in request.requests
        ]

        futs: list[Future[tuple]] = [
            self._pool.submit(lambda: 0).then(handle_item, deps=[item])
            for item in items
        ]
        results: list[tuple] = all_of(*futs).result(timeout=30.0)

        total_ms = (time.perf_counter() - batch_start) * 1000

        responses = [
            worker_pb2.ProcessResponse(
                id=req_id,
                result=result_hex,
                status=200,
                duration_ms=dur_ms,
            )
            for req_id, result_hex, dur_ms in results
        ]

        return worker_pb2.BatchResponse(
            responses=responses,
            total_duration_ms=total_ms,
        )


# ── entrypoint ────────────────────────────────────────────────────────────────

def main() -> None:
    # Boot sub-interpreter workers once before accepting any requests.
    pool = ThreadPoolExecutor(workers=_WORKERS)
    print(f"cfuture: {_WORKERS} sub-interpreter workers ready")

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    worker_pb2_grpc.add_WorkerServiceServicer_to_server(WorkerServicer(pool), server)
    server.add_insecure_port(f"[::]:{_PORT}")
    server.start()
    print(f"cfuture gRPC server listening on port {_PORT}")

    try:
        server.wait_for_termination()
    finally:
        pool.shutdown()


if __name__ == "__main__":
    main()
