"""Tests for the Future public API: completed/failed factories, state predicates, and callbacks."""
import pytest
import cfuture


# ---------------------------------------------------------------------------
# Future.completed — factory
# ---------------------------------------------------------------------------

def test_future_completed_int():
    f = cfuture.Future.completed(42)
    assert f.result() == 42
    assert f.done()


def test_future_completed_default_none():
    f = cfuture.Future.completed()
    assert f.result() is None


def test_future_completed_string():
    f = cfuture.Future.completed("hi")
    assert f.result() == "hi"


def test_future_completed_bytes():
    f = cfuture.Future.completed(b"\xde\xad")
    assert f.result() == b"\xde\xad"


def test_future_completed_list():
    f = cfuture.Future.completed([1, 2, 3])
    assert f.result() == [1, 2, 3]


def test_future_completed_tuple():
    f = cfuture.Future.completed((1, 2))
    assert f.result() == (1, 2)


def test_future_completed_dict():
    f = cfuture.Future.completed({"a": 1})
    assert f.result() == {"a": 1}


def test_future_completed_bool_true():
    f = cfuture.Future.completed(True)
    assert f.result() is True


def test_future_completed_bool_false():
    f = cfuture.Future.completed(False)
    assert f.result() is False


def test_future_completed_float():
    f = cfuture.Future.completed(3.14)
    assert abs(f.result() - 3.14) < 1e-9


def test_future_completed_nested():
    f = cfuture.Future.completed({"x": [1, (2, 3)]})
    assert f.result() == {"x": [1, (2, 3)]}


# ---------------------------------------------------------------------------
# Future.failed — factory
# ---------------------------------------------------------------------------

def test_future_failed_raises():
    f = cfuture.Future.failed("bad thing")
    with pytest.raises(RuntimeError, match="bad thing"):
        f.result()
    assert f.done()


def test_future_failed_default_message():
    f = cfuture.Future.failed()
    with pytest.raises(RuntimeError):
        f.result()


def test_future_failed_is_done():
    f = cfuture.Future.failed("x")
    assert f.done()


# ---------------------------------------------------------------------------
# State predicates — done / cancelled / cancel
# ---------------------------------------------------------------------------

def test_future_cancel_on_completed_returns_false():
    f = cfuture.Future.completed(1)
    assert f.cancel() is False


def test_future_cancelled_false_on_completed():
    f = cfuture.Future.completed(1)
    assert not f.cancelled()


# ---------------------------------------------------------------------------
# Callbacks — then / except_ / finally_
# ---------------------------------------------------------------------------

def test_chained_then():
    with cfuture.ThreadPoolExecutor(workers=2) as pool:
        f = pool.submit(lambda: 10).then(lambda x, d: x * 2, deps=[])
        assert f.result(timeout=5.0) == 20


def test_except_handler():
    def boom():
        raise ValueError("intentional")

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(boom).except_(lambda e, d: "caught", deps=[])
        assert f.result(timeout=5.0) == "caught"


def test_finally_handler():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 7).finally_(lambda x, d: x, deps=[])
        assert f.result(timeout=5.0) == 7


def test_then_on_completed_future():
    f = cfuture.Future.completed(5).then(lambda x, d: x + 1, deps=[])
    assert f.result(timeout=5.0) == 6


def test_except_not_triggered_on_success():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 42).except_(lambda e, d: -1, deps=[])
        assert f.result(timeout=5.0) == 42


def test_finally_on_failed_future():
    def boom():
        raise ValueError("oops")

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(boom).finally_(lambda x, d: "always", deps=[])
        assert f.result(timeout=5.0) == "always"
