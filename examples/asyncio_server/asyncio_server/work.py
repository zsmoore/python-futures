"""
CPU-bound work kernel — identical to cfuture_server/work.py.

Both servers use the same work so benchmarks are comparing the concurrency
model, not the computation.
"""
import hashlib
import time


def cpu_work(payload: str, iterations: int = 2000) -> str:
    """
    Simulate CPU-bound processing: hash the payload repeatedly.

    iterations=2000 gives ~1-3 ms of real CPU work per call — enough to
    demonstrate parallelism gains without making benchmarks unbearably slow.
    """
    data = payload.encode()
    for _ in range(iterations):
        data = hashlib.sha256(data).digest()
    return data.hex()[:16]


def process_item(request_id: int, payload: str) -> tuple[int, str, float]:
    """Return (id, result_hex, duration_ms)."""
    start = time.perf_counter()
    result = cpu_work(payload)
    duration_ms = (time.perf_counter() - start) * 1000
    return request_id, result, duration_ms
