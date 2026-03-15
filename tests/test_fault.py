"""
P1: Worker fault tolerance tests.

These tests run in subprocesses to avoid killing the test runner.
"""

import subprocess
import sys
import textwrap
import pytest


def run_in_subprocess(code: str, timeout: float = 15.0) -> tuple[int, str, str]:
    """Run Python code in a subprocess, return (returncode, stdout, stderr)."""
    result = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return result.returncode, result.stdout, result.stderr


def test_basic_pool_runs():
    """Smoke test: pool starts, tasks run, pool shuts down cleanly."""
    code = """
        import cfuture
        pool = cfuture.ThreadPoolExecutor(workers=2)
        f = pool.submit(lambda: 42)
        result = f.result(timeout=5.0)
        assert result == 42, f"Expected 42, got {result}"
        pool.shutdown()
        print("OK")
    """
    rc, stdout, stderr = run_in_subprocess(code)
    assert rc == 0, f"subprocess failed:\nstdout: {stdout}\nstderr: {stderr}"
    assert "OK" in stdout


def test_exception_in_task_does_not_kill_pool():
    """A failing task does not kill the worker — pool continues processing."""
    code = """
        import cfuture
        pool = cfuture.ThreadPoolExecutor(workers=1)

        def boom():
            raise RuntimeError("intentional failure")

        f1 = pool.submit(boom)
        f2 = pool.submit(lambda: 99)

        try:
            f1.result(timeout=5.0)
        except RuntimeError:
            pass

        result = f2.result(timeout=5.0)
        assert result == 99, f"Expected 99, got {result}"
        pool.shutdown()
        print("OK")
    """
    rc, stdout, stderr = run_in_subprocess(code)
    assert rc == 0, f"subprocess failed:\nstdout: {stdout}\nstderr: {stderr}"
    assert "OK" in stdout


def test_many_tasks():
    """Submit many tasks and verify all complete."""
    code = """
        import cfuture
        pool = cfuture.ThreadPoolExecutor(workers=4)
        futures = [pool.submit(lambda: i * i) for i in range(20)]
        results = [f.result(timeout=5.0) for f in futures]
        # Results may be in any order due to parallelism, but all should be squares
        pool.shutdown()
        print("OK", len(results))
    """
    rc, stdout, stderr = run_in_subprocess(code)
    assert rc == 0, f"subprocess failed:\nstdout: {stdout}\nstderr: {stderr}"
    assert "OK 20" in stdout


def test_chained_callbacks_in_subprocess():
    """Callback chains work correctly."""
    code = """
        import cfuture
        pool = cfuture.ThreadPoolExecutor(workers=2)
        result = (
            pool.submit(lambda: 5)
            .then(lambda x, d: x + d[0], deps=[10])
            .then(lambda x, d: x * 2, deps=[])
            .result(timeout=5.0)
        )
        assert result == 30, f"Expected 30, got {result}"
        pool.shutdown()
        print("OK")
    """
    rc, stdout, stderr = run_in_subprocess(code)
    assert rc == 0, f"subprocess failed:\nstdout: {stdout}\nstderr: {stderr}"
    assert "OK" in stdout


def test_all_of_in_subprocess():
    """all_of resolves only after all input futures complete."""
    code = """
        import cfuture, time
        pool = cfuture.ThreadPoolExecutor(workers=4)
        start = time.time()
        futures = [pool.submit(lambda: time.sleep(0.1)) for _ in range(4)]
        combined = cfuture.all_of(*futures)
        combined.result(timeout=5.0)
        elapsed = time.time() - start
        assert elapsed < 0.8, f"Expected parallel execution but took {elapsed:.2f}s"
        pool.shutdown()
        print("OK")
    """
    rc, stdout, stderr = run_in_subprocess(code)
    assert rc == 0, f"subprocess failed:\nstdout: {stdout}\nstderr: {stderr}"
    assert "OK" in stdout
