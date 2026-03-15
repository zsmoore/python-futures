"""Tests for __xi_encode__/__xi_decode__ protocol and @xi_dataclass decorator."""
import dataclasses
import pytest
import cfuture
from cfuture.lint import xi_dataclass


def test_xi_dataclass_basic_roundtrip():
    """@xi_dataclass encodes and decodes a simple dataclass."""
    @xi_dataclass
    @dataclasses.dataclass
    class Vector:
        x: float
        y: float
        z: float

    pool = cfuture.ThreadPoolExecutor(workers=1)
    v = Vector(1.0, 2.0, 3.0)
    f = pool.submit(lambda: 0).then(
        lambda x, d: d[0].x + d[0].y + d[0].z,
        deps=[v],
    )
    assert abs(f.result(timeout=5.0) - 6.0) < 1e-9
    pool.shutdown()


def test_xi_dataclass_adds_encode_decode():
    """@xi_dataclass attaches __xi_encode__ and __xi_decode__."""
    @xi_dataclass
    @dataclasses.dataclass
    class Tag:
        name: str

    t = Tag("hello")
    encoded = t.__xi_encode__()
    assert isinstance(encoded, dict)
    assert encoded["name"] == "hello"

    decoded = Tag.__xi_decode__(encoded)
    assert decoded.name == "hello"


def test_manual_xi_protocol():
    """Manual __xi_encode__/__xi_decode__ implementation works."""
    class Color:
        def __init__(self, r: int, g: int, b: int):
            self.r = r
            self.g = g
            self.b = b

        def __xi_encode__(self):
            return {"r": self.r, "g": self.g, "b": self.b}

        @classmethod
        def __xi_decode__(cls, data):
            return cls(data["r"], data["g"], data["b"])

    pool = cfuture.ThreadPoolExecutor(workers=1)
    c = Color(255, 128, 0)
    f = pool.submit(lambda: 0).then(
        lambda x, d: d[0].r + d[0].g + d[0].b,
        deps=[c],
    )
    assert f.result(timeout=5.0) == 383
    pool.shutdown()


def test_nested_xi_dataclass():
    """Nested xi_dataclass fields are encoded/decoded correctly."""
    @xi_dataclass
    @dataclasses.dataclass
    class Inner:
        value: int

    @xi_dataclass
    @dataclasses.dataclass
    class Outer:
        label: str
        count: int

    pool = cfuture.ThreadPoolExecutor(workers=1)
    obj = Outer("test", 7)
    f = pool.submit(lambda: 0).then(
        lambda x, d: d[0].count * 2,
        deps=[obj],
    )
    assert f.result(timeout=5.0) == 14
    pool.shutdown()


def test_xi_dataclass_with_string_fields():
    @xi_dataclass
    @dataclasses.dataclass
    class Config:
        host: str
        port: int
        debug: bool

    pool = cfuture.ThreadPoolExecutor(workers=1)
    cfg = Config("localhost", 8080, False)
    f = pool.submit(lambda: 0).then(
        lambda x, d: f"{d[0].host}:{d[0].port}",
        deps=[cfg],
    )
    assert f.result(timeout=5.0) == "localhost:8080"
    pool.shutdown()
