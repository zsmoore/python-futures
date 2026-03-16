"""
Demonstrates fully-typed callback patterns.

Lambda parameters cannot carry inline type annotations (Python syntax
restriction), so fully-typed callbacks use named functions with explicit
signatures.  This file shows both styles side by side so it's clear when
each is appropriate.
"""
from typing import Any
import cfuture
from cfuture import Future, ThreadPoolExecutor


# ---------------------------------------------------------------------------
# Named callbacks with full type signatures
# ---------------------------------------------------------------------------

def double(x: int) -> int:
    return x * 2


def add_offset(x: int, d: list[int]) -> int:
    return x + d[0]


def with_shared(x: int, d: list[int], s: dict[str, Any]) -> str:
    return f"{x + d[0]}@{s['env']}"


def recover(e: BaseException) -> int:
    return -1


def recover_with_deps(e: BaseException, d: list[int]) -> int:
    return d[0]


def always_cleanup(x: Any) -> str:
    return "done"


# ---------------------------------------------------------------------------
# Tests using named callbacks (fully typed)
# ---------------------------------------------------------------------------

def test_named_then_one_arg():
    with ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 5).then(double)
        assert f.result(timeout=5.0) == 10


def test_named_then_two_args():
    with ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 5).then(add_offset, deps=[10])
        assert f.result(timeout=5.0) == 15


def test_named_then_three_args():
    with ThreadPoolExecutor(workers=1, shared={"env": "prod"}) as pool:
        f: Future[str] = pool.submit(lambda: 3).then(with_shared, deps=[7])
        assert f.result(timeout=5.0) == "10@prod"


def test_named_except_one_arg():
    def boom() -> int:
        raise RuntimeError("fail")

    with ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(boom).except_(recover)
        assert f.result(timeout=5.0) == -1


def test_named_except_two_args():
    def boom() -> int:
        raise RuntimeError("fail")

    with ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(boom).except_(recover_with_deps, deps=[42])
        assert f.result(timeout=5.0) == 42


def test_named_finally():
    with ThreadPoolExecutor(workers=1) as pool:
        f: Future[str] = pool.submit(lambda: 99).finally_(always_cleanup)
        assert f.result(timeout=5.0) == "done"


def test_named_and_lambda_mixed_chain():
    """Real-world pattern: named functions where types matter, lambdas for trivial steps."""
    with ThreadPoolExecutor(workers=1, shared={"env": "staging"}) as pool:
        f: Future[str] = (
            pool.submit(lambda: 3)
            .then(double)                              # named: typed
            .then(add_offset, deps=[4])               # named: typed
            .then(with_shared, deps=[0], )            # named: typed, uses shared
        )
        assert f.result(timeout=5.0) == "10@staging"  # (3*2)+4=10, d[0]=0 → "10@staging"
