"""Tests for __xi_encode__/__xi_decode__ protocol and @xi_dataclass decorator."""
import dataclasses
import cfuture
from cfuture import Future, xi_dataclass


# Module-level class definitions so the worker interpreter can import and
# resolve them by module + qualname during __xi_decode__.

@xi_dataclass
@dataclasses.dataclass
class Vector:
    x: float
    y: float
    z: float


@xi_dataclass
@dataclasses.dataclass
class Tag:
    name: str


class Color:
    def __init__(self, r: int, g: int, b: int):
        self.r = r
        self.g = g
        self.b = b

    def __xi_encode__(self) -> dict:
        return {"r": self.r, "g": self.g, "b": self.b}

    @classmethod
    def __xi_decode__(cls, data: dict) -> "Color":
        return cls(data["r"], data["g"], data["b"])


@xi_dataclass
@dataclasses.dataclass
class Outer:
    label: str
    count: int


@xi_dataclass
@dataclasses.dataclass
class Config:
    host: str
    port: int
    debug: bool


@xi_dataclass
@dataclasses.dataclass
class Rectangle:
    width: float
    height: float

    def area(self) -> float:
        return self.width * self.height

    def perimeter(self) -> float:
        return 2 * (self.width + self.height)


def test_xi_dataclass_basic_roundtrip():
    """@xi_dataclass encodes and decodes a simple dataclass."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        v = Vector(1.0, 2.0, 3.0)
        f: Future[float] = pool.submit(lambda: 0).then(
            lambda x, d: d[0].x + d[0].y + d[0].z,
            deps=[v],
        )
        assert abs(f.result(timeout=5.0) - 6.0) < 1e-9


def test_xi_dataclass_adds_encode_decode():
    """@xi_dataclass attaches __xi_encode__ and __xi_decode__."""
    t = Tag("hello")
    encoded: dict = t.__xi_encode__()
    assert isinstance(encoded, dict)
    assert encoded["name"] == "hello"

    decoded: Tag = Tag.__xi_decode__(encoded)
    assert decoded.name == "hello"


def test_manual_xi_protocol():
    """Manual __xi_encode__/__xi_decode__ implementation works."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        c = Color(255, 128, 0)
        f: Future[int] = pool.submit(lambda: 0).then(
            lambda x, d: d[0].r + d[0].g + d[0].b,
            deps=[c],
        )
        assert f.result(timeout=5.0) == 383


def test_nested_xi_dataclass():
    """xi_dataclass fields encode/decode correctly."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        obj = Outer("test", 7)
        f: Future[int] = pool.submit(lambda: 0).then(
            lambda x, d: d[0].count * 2,
            deps=[obj],
        )
        assert f.result(timeout=5.0) == 14


def test_xi_dataclass_with_string_fields():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        cfg = Config("localhost", 8080, False)
        f: Future[str] = pool.submit(lambda: 0).then(
            lambda x, d: f"{d[0].host}:{d[0].port}",
            deps=[cfg],
        )
        assert f.result(timeout=5.0) == "localhost:8080"


def test_xi_dataclass_custom_method_callable_in_worker():
    """Custom methods on xi_dataclass are callable on the decoded object in the worker."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        rect = Rectangle(3.0, 4.0)
        f: Future[float] = pool.submit(lambda: 0).then(
            lambda x, d: d[0].area(),
            deps=[rect],
        )
        assert abs(f.result(timeout=5.0) - 12.0) < 1e-9


def test_xi_dataclass_multiple_method_calls_in_worker():
    """Multiple method calls on the decoded object work correctly."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        rect = Rectangle(3.0, 4.0)
        f: Future[float] = pool.submit(lambda: 0).then(
            lambda x, d: d[0].area() + d[0].perimeter(),
            deps=[rect],
        )
        assert abs(f.result(timeout=5.0) - 26.0) < 1e-9  # 12 + 14
