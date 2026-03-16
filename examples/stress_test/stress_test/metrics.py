"""
Thread-safe metrics collector.

Each worker thread records individual RPC latencies here.
The runner reads results after all workers finish.
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field


@dataclass
class Metrics:
    latencies_ms: list[float] = field(default_factory=list)
    errors: int = 0
    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)

    def record(self, latency_ms: float) -> None:
        with self._lock:
            self.latencies_ms.append(latency_ms)

    def record_error(self) -> None:
        with self._lock:
            self.errors += 1

    # ── derived stats ──────────────────────────────────────────────────────────

    @property
    def count(self) -> int:
        return len(self.latencies_ms)

    @property
    def mean_ms(self) -> float:
        if not self.latencies_ms:
            return 0.0
        return sum(self.latencies_ms) / len(self.latencies_ms)

    @property
    def min_ms(self) -> float:
        return min(self.latencies_ms) if self.latencies_ms else 0.0

    @property
    def max_ms(self) -> float:
        return max(self.latencies_ms) if self.latencies_ms else 0.0

    def percentile(self, p: int) -> float:
        if not self.latencies_ms:
            return 0.0
        s = sorted(self.latencies_ms)
        idx = int(len(s) * p / 100)
        return s[min(idx, len(s) - 1)]

    def qps(self, wall_seconds: float) -> float:
        if wall_seconds <= 0:
            return 0.0
        return self.count / wall_seconds
