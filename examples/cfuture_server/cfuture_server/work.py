"""
CPU-bound work kernel shared between server handlers.

Kept in its own module so it can be imported cleanly inside sub-interpreter
workers (which run in isolated interpreters and re-import from scratch).

Uses a pure-Python LCG loop rather than hashlib so the GIL is held for the
entire computation.  hashlib.sha256 is a C extension that releases the GIL,
which would let asyncio coroutines run in parallel and obscure the difference
between the two concurrency models.
"""
import time


def cpu_work(payload: str, iterations: int = 250000) -> str:
    """
    Simulate CPU-bound processing with a pure-Python linear congruential
    generator.  The GIL is held throughout — no C extension calls.

    iterations=250000 ≈ 20ms of real CPU work per call on a modern laptop.
    """
    x = hash(payload) & 0xFFFFFFFF
    for _ in range(iterations):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
    return format(x, "08x")


def process_item(request_id: int, payload: str) -> tuple[int, str, float]:
    """Return (id, result_hex, duration_ms)."""
    start = time.perf_counter()
    result = cpu_work(payload)
    duration_ms = (time.perf_counter() - start) * 1000
    return request_id, result, duration_ms
