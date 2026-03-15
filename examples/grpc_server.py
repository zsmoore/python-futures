"""
cfuture gRPC server demo.

Shows how cfuture integrates with a gRPC server to achieve true parallelism
without the GIL, while keeping the main thread non-blocking.

To run (requires grpcio):
  pip install grpcio grpcio-tools
  python examples/grpc_server.py
"""

import time
import cfuture

# In a real server you would import generated pb2 and pb2_grpc modules.
# This demo simulates the pattern.

pool = cfuture.ThreadPoolExecutor(
    workers=8,
    shared={
        "config": {"timeout": 5.0, "retries": 3},
    },
)


def simulate_api_call(name: str, delay: float = 0.1) -> dict:
    """Simulate a downstream API call."""
    time.sleep(delay)
    return {"service": name, "status": "ok", "ts": time.time()}


def handle_request(request_id: str) -> cfuture.Future:
    """
    Handle a single gRPC request using cfuture pipeline.

    The main thread builds the dependency graph and returns immediately.
    No worker threads are blocked waiting — they fire as dependencies complete.
    """
    config = {"request_id": request_id, "scale": 1.0}

    # Fan out to 3 downstream services in parallel
    api_future_1 = pool.submit(lambda: simulate_api_call("auth", 0.05))
    api_future_2 = pool.submit(lambda: simulate_api_call("data", 0.08))
    api_future_3 = pool.submit(lambda: simulate_api_call("cache", 0.03))

    # Wait for all three, then compose
    pipeline = (
        cfuture.all_of(api_future_1, api_future_2, api_future_3)
        .then(
            lambda results, d: {
                "request_id": d[0]["request_id"],
                "services": [r["service"] for r in results],
                "all_ok": all(r["status"] == "ok" for r in results),
            },
            deps=[config],
        )
        .except_(
            lambda err, d: {"request_id": d[0]["request_id"], "error": str(err)},
            deps=[config],
        )
    )

    return pipeline


def demo():
    print("cfuture gRPC server demo")
    print("=" * 40)

    start = time.time()

    # Simulate 5 concurrent requests
    pipelines = [handle_request(f"req-{i}") for i in range(5)]

    print(f"All {len(pipelines)} pipelines built in {(time.time()-start)*1000:.1f}ms "
          f"(main thread never blocked)")
    print("Waiting for results...")

    for i, pipeline in enumerate(pipelines):
        result = pipeline.result(timeout=10.0)
        print(f"  Request {i}: {result}")

    elapsed = time.time() - start
    print(f"\nTotal time: {elapsed:.3f}s (5 requests × ~0.08s = {elapsed:.2f}s parallelised)")

    pool.shutdown()


if __name__ == "__main__":
    demo()
