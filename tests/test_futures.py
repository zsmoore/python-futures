"""MVP functional tests for Future basics."""
import time
import pytest
import cfuture


def test_submit_and_result():
    pool = cfuture.ThreadPoolExecutor(workers=2)
    f = pool.submit(lambda: 42)
    assert f.result(timeout=5.0) == 42
    pool.shutdown()


def test_future_done():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 1)
    f.result(timeout=5.0)
    assert f.done()
    pool.shutdown()


def test_future_timeout():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: time.sleep(0.5))
    with pytest.raises(TimeoutError):
        f.result(timeout=0.1)
    pool.shutdown()


def test_future_cancel():
    # Submit many tasks to fill the queue, then cancel one
    pool = cfuture.ThreadPoolExecutor(workers=1)
    # Fill worker
    blocker = pool.submit(lambda: time.sleep(0.5))
    # This should be queued (not started)
    f = pool.submit(lambda: 99)
    cancelled = f.cancel()
    # May or may not cancel depending on timing, but should not raise
    if cancelled:
        with pytest.raises(RuntimeError):
            f.result(timeout=1.0)
    pool.shutdown()


def test_future_completed():
    f = cfuture.Future.completed(100)
    assert f.result() == 100
    assert f.done()


def test_future_failed():
    f = cfuture.Future.failed("boom")
    with pytest.raises(RuntimeError, match="boom"):
        f.result()


def test_chained_then():
    pool = cfuture.ThreadPoolExecutor(workers=2)
    f = pool.submit(lambda: 10).then(lambda x, d: x * 2, deps=[])
    assert f.result(timeout=5.0) == 20
    pool.shutdown()


def test_except_handler():
    pool = cfuture.ThreadPoolExecutor(workers=1)

    def boom():
        raise ValueError("intentional")

    f = pool.submit(boom).except_(lambda e, d: "caught", deps=[])
    result = f.result(timeout=5.0)
    assert result == "caught"
    pool.shutdown()


def test_finally_handler():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 7).finally_(lambda x, d: x, deps=[])
    assert f.result(timeout=5.0) == 7
    pool.shutdown()


def test_parallel_workers():
    """All workers run in parallel — measure total time."""
    pool = cfuture.ThreadPoolExecutor(workers=4)
    start = time.time()
    futures = [pool.submit(lambda: time.sleep(0.3)) for _ in range(4)]
    for ftr in futures:
        ftr.result(timeout=5.0)
    elapsed = time.time() - start
    # 4 parallel sleeps of 0.3s should finish well under 1.0s
    assert elapsed < 1.0, f"Expected parallel execution, got {elapsed:.2f}s"
    pool.shutdown()


def test_context_manager():
    with cfuture.ThreadPoolExecutor(workers=2) as pool:
        f = pool.submit(lambda: "hello")
        assert f.result(timeout=5.0) == "hello"
