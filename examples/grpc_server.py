"""
grpc_server.py — example: fan-out RPC processing with cfuture.

Simulates a gRPC server that receives a batch of requests, processes each one
in a worker sub-interpreter, then collects results and returns a response.

Key patterns demonstrated:
  - ThreadPoolExecutor with shared= config injected into every callback
  - deps= for per-request data
  - all_of to collect results from parallel futures
  - @xi_dataclass for passing structured data across interpreter boundaries
  - with pool: to guarantee shutdown even if processing raises
  - callbacks declare only the args they need (1, 2, or 3)
"""

import dataclasses
from cfuture import Future, ThreadPoolExecutor, all_of, xi_dataclass


# ── xi-protocol types ─────────────────────────────────────────────────────────
# Must be defined at module level so workers can resolve them via import.

@xi_dataclass
@dataclasses.dataclass
class Request:
    request_id: int
    payload: str


@xi_dataclass
@dataclasses.dataclass
class Response:
    request_id: int
    result: str
    status: int


# ── processing logic ──────────────────────────────────────────────────────────

def process_batch(requests: list[Request], server_config: dict) -> list[Response]:
    """Process a batch of requests in parallel worker sub-interpreters."""

    with ThreadPoolExecutor(
        workers=4,
        shared=server_config,  # injected as `s` in every callback
    ) as pool:
        futures: list[Future[Response]] = [
            pool.submit(lambda: 0).then(
                # 3-arg form: result, deps, shared — all needed here
                lambda _, d, s: Response(
                    request_id=d[0].request_id,
                    result=f"processed:{d[0].payload}@{s['version']}",
                    status=200,
                ),
                deps=[req],
            )
            for req in requests
        ]

        return all_of(*futures).result(timeout=30.0)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    server_config = {"version": "v1.2", "env": "production", "timeout_ms": 500}

    requests = [
        Request(request_id=i, payload=f"data-{i}")
        for i in range(8)
    ]

    responses = process_batch(requests, server_config)

    for resp in responses:
        print(f"[{resp.request_id}] status={resp.status}  result={resp.result}")


if __name__ == "__main__":
    main()
