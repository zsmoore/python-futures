"""Tests for PR 1: core thread pool, Future basics, and SharedValue encoding."""
import time
import pytest
import cfuture


# ---------------------------------------------------------------------------
# ThreadPoolExecutor — lifecycle (no worker invocation)
# ---------------------------------------------------------------------------

def test_pool_creates_and_shuts_down():
    pool = cfuture.ThreadPoolExecutor(workers=2)
    pool.shutdown()


def test_pool_context_manager_enter_exit():
    # __enter__ / __exit__ without submitting tasks
    with cfuture.ThreadPoolExecutor(workers=2):
        pass


# ---------------------------------------------------------------------------
# Future.completed / Future.failed — no worker needed
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
    result = f.result()
    assert abs(result - 3.14) < 1e-9


def test_future_completed_nested():
    f = cfuture.Future.completed({"x": [1, (2, 3)]})
    assert f.result() == {"x": [1, (2, 3)]}


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


def test_future_cancel_on_completed_returns_false():
    f = cfuture.Future.completed(1)
    assert f.cancel() is False


def test_future_cancelled_false_on_completed():
    f = cfuture.Future.completed(1)
    assert not f.cancelled()


# ---------------------------------------------------------------------------
# SharedValue encoding — pickled() wrapper
# ---------------------------------------------------------------------------

def test_pickled_has_sentinel_attribute():
    p = cfuture.pickled(42)
    assert p.__cfuture_pickled__ is True


def test_pickled_exposes_wrapped_value():
    obj = object()
    p = cfuture.pickled(obj)
    assert p.value is obj


def test_pickled_wraps_none():
    p = cfuture.pickled(None)
    assert p.value is None


# ---------------------------------------------------------------------------
# Worker-based tests
# ---------------------------------------------------------------------------

def test_submit_and_result_int():
    with cfuture.ThreadPoolExecutor(workers=2) as pool:
        assert pool.submit(lambda: 42).result(timeout=5.0) == 42


def test_submit_and_result_none():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        assert pool.submit(lambda: None).result(timeout=5.0) is None


def test_submit_and_result_string():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        assert pool.submit(lambda: "hello").result(timeout=5.0) == "hello"


def test_submit_and_result_bytes():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        assert pool.submit(lambda: b"\x01\x02").result(timeout=5.0) == b"\x01\x02"


def test_submit_and_result_float():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        result = pool.submit(lambda: 3.14).result(timeout=5.0)
        assert abs(result - 3.14) < 1e-9


def test_submit_and_result_list():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        assert pool.submit(lambda: [1, 2, 3]).result(timeout=5.0) == [1, 2, 3]


def test_submit_and_result_dict():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        assert pool.submit(lambda: {"a": 1}).result(timeout=5.0) == {"a": 1}


def test_submit_raises_on_exception():
    def boom():
        raise ValueError("oops")

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(boom)
        with pytest.raises(RuntimeError, match="oops"):
            f.result(timeout=5.0)


def test_result_timeout_raises():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: time.sleep(10))
        with pytest.raises(TimeoutError):
            f.result(timeout=0.05)
        pool.shutdown(wait=False)


def test_future_done_after_result():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 1)
        f.result(timeout=5.0)
        assert f.done()


def test_future_cancel_queued():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        blocker = pool.submit(lambda: time.sleep(0.3))
        queued = pool.submit(lambda: 99)
        cancelled = queued.cancel()
        if cancelled:
            assert queued.cancelled()
            assert queued.done()
            with pytest.raises(RuntimeError):
                queued.result(timeout=1.0)
        blocker.result(timeout=5.0)


def test_many_tasks_correct_results():
    with cfuture.ThreadPoolExecutor(workers=4) as pool:
        futures = [pool.submit(lambda: 1) for _ in range(20)]
        assert all(f.result(timeout=5.0) == 1 for f in futures)


def test_pool_min_workers_clamp():
    pool = cfuture.ThreadPoolExecutor(workers=0)
    f = pool.submit(lambda: 7)
    assert f.result(timeout=5.0) == 7
    pool.shutdown()


def test_non_transferable_result_raises():
    # The lambda captures Opaque as a free variable, so validate_fn rejects
    # it at submit time with ValueError (before the worker runs).
    class Opaque:
        pass

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        with pytest.raises((ValueError, RuntimeError)):
            f = pool.submit(lambda: Opaque())
            f.result(timeout=5.0)
