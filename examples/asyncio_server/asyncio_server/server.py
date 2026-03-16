"""
asyncio gRPC server.

Uses grpc.aio and asyncio.gather to handle batch requests concurrently.
CPU-bound work runs synchronously inside each coroutine — the GIL is held
during computation so tasks interleave rather than run in true parallel.

This is the baseline for profiling against the cfuture sub-interpreter server.
"""
from __future__ import annotations

import asyncio
import os
import time

import grpc
import grpc.aio

from asyncio_server.generated import worker_pb2, worker_pb2_grpc
from asyncio_server.work import cpu_work

_PORT: str = os.environ.get("ASYNCIO_PORT", "50052")


# ── gRPC servicer ─────────────────────────────────────────────────────────────

class WorkerServicer(worker_pb2_grpc.WorkerServiceServicer):

    async def Process(
        self,
        request: worker_pb2.ProcessRequest,
        context: grpc.aio.ServicerContext,
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

    async def ProcessBatch(
        self,
        request: worker_pb2.BatchRequest,
        context: grpc.aio.ServicerContext,
    ) -> worker_pb2.BatchResponse:
        batch_start = time.perf_counter()

        async def handle_one(req: worker_pb2.ProcessRequest) -> worker_pb2.ProcessResponse:
            start = time.perf_counter()
            result = cpu_work(req.payload)
            duration_ms = (time.perf_counter() - start) * 1000
            return worker_pb2.ProcessResponse(
                id=req.id,
                result=result,
                status=200,
                duration_ms=duration_ms,
            )

        responses = await asyncio.gather(*[handle_one(r) for r in request.requests])
        total_ms = (time.perf_counter() - batch_start) * 1000

        return worker_pb2.BatchResponse(
            responses=list(responses),
            total_duration_ms=total_ms,
        )


# ── entrypoint ────────────────────────────────────────────────────────────────

async def _serve() -> None:
    server = grpc.aio.server()
    worker_pb2_grpc.add_WorkerServiceServicer_to_server(WorkerServicer(), server)
    server.add_insecure_port(f"[::]:{_PORT}")
    await server.start()
    print(f"asyncio gRPC server listening on port {_PORT}")
    await server.wait_for_termination()


def main() -> None:
    asyncio.run(_serve())


if __name__ == "__main__":
    main()
