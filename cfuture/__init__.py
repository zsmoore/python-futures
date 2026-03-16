"""cfuture — CompletableFuture-style futures with true GIL-free parallelism.

Requires Python 3.12+.

Public API:
  ThreadPoolExecutor  — fixed thread pool backed by sub-interpreters
  Future              — lazy, chainable future value
  all_of(*futures)    — non-blocking combinator: resolves when all inputs resolve
  pickled(obj)        — explicit opt-in for pickle-based cross-interpreter transfer
"""

from cfuture._cfuture import (  # noqa: F401
    Future,
    ThreadPoolExecutor,
    all_of,
    pickled,
)

__all__ = [
    "Future",
    "ThreadPoolExecutor",
    "all_of",
    "pickled",
]
