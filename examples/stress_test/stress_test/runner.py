"""
Stress test runner.

Spins up N concurrent worker threads, each hammering the target server for
a fixed duration, then prints a latency + QPS report.

Can target one server or both side-by-side for comparison.

Usage:
    # Single target
    python -m stress_test.runner --address localhost:50051 --concurrency 8 --duration 15

    # Compare cfuture vs asyncio side by side
    python -m stress_test.runner \\
        --address localhost:50051 --label cfuture \\
        --compare localhost:50052 --compare-label asyncio \\
        --concurrency 8 --duration 15 --batch-size 8
"""
from __future__ import annotations

import argparse
import threading
import time
from dataclasses import dataclass

from stress_test.metrics import Metrics
from stress_test.worker import run_worker


@dataclass
class RunResult:
    label: str
    metrics: Metrics
    wall_s: float


def run_stress(
    address: str,
    concurrency: int,
    duration_s: float,
    batch_size: int,
    label: str,
) -> RunResult:
    metrics = Metrics()
    barrier = threading.Barrier(concurrency)
    threads: list[threading.Thread] = [
        threading.Thread(
            target=run_worker,
            args=(i, address, duration_s, batch_size, metrics, barrier),
            daemon=True,
        )
        for i in range(concurrency)
    ]

    print(f"\n[{label}] starting {concurrency} workers → {address}  "
          f"(batch={batch_size}, duration={duration_s}s)")

    wall_start = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall_s = time.perf_counter() - wall_start

    return RunResult(label=label, metrics=metrics, wall_s=wall_s)


def _report(result: RunResult) -> None:
    m = result.metrics
    print(f"\n{'━' * 54}")
    print(f"  {result.label}")
    print(f"{'━' * 54}")
    print(f"  requests completed : {m.count:,}")
    print(f"  errors             : {m.errors:,}")
    print(f"  wall time          : {result.wall_s:.2f}s")
    print(f"  QPS                : {m.qps(result.wall_s):,.0f} req/s")
    print("  latency (per req)  :")
    print(f"    min  = {m.min_ms:.2f} ms")
    print(f"    mean = {m.mean_ms:.2f} ms")
    print(f"    p50  = {m.percentile(50):.2f} ms")
    print(f"    p75  = {m.percentile(75):.2f} ms")
    print(f"    p95  = {m.percentile(95):.2f} ms")
    print(f"    p99  = {m.percentile(99):.2f} ms")
    print(f"    max  = {m.max_ms:.2f} ms")
    print(f"{'━' * 54}")


def _compare(a: RunResult, b: RunResult) -> None:
    print(f"\n{'━' * 54}")
    print(f"  Comparison: {a.label} vs {b.label}")
    print(f"{'━' * 54}")
    print(f"  {'metric':<18}  {a.label:>14}  {b.label:>14}")
    print(f"  {'─' * 50}")

    def row(name: str, av: float, bv: float, fmt: str = ".1f", suffix: str = "") -> None:
        ratio = f"  ({av/bv:.2f}x)" if bv > 0 else ""
        print(f"  {name:<18}  {av:>13{fmt}}{suffix}  {bv:>13{fmt}}{suffix}{ratio}")

    row("QPS", a.metrics.qps(a.wall_s), b.metrics.qps(b.wall_s), ".0f", " req/s")
    row("mean latency", a.metrics.mean_ms, b.metrics.mean_ms, ".2f", " ms")
    row("p50", a.metrics.percentile(50), b.metrics.percentile(50), ".2f", " ms")
    row("p95", a.metrics.percentile(95), b.metrics.percentile(95), ".2f", " ms")
    row("p99", a.metrics.percentile(99), b.metrics.percentile(99), ".2f", " ms")
    row("errors", float(a.metrics.errors), float(b.metrics.errors), ".0f")
    print(f"{'━' * 54}")


def main() -> None:
    parser = argparse.ArgumentParser(description="gRPC stress tester")
    parser.add_argument("--address", default="localhost:50051",
                        help="target server address (default: localhost:50051)")
    parser.add_argument("--label", default="server",
                        help="label for the primary target in reports")
    parser.add_argument("--compare", default=None, metavar="ADDRESS",
                        help="optional second server to compare against")
    parser.add_argument("--compare-label", default="compare",
                        help="label for the comparison target")
    parser.add_argument("--concurrency", type=int, default=8,
                        help="number of concurrent client threads (default: 8)")
    parser.add_argument("--duration", type=float, default=15.0,
                        help="how long each run fires RPCs, in seconds (default: 15)")
    parser.add_argument("--batch-size", type=int, default=8,
                        help="items per ProcessBatch RPC (default: 8)")
    args = parser.parse_args()

    primary = run_stress(
        address=args.address,
        concurrency=args.concurrency,
        duration_s=args.duration,
        batch_size=args.batch_size,
        label=args.label,
    )
    _report(primary)

    if args.compare:
        secondary = run_stress(
            address=args.compare,
            concurrency=args.concurrency,
            duration_s=args.duration,
            batch_size=args.batch_size,
            label=args.compare_label,
        )
        _report(secondary)
        _compare(primary, secondary)


if __name__ == "__main__":
    main()
