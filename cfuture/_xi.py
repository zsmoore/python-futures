"""
cfuture._xi — xi protocol utilities.

xi_dataclass  — decorator: auto-implement __xi_encode__ / __xi_decode__
                for dataclasses so they can be passed as deps= across
                interpreter boundaries.
"""

import dataclasses


def xi_dataclass(cls):
    """Decorator that auto-implements __xi_encode__ / __xi_decode__ for dataclasses."""
    def __xi_encode__(self):
        return dataclasses.asdict(self)

    @classmethod
    def __xi_decode__(klass, data):
        return klass(**data)

    cls.__xi_encode__ = __xi_encode__
    cls.__xi_decode__ = __xi_decode__
    return cls
