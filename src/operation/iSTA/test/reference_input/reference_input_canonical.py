#!/usr/bin/env python3
"""Strict canonical primitives shared by the independent Step 4 wire codecs."""
# ruff: noqa: FBT001

import math
import struct
from collections.abc import Callable
from enum import IntEnum
from typing import TypeVar

_MAX_BLOB_BYTES = 256 * 1024 * 1024
_MAX_VECTOR_ITEMS = 1_000_000
_FINITE_DOUBLE = 0
_POSITIVE_INFINITY = 1
_NEGATIVE_INFINITY = 2


class CanonicalError(RuntimeError):
    """A Step 4 test wire is malformed, non-canonical, or out of bounds."""


T = TypeVar("T")
E = TypeVar("E", bound=IntEnum)


class CanonicalWriter:
    """Small big-endian writer; it is independent from production CSR code."""

    def __init__(self) -> None:
        self._data = bytearray()

    def u8(self, value: int) -> None:
        if type(value) is not int or not 0 <= value < 1 << 8:
            raise CanonicalError(f"non-canonical u8 value: {value!r}")
        self._data.append(value)

    def boolean(self, value: bool) -> None:
        if type(value) is not bool:
            raise CanonicalError(f"non-canonical bool value: {value!r}")
        self.u8(1 if value else 0)

    def u64(self, value: int) -> None:
        if type(value) is not int or not 0 <= value < 1 << 64:
            raise CanonicalError(f"non-canonical u64 value: {value!r}")
        self._data.extend(value.to_bytes(8, "big", signed=False))

    def i32(self, value: int) -> None:
        if type(value) is not int or not -(1 << 31) <= value < 1 << 31:
            raise CanonicalError(f"non-canonical i32 value: {value!r}")
        self._data.extend(value.to_bytes(4, "big", signed=True))

    def enum(self, value: IntEnum) -> None:
        if not isinstance(value, IntEnum):
            raise CanonicalError(f"canonical enum must be IntEnum, got {value!r}")
        self.u8(int(value))

    def finite_double(self, value: float) -> None:
        if type(value) is not float or not math.isfinite(value):
            raise CanonicalError(f"canonical finite double required, got {value!r}")
        normalized = 0.0 if value == 0.0 else value
        self.u8(_FINITE_DOUBLE)
        self._data.extend(struct.pack(">d", normalized))

    def canary_double(self, value: float) -> None:
        if type(value) is not float or math.isnan(value):
            raise CanonicalError(f"canonical canary double required, got {value!r}")
        if value == math.inf:
            self.u8(_POSITIVE_INFINITY)
        elif value == -math.inf:
            self.u8(_NEGATIVE_INFINITY)
        else:
            self.finite_double(value)

    def raw_bytes(self, value: bytes) -> None:
        if not isinstance(value, bytes) or len(value) > _MAX_BLOB_BYTES:
            raise CanonicalError("canonical byte string is invalid or too large")
        self.u64(len(value))
        self._data.extend(value)

    def string(self, value: str) -> None:
        if not isinstance(value, str):
            raise CanonicalError(f"canonical string required, got {value!r}")
        self.raw_bytes(value.encode("utf-8", errors="strict"))

    def optional(self, value: T | None, write_value: Callable[[T], None]) -> None:
        self.boolean(value is not None)
        if value is not None:
            write_value(value)

    def vector(self, values: tuple[T, ...], write_item: Callable[[T], None]) -> None:
        if not isinstance(values, tuple) or len(values) > _MAX_VECTOR_ITEMS:
            raise CanonicalError("canonical vector must be a bounded tuple")
        self.u64(len(values))
        for item in values:
            write_item(item)

    def result(self) -> bytes:
        return bytes(self._data)


class CanonicalReader:
    """Bounds-checked reader with strict bool, enum, UTF-8, and double rules."""

    def __init__(self, payload: bytes, where: str) -> None:
        if not isinstance(payload, bytes) or len(payload) > _MAX_BLOB_BYTES:
            raise CanonicalError(f"{where} payload is invalid or too large")
        self._payload = payload
        self._offset = 0
        self.where = where

    def _take(self, size: int) -> bytes:
        if size < 0 or size > len(self._payload) - self._offset:
            raise CanonicalError(f"{self.where} truncated at byte {self._offset}; requested {size}")
        result = self._payload[self._offset : self._offset + size]
        self._offset += size
        return result

    def u8(self) -> int:
        return self._take(1)[0]

    def boolean(self) -> bool:
        value = self.u8()
        if value not in (0, 1):
            raise CanonicalError(f"{self.where} has non-canonical bool {value}")
        return value == 1

    def u64(self) -> int:
        return int.from_bytes(self._take(8), "big", signed=False)

    def i32(self) -> int:
        return int.from_bytes(self._take(4), "big", signed=True)

    def enum(self, enum_type: type[E], field: str) -> E:
        value = self.u8()
        try:
            return enum_type(value)
        except ValueError as exc:
            raise CanonicalError(f"{self.where}.{field} has unknown enum {value}") from exc

    def _double(self, *, allow_infinity: bool) -> float:
        value_class = self.u8()
        if value_class == _POSITIVE_INFINITY and allow_infinity:
            return math.inf
        if value_class == _NEGATIVE_INFINITY and allow_infinity:
            return -math.inf
        if value_class != _FINITE_DOUBLE:
            raise CanonicalError(f"{self.where} has invalid double class {value_class}")
        raw = self._take(8)
        value = struct.unpack(">d", raw)[0]
        if not math.isfinite(value):
            raise CanonicalError(f"{self.where} finite double contains NaN/Inf bits")
        if value == 0.0 and raw != b"\x00" * 8:
            raise CanonicalError(f"{self.where} contains non-canonical negative zero")
        return value

    def finite_double(self) -> float:
        return self._double(allow_infinity=False)

    def canary_double(self) -> float:
        return self._double(allow_infinity=True)

    def raw_bytes(self) -> bytes:
        size = self.u64()
        if size > _MAX_BLOB_BYTES:
            raise CanonicalError(f"{self.where} byte-string length exceeds limit")
        return self._take(size)

    def string(self) -> str:
        raw = self.raw_bytes()
        try:
            value = raw.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise CanonicalError(f"{self.where} has invalid UTF-8: {exc}") from exc
        if value.encode("utf-8") != raw:
            raise CanonicalError(f"{self.where} has non-canonical UTF-8")
        return value

    def optional(self, read_value: Callable[[], T]) -> T | None:
        return read_value() if self.boolean() else None

    def vector(self, read_item: Callable[[int], T], field: str) -> tuple[T, ...]:
        count = self.u64()
        if count > _MAX_VECTOR_ITEMS:
            raise CanonicalError(f"{self.where}.{field} count exceeds limit")
        return tuple(read_item(index) for index in range(count))

    def finish(self) -> None:
        if self._offset != len(self._payload):
            raise CanonicalError(
                f"{self.where} has {len(self._payload) - self._offset} trailing bytes"
            )
