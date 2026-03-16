"""Non-blocking all_of combinator tests."""
import time
import pytest
import cfuture


def test_all_of_basic():
    pool = cfuture.ThreadPoolExecutor(workers=4)
    futures = [pool.submit(lambda: 1), pool.submit(lambda: 2), pool.submit(lambda: 3)]
    combined = cfuture.all_of(*futures)
    results = combined.result(timeout=5.0)
    assert isinstance(results, list)
    assert len(results) == 3
    pool.shutdown()


def test_all_of_empty():
    f = cfuture.all_of()
    result = f.result(timeout=1.0)
    assert result == []


def test_all_of_parallel():
    """all_of should not block worker threads."""
    pool = cfuture.ThreadPoolExecutor(workers=4)
    start = time.time()
    futures = [pool.submit(lambda: time.sleep(0.3)) for _ in range(4)]
    combined = cfuture.all_of(*futures)
    combined.result(timeout=5.0)
    elapsed = time.time() - start
    assert elapsed < 1.2, f"Expected parallel execution, got {elapsed:.2f}s"
    pool.shutdown()


def test_all_of_then_chain():
    """all_of result can be chained with .then()."""
    pool = cfuture.ThreadPoolExecutor(workers=2)
    f1 = pool.submit(lambda: 1)
    f2 = pool.submit(lambda: 2)
    result_f = cfuture.all_of(f1, f2).then(lambda results, d, s: sum(results), deps=[])
    assert result_f.result(timeout=5.0) == 3
    pool.shutdown()


def test_all_of_single():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 42)
    combined = cfuture.all_of(f)
    results = combined.result(timeout=5.0)
    assert results[0] == 42
    pool.shutdown()
