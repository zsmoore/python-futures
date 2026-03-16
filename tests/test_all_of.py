"""Non-blocking all_of combinator tests."""
import time
import cfuture
from cfuture import Future


def _sleep_03() -> None:
    time.sleep(0.3)


def test_all_of_basic():
    with cfuture.ThreadPoolExecutor(workers=4) as pool:
        futures: list[Future[int]] = [
            pool.submit(lambda: 1),
            pool.submit(lambda: 2),
            pool.submit(lambda: 3),
        ]
        combined: Future[list[int]] = cfuture.all_of(*futures)
        results: list[int] = combined.result(timeout=5.0)
        assert isinstance(results, list)
        assert len(results) == 3


def test_all_of_empty():
    f: Future[list] = cfuture.all_of()
    result: list = f.result(timeout=1.0)
    assert result == []


def test_all_of_parallel():
    """all_of should not block worker threads."""
    with cfuture.ThreadPoolExecutor(workers=4) as pool:
        start: float = time.time()
        futures: list[Future[None]] = [pool.submit(_sleep_03) for _ in range(4)]
        combined: Future[list[None]] = cfuture.all_of(*futures)
        combined.result(timeout=5.0)
        elapsed: float = time.time() - start
        assert elapsed < 1.2, f"Expected parallel execution, got {elapsed:.2f}s"


def test_all_of_then_chain():
    """all_of result can be chained with .then()."""
    with cfuture.ThreadPoolExecutor(workers=2) as pool:
        f1: Future[int] = pool.submit(lambda: 1)
        f2: Future[int] = pool.submit(lambda: 2)
        result_f: Future[int] = cfuture.all_of(f1, f2).then(
            lambda results, d, s: sum(results)
        )
        assert result_f.result(timeout=5.0) == 3


def test_all_of_single():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 42)
        combined: Future[list[int]] = cfuture.all_of(f)
        results: list[int] = combined.result(timeout=5.0)
        assert results[0] == 42
