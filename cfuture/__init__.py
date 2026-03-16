"""cfuture — CompletableFuture-style futures with true GIL-free parallelism.

Requires Python 3.12+.

Public API:
  ThreadPoolExecutor  — fixed thread pool backed by sub-interpreters
  Future              — lazy, chainable future value
  all_of(*futures)    — non-blocking combinator: resolves when all inputs resolve
  pickled(obj)        — explicit opt-in for pickle-based cross-interpreter transfer
  xi_dataclass        — decorator: auto-implement __xi_encode__/__xi_decode__
"""

from cfuture._wrap import Future, ThreadPoolExecutor  # noqa: F401
from cfuture._cfuture import pickled                  # noqa: F401
from cfuture._xi import xi_dataclass                  # noqa: F401
import cfuture._cfuture as _c


def all_of(*futures):
    """Wait for all futures (non-blocking)."""
    unwrapped = tuple(
        f._f if isinstance(f, Future) else f
        for f in futures
    )
    return Future(_c.all_of(*unwrapped))

__all__ = [
    "Future",
    "ThreadPoolExecutor",
    "all_of",
    "pickled",
    "xi_dataclass",
]
