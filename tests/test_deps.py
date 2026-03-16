"""SharedValue encoding/decoding tests."""
import pytest
import cfuture
from cfuture import Future


def test_none_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[bool] = pool.submit(lambda: None).then(
            lambda x, d: d[0] is None, deps=[None]
        )
        assert f.result(timeout=5.0) is True


def test_bool_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[bool] = pool.submit(lambda: 0).then(lambda x, d: d[0], deps=[True])
        assert f.result(timeout=5.0) is True


def test_int_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 0).then(lambda x, d: d[0] + 1, deps=[41])
        assert f.result(timeout=5.0) == 42


def test_float_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[float] = pool.submit(lambda: 0).then(lambda x, d: d[0] * 2, deps=[3.14])
        result: float = f.result(timeout=5.0)
        assert abs(result - 6.28) < 1e-9


def test_str_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[str] = pool.submit(lambda: 0).then(
            lambda x, d: d[0] + " world", deps=["hello"]
        )
        assert f.result(timeout=5.0) == "hello world"


def test_bytes_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 0).then(lambda x, d: len(d[0]), deps=[b"abc"])
        assert f.result(timeout=5.0) == 3


def test_list_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        items: list[int] = [1, 2, 3]
        f: Future[int] = pool.submit(lambda: 0).then(lambda x, d: sum(d[0]), deps=[items])
        assert f.result(timeout=5.0) == 6


def test_dict_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        config: dict[str, float] = {"scale": 10.0}
        f: Future[float] = pool.submit(lambda: 0).then(
            lambda x, d: d[0]["scale"] * 2, deps=[config]
        )
        result: float = f.result(timeout=5.0)
        assert abs(result - 20.0) < 1e-9


def test_tuple_dep():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        t: tuple[int, int, int] = (1, 2, 3)
        f: Future[int] = pool.submit(lambda: 0).then(lambda x, d: d[0][1], deps=[t])
        assert f.result(timeout=5.0) == 2


def test_mutation_after_registration_has_no_effect():
    """Mutations to deps after .then() registration must not affect callback."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        data: list[int] = [1, 2, 3]
        f: Future[list[int]] = pool.submit(lambda: 0).then(
            lambda x, d: d[0][:], deps=[data]
        )
        data.append(99)
        result: list[int] = f.result(timeout=5.0)
        assert result == [1, 2, 3], f"Expected [1,2,3] but got {result}"


def test_pickled_dep():
    """cfuture.pickled() allows unsupported types via pickle."""
    import datetime
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        dt = datetime.datetime(2024, 1, 1, 12, 0, 0)
        f: Future[str] = pool.submit(lambda: 0).then(
            lambda x, d: str(d[0].year), deps=[cfuture.pickled(dt)]
        )
        assert f.result(timeout=5.0) == "2024"


def test_non_transferable_raises_at_registration():
    """Non-serialisable objects raise TypeError at .then() time."""
    import socket
    sock = socket.socket()
    try:
        with cfuture.ThreadPoolExecutor(workers=1) as pool:
            f: Future[None] = pool.submit(lambda: 0)
            with pytest.raises(TypeError):
                f.then(lambda x, d: None, deps=[sock])
    finally:
        sock.close()


# ── shared= startup injection tests ──────────────────────────────────────────

def test_shared_must_be_dict():
    """shared= must be a dict, not a list."""
    with pytest.raises(TypeError):
        cfuture.ThreadPoolExecutor(workers=1, shared=["not", "a", "dict"])


# ── shared= value access inside callbacks ─────────────────────────────────────

def test_shared_string_accessible_in_then():
    """shared dict is passed as third arg and values are accessible."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"greeting": "hello"}) as pool:
        f: Future[str] = pool.submit(lambda: "world").then(
            lambda x, d, s: s["greeting"] + " " + x
        )
        assert f.result(timeout=5.0) == "hello world"


def test_shared_int_accessible_in_then():
    """shared integer is accessible and correct type."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"scale": 7}) as pool:
        f: Future[int] = pool.submit(lambda: 6).then(lambda x, d, s: x * s["scale"])
        assert f.result(timeout=5.0) == 42


def test_shared_dict_accessible_in_then():
    """shared nested dict is accessible."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"cfg": {"factor": 3}}) as pool:
        f: Future[int] = pool.submit(lambda: 10).then(
            lambda x, d, s: x * s["cfg"]["factor"]
        )
        assert f.result(timeout=5.0) == 30


def test_shared_and_deps_together():
    """shared dict and deps list are both accessible in the same callback."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"base": 100}) as pool:
        f: Future[int] = pool.submit(lambda: 0).then(
            lambda x, d, s: s["base"] + d[0], deps=[5]
        )
        assert f.result(timeout=5.0) == 105


def test_shared_is_empty_dict_when_no_shared():
    """When no shared= is given, s is an empty dict (not None)."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[bool] = pool.submit(lambda: 0).then(
            lambda x, d, s: isinstance(s, dict) and len(s) == 0
        )
        assert f.result(timeout=5.0) is True


def test_shared_is_deep_copy_per_callback():
    """Each callback invocation gets an independent copy of shared values."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"items": [1, 2, 3]}) as pool:
        f1: Future[list[int]] = pool.submit(lambda: 0).then(lambda x, d, s: s["items"])
        f2: Future[list[int]] = pool.submit(lambda: 0).then(lambda x, d, s: s["items"])
        r1: list[int] = f1.result(timeout=5.0)
        r2: list[int] = f2.result(timeout=5.0)
        assert r1 == [1, 2, 3]
        assert r2 == [1, 2, 3]
        assert r1 is not r2  # independent copies


def test_shared_accessible_in_except():
    """shared dict is accessible in except_ callbacks."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"fallback": -1}) as pool:
        def fail() -> int:
            raise RuntimeError("oops")

        f: Future[int] = pool.submit(fail).except_(lambda e, d, s: s["fallback"])
        assert f.result(timeout=5.0) == -1
