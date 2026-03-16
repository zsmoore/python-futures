"""
cfuture._wrap — thin Python wrappers over the C Future and ThreadPoolExecutor.

Provides:
  - deps=None default on then/except_/finally_ (optional deps)
  - arity adaptation: fn may declare 1, 2, or 3 positional parameters;
    the wrapper pads or trims the (result, deps, shared) args to match.
"""

import inspect
import cfuture._cfuture as _c


def _adapt(fn):
    """
    Return a 3-arg wrapper around fn that accepts however many args fn declares.

    fn may be:
      lambda x: ...           → called as fn(result)
      lambda x, d: ...        → called as fn(result, deps)
      lambda x, d, s: ...     → called as fn(result, deps, shared)  [no-op wrap]
    """
    try:
        sig = inspect.signature(fn)
        nparams = sum(
            1 for p in sig.parameters.values()
            if p.kind in (
                inspect.Parameter.POSITIONAL_ONLY,
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
            )
        )
    except (ValueError, TypeError):
        nparams = 3  # can't introspect — pass all args and let it fail naturally

    if nparams >= 3:
        return fn  # already 3-arg, no wrapping needed

    if nparams == 2:
        def _wrapper(result, deps, shared, _fn=fn):
            return _fn(result, deps)
        _wrapper.__wrapped__ = fn
        return _wrapper

    # nparams <= 1
    def _wrapper(result, deps, shared, _fn=fn):
        return _fn(result)
    _wrapper.__wrapped__ = fn
    return _wrapper


class Future:
    """Proxy for cfuture._cfuture.Future with ergonomic Python defaults."""

    __slots__ = ("_f",)

    def __init__(self, c_future):
        object.__setattr__(self, "_f", c_future)

    # ── delegation ────────────────────────────────────────────────────────────

    def __getattr__(self, name):
        return getattr(object.__getattribute__(self, "_f"), name)

    def __repr__(self):
        return repr(object.__getattribute__(self, "_f"))

    # ── chaining methods with optional deps + arity adaptation ────────────────

    def then(self, fn, deps=None):
        return Future(object.__getattribute__(self, "_f").then(_adapt(fn), deps))

    def except_(self, fn, deps=None):
        return Future(object.__getattribute__(self, "_f").except_(_adapt(fn), deps))

    def finally_(self, fn, deps=None):
        return Future(object.__getattribute__(self, "_f").finally_(_adapt(fn), deps))

    # ── result / done passthrough (avoids double-proxy on these hot paths) ────

    def result(self, timeout=None):
        f = object.__getattribute__(self, "_f")
        if timeout is None:
            return f.result()
        return f.result(timeout=timeout)

    def cancel(self):
        return object.__getattribute__(self, "_f").cancel()

    def done(self):
        return object.__getattribute__(self, "_f").done()

    def failed(self):
        return object.__getattribute__(self, "_f").failed()

    def cancelled(self):
        return object.__getattribute__(self, "_f").cancelled()

    @classmethod
    def completed(cls, value=None):
        return cls(_c.Future.completed(value))

    @classmethod
    def failed(cls, exc=None):
        if exc is None:
            return cls(_c.Future.failed())
        return cls(_c.Future.failed(exc))


class ThreadPoolExecutor:
    """Proxy for cfuture._cfuture.ThreadPoolExecutor; submit returns a wrapped Future."""

    def __init__(self, workers, shared=None):
        if shared is not None:
            self._pool = _c.ThreadPoolExecutor(workers=workers, shared=shared)
        else:
            self._pool = _c.ThreadPoolExecutor(workers=workers)

    def submit(self, fn):
        return Future(self._pool.submit(fn))

    def shutdown(self, wait=True):
        self._pool.shutdown()

    def __enter__(self):
        self._pool.__enter__()
        return self

    def __exit__(self, *args):
        return self._pool.__exit__(*args)
