"""
cfuture._wrap — thin Python wrappers over the C Future and ThreadPoolExecutor.

Provides:
  - Generic Future[T] with typed then/except_/finally_ chains
  - deps= typed via TypeVarTuple so each element's type is tracked
  - deps=None default (optional deps)
  - arity adaptation: fn may declare 1, 2, or 3 positional parameters
"""

from __future__ import annotations

import inspect
from typing import (
    Any,
    Callable,
    Generic,
    TypeVar,
    TypeVarTuple,
    Unpack,
    overload,
)

import cfuture._cfuture as _c

T = TypeVar("T")
U = TypeVar("U")
Ts = TypeVarTuple("Ts")


def _adapt(fn: Callable[..., U]) -> Callable[[Any, Any, Any], U]:
    """
    Return a 3-arg wrapper around fn that accepts however many args fn declares.

    fn may be:
      lambda x: ...           → called as fn(result)
      lambda x, d: ...        → called as fn(result, deps)
      lambda x, d, s: ...     → called as fn(result, deps, shared)  [no-op]
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
        def _wrapper2(result: Any, deps: Any, shared: Any, _fn: Callable[..., U] = fn) -> U:
            return _fn(result, deps)
        _wrapper2.__wrapped__ = fn  # type: ignore[attr-defined]
        return _wrapper2

    # nparams <= 1
    def _wrapper1(result: Any, deps: Any, shared: Any, _fn: Callable[..., U] = fn) -> U:
        return _fn(result)
    _wrapper1.__wrapped__ = fn  # type: ignore[attr-defined]
    return _wrapper1


class Future(Generic[T]):
    """
    A chainable future value. Wraps cfuture._cfuture.Future with:
      - Generic type parameter T (the resolved value type)
      - deps=None default on then/except_/finally_
      - arity adaptation on callbacks
    """

    __slots__ = ("_f",)

    def __init__(self, c_future: Any) -> None:
        object.__setattr__(self, "_f", c_future)

    # ── delegation ────────────────────────────────────────────────────────────

    def __getattr__(self, name: str) -> Any:
        return getattr(object.__getattribute__(self, "_f"), name)

    def __repr__(self) -> str:
        return repr(object.__getattribute__(self, "_f"))

    # ── result ────────────────────────────────────────────────────────────────

    def result(self, timeout: float | None = None) -> T:
        f = object.__getattribute__(self, "_f")
        if timeout is None:
            return f.result()  # type: ignore[no-any-return]
        return f.result(timeout=timeout)  # type: ignore[no-any-return]

    # ── chaining: then ────────────────────────────────────────────────────────

    @overload
    def then(self, fn: Callable[[T], U], deps: None = ...) -> Future[U]: ...

    @overload
    def then(self, fn: Callable[[T, list[Any]], U], deps: list[Any] = ...) -> Future[U]: ...

    @overload
    def then(
        self,
        fn: Callable[[T, tuple[Unpack[Ts]]], U],
        deps: tuple[Unpack[Ts]] = ...,
    ) -> Future[U]: ...

    @overload
    def then(
        self,
        fn: Callable[[T, list[Any], dict[str, Any]], U],
        deps: list[Any] | None = ...,
    ) -> Future[U]: ...

    def then(self, fn: Callable[..., U], deps: Any = None) -> Future[U]:
        """Chain a callback on success. fn may take 1, 2, or 3 positional args."""
        return Future(object.__getattribute__(self, "_f").then(_adapt(fn), deps))

    # ── chaining: except_ ─────────────────────────────────────────────────────

    @overload
    def except_(self, fn: Callable[[BaseException], U], deps: None = ...) -> Future[U]: ...

    @overload
    def except_(
        self,
        fn: Callable[[BaseException, list[Any]], U],
        deps: list[Any] = ...,
    ) -> Future[U]: ...

    @overload
    def except_(
        self,
        fn: Callable[[BaseException, list[Any], dict[str, Any]], U],
        deps: list[Any] | None = ...,
    ) -> Future[U]: ...

    def except_(self, fn: Callable[..., U], deps: Any = None) -> Future[U]:
        """Chain a callback on failure. fn may take 1, 2, or 3 positional args."""
        return Future(object.__getattribute__(self, "_f").except_(_adapt(fn), deps))

    # ── chaining: finally_ ────────────────────────────────────────────────────

    @overload
    def finally_(self, fn: Callable[[T], U], deps: None = ...) -> Future[U]: ...

    @overload
    def finally_(
        self,
        fn: Callable[[T, list[Any]], U],
        deps: list[Any] = ...,
    ) -> Future[U]: ...

    @overload
    def finally_(
        self,
        fn: Callable[[T, list[Any], dict[str, Any]], U],
        deps: list[Any] | None = ...,
    ) -> Future[U]: ...

    def finally_(self, fn: Callable[..., U], deps: Any = None) -> Future[U]:
        """Chain a callback unconditionally. fn may take 1, 2, or 3 positional args."""
        return Future(object.__getattribute__(self, "_f").finally_(_adapt(fn), deps))

    # ── state ─────────────────────────────────────────────────────────────────

    def cancel(self) -> bool:
        return object.__getattribute__(self, "_f").cancel()  # type: ignore[no-any-return]

    def done(self) -> bool:
        return object.__getattribute__(self, "_f").done()  # type: ignore[no-any-return]

    def cancelled(self) -> bool:
        return object.__getattribute__(self, "_f").cancelled()  # type: ignore[no-any-return]

    # ── constructors ──────────────────────────────────────────────────────────

    @classmethod
    def completed(cls, value: T = None) -> Future[T]:  # type: ignore[assignment]
        """Return a pre-resolved Future."""
        return cls(_c.Future.completed(value))

    @classmethod
    def failed(cls, exc: BaseException | None = None) -> Future[Any]:
        """Return a pre-failed Future."""
        if exc is None:
            return cls(_c.Future.failed())
        return cls(_c.Future.failed(exc))


class ThreadPoolExecutor:
    """
    Fixed-size thread pool backed by sub-interpreters.

    Args:
        workers: number of worker sub-interpreters
        shared:  optional dict encoded once and injected into every callback
                 as the third ``shared`` argument
    """

    def __init__(self, workers: int, shared: dict[str, Any] | None = None) -> None:
        if shared is not None:
            self._pool = _c.ThreadPoolExecutor(workers=workers, shared=shared)
        else:
            self._pool = _c.ThreadPoolExecutor(workers=workers)

    def submit(self, fn: Callable[[], T]) -> Future[T]:
        """Submit a zero-argument callable; returns a Future[T]."""
        return Future(self._pool.submit(fn))

    def shutdown(self, wait: bool = True) -> None:
        self._pool.shutdown()

    def __enter__(self) -> ThreadPoolExecutor:
        self._pool.__enter__()
        return self

    def __exit__(self, *args: Any) -> Any:
        return self._pool.__exit__(*args)
