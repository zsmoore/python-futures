"""SharedValue encoding/decoding tests."""
import pytest
import cfuture


def test_none_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: None).then(lambda x, d: d[0] is None, deps=[None])
    assert f.result(timeout=5.0) is True
    pool.shutdown()


def test_bool_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 0).then(lambda x, d: d[0], deps=[True])
    assert f.result(timeout=5.0) is True
    pool.shutdown()


def test_int_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 0).then(lambda x, d: d[0] + 1, deps=[41])
    assert f.result(timeout=5.0) == 42
    pool.shutdown()


def test_float_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 0).then(lambda x, d: d[0] * 2, deps=[3.14])
    result = f.result(timeout=5.0)
    assert abs(result - 6.28) < 1e-9
    pool.shutdown()


def test_str_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 0).then(lambda x, d: d[0] + " world", deps=["hello"])
    assert f.result(timeout=5.0) == "hello world"
    pool.shutdown()


def test_bytes_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 0).then(lambda x, d: len(d[0]), deps=[b"abc"])
    assert f.result(timeout=5.0) == 3
    pool.shutdown()


def test_list_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    items = [1, 2, 3]
    f = pool.submit(lambda: 0).then(lambda x, d: sum(d[0]), deps=[items])
    assert f.result(timeout=5.0) == 6
    pool.shutdown()


def test_dict_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    config = {"scale": 10.0}
    f = pool.submit(lambda: 0).then(lambda x, d: d[0]["scale"] * 2, deps=[config])
    result = f.result(timeout=5.0)
    assert abs(result - 20.0) < 1e-9
    pool.shutdown()


def test_tuple_dep():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    t = (1, 2, 3)
    f = pool.submit(lambda: 0).then(lambda x, d: d[0][1], deps=[t])
    assert f.result(timeout=5.0) == 2
    pool.shutdown()


def test_mutation_after_registration_has_no_effect():
    """Mutations to deps after .then() registration must not affect callback."""
    pool = cfuture.ThreadPoolExecutor(workers=1)
    data = [1, 2, 3]
    f = pool.submit(lambda: 0).then(lambda x, d: d[0][:], deps=[data])
    # Mutate after registration
    data.append(99)
    result = f.result(timeout=5.0)
    assert result == [1, 2, 3], f"Expected [1,2,3] but got {result}"
    pool.shutdown()


def test_pickled_dep():
    """cfuture.pickled() allows unsupported types via pickle."""
    import datetime
    pool = cfuture.ThreadPoolExecutor(workers=1)
    dt = datetime.datetime(2024, 1, 1, 12, 0, 0)
    f = pool.submit(lambda: 0).then(lambda x, d: str(d[0].year), deps=[cfuture.pickled(dt)])
    assert f.result(timeout=5.0) == "2024"
    pool.shutdown()


def test_xi_dataclass():
    """@xi_dataclass auto-implements encode/decode."""
    import dataclasses

    @cfuture.xi_dataclass
    @dataclasses.dataclass
    class Point:
        x: float
        y: float

    pool = cfuture.ThreadPoolExecutor(workers=1)
    p = Point(3.0, 4.0)
    f = pool.submit(lambda: 0).then(
        lambda x, d: d[0].x + d[0].y,
        deps=[p]
    )
    result = f.result(timeout=5.0)
    assert abs(result - 7.0) < 1e-9, f"Expected 7.0 got {result}"
    pool.shutdown()


def test_non_transferable_raises_at_registration():
    """Non-serialisable objects raise TypeError at .then() time."""
    import socket
    pool = cfuture.ThreadPoolExecutor(workers=1)
    sock = socket.socket()
    f = pool.submit(lambda: 0)
    try:
        with pytest.raises(TypeError):
            f.then(lambda x, d: None, deps=[sock])
    finally:
        sock.close()
        pool.shutdown()
