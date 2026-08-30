# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
# http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
#
# See the Mulan PSL v2 for more details.
# ***************************************************************************************
"""Small strict JSON loader and JSON Schema subset shared by STA contracts."""

import json
import math
import re
from datetime import datetime
from pathlib import Path
from typing import NoReturn


class ValidationError(ValueError):
    """A structured raw-JSON or declarative-schema contract violation."""

    def __init__(
        self,
        message: str,
        *,
        field: str = "$",
        category: str = "shape",
    ) -> None:
        super().__init__(f"{field}: {message}")
        self.message = message
        self.field = field
        self.category = category


def fail(field: str, message: str, category: str = "relationship") -> NoReturn:
    raise ValidationError(message, field=field, category=category)


def _reject_constant(token: str) -> NoReturn:
    raise ValidationError(f"invalid JSON numeric constant {token!r}", category="non_finite")


def _object_without_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON object key {key!r}", category="duplicate_key")
        result[key] = value
    return result


def loads_json(text: str) -> object:
    """Load strict JSON, rejecting duplicate keys and non-standard constants."""

    try:
        return json.loads(
            text,
            parse_constant=_reject_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except json.JSONDecodeError as error:
        raise ValidationError(
            f"invalid JSON at line {error.lineno}, column {error.colno}: {error.msg}",
            category="json_syntax",
        ) from error


def load_json(path: Path) -> object:
    try:
        return loads_json(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"cannot read {path}: {error}", category="json_syntax") from error


def resolve_ref(root_schema: dict[str, object], reference: str) -> dict[str, object]:
    if not reference.startswith("#/"):
        raise ValidationError(f"unsupported non-local schema reference {reference!r}")
    current: object = root_schema
    for raw_part in reference[2:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        if not isinstance(current, dict) or part not in current:
            raise ValidationError(f"unresolvable schema reference {reference!r}")
        current = current[part]
    if not isinstance(current, dict):
        raise ValidationError(f"schema reference {reference!r} is not an object")
    return current


def _is_number(value: object) -> bool:
    return type(value) is int or type(value) is float and math.isfinite(value)


def _matches_type(value: object, expected: str) -> bool:
    match expected:
        case "null":
            return value is None
        case "boolean":
            return type(value) is bool
        case "integer":
            return type(value) is int
        case "number":
            return _is_number(value)
        case "string":
            return type(value) is str
        case "array":
            return type(value) is list
        case "object":
            return type(value) is dict
        case _:
            raise ValidationError(f"unsupported schema type {expected!r}")


def _json_equal(left: object, right: object) -> bool:
    if _is_number(left) and _is_number(right):
        return left == right
    return type(left) is type(right) and left == right


def _validate_datetime(value: str, field: str) -> None:
    if not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})",
        value,
    ):
        fail(
            field,
            "must be an RFC 3339 timestamp with an explicit UTC offset",
            "format",
        )
    try:
        parsed = datetime.fromisoformat(
            value.removesuffix("Z") + ("+00:00" if value.endswith("Z") else "")
        )
    except ValueError as error:
        fail(field, f"invalid date-time: {error}", "format")
    if parsed.tzinfo is None:
        fail(field, "date-time must include a timezone", "format")


def _schema_matches(
    value: object,
    schema: dict[str, object],
    root_schema: dict[str, object],
    field: str,
) -> bool:
    try:
        _validate(value, schema, root_schema, field)
    except ValidationError:
        return False
    return True


def _declared_type_matches(
    value: object, schema: dict[str, object], root_schema: dict[str, object]
) -> bool:
    reference = schema.get("$ref")
    if isinstance(reference, str):
        return _declared_type_matches(value, resolve_ref(root_schema, reference), root_schema)
    expected = schema.get("type")
    if isinstance(expected, str):
        return _matches_type(value, expected)
    if isinstance(expected, list) and all(isinstance(item, str) for item in expected):
        return any(_matches_type(value, item) for item in expected)
    return False


def _validate_combinators(
    value: object,
    schema: dict[str, object],
    root_schema: dict[str, object],
    field: str,
) -> None:
    all_of = schema.get("allOf")
    if all_of is not None:
        if not isinstance(all_of, list) or not all(isinstance(item, dict) for item in all_of):
            raise ValidationError("schema allOf must be an array of objects")
        for item in all_of:
            _validate(value, item, root_schema, field)
    any_of = schema.get("anyOf")
    if any_of is not None:
        if not isinstance(any_of, list) or not all(isinstance(item, dict) for item in any_of):
            raise ValidationError("schema anyOf must be an array of objects")
        errors: list[tuple[dict[str, object], ValidationError]] = []
        for item in any_of:
            try:
                _validate(value, item, root_schema, field)
                break
            except ValidationError as error:
                errors.append((item, error))
        else:
            for item, error in errors:
                if _declared_type_matches(value, item, root_schema):
                    raise error
            fail(field, "must match at least one anyOf branch", "shape")
    negated = schema.get("not")
    if negated is not None:
        if not isinstance(negated, dict):
            raise ValidationError("schema not must be an object")
        if _schema_matches(value, negated, root_schema, field):
            fail(field, "must not match the excluded schema")
    condition = schema.get("if")
    if condition is not None:
        if not isinstance(condition, dict):
            raise ValidationError("schema if must be an object")
        branch_name = "then" if _schema_matches(value, condition, root_schema, field) else "else"
        branch = schema.get(branch_name)
        if branch is not None:
            if not isinstance(branch, dict):
                raise ValidationError(f"schema {branch_name} must be an object")
            _validate(value, branch, root_schema, field)


def _validate(
    value: object,
    schema: dict[str, object],
    root_schema: dict[str, object],
    field: str = "$",
) -> None:
    reference = schema.get("$ref")
    if reference is not None:
        if not isinstance(reference, str):
            raise ValidationError("schema $ref must be a string")
        _validate(value, resolve_ref(root_schema, reference), root_schema, field)
        return
    _validate_combinators(value, schema, root_schema, field)
    expected_types = schema.get("type")
    if expected_types is not None:
        if isinstance(expected_types, str):
            expected = [expected_types]
        elif isinstance(expected_types, list) and all(
            isinstance(item, str) for item in expected_types
        ):
            expected = expected_types
        else:
            raise ValidationError("schema type must be a string or string array")
        if not any(_matches_type(value, item) for item in expected):
            if "number" in expected and type(value) is float and not math.isfinite(value):
                fail(field, "must be finite", "non_finite")
            fail(
                field,
                f"expected {' or '.join(expected)}, got {type(value).__name__}",
                "value_type",
            )
    if "const" in schema and not _json_equal(value, schema["const"]):
        fail(field, f"must equal {schema['const']!r}", "enum")
    enum = schema.get("enum")
    if enum is not None:
        if not isinstance(enum, list):
            raise ValidationError("schema enum must be an array")
        if not any(_json_equal(value, candidate) for candidate in enum):
            fail(field, f"must be one of {enum!r}", "enum")
    if value is None:
        return
    if type(value) is dict:
        if any(type(key) is not str for key in value):
            fail(field, "mapping keys must be exact strings", "key_type")
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        if not isinstance(properties, dict) or not isinstance(required, list):
            raise ValidationError("schema object properties/required have invalid types")
        for key in required:
            if key not in value:
                fail(f"{field}.{key}", "required key is missing", "missing_key")
        if schema.get("additionalProperties") is False:
            unexpected = sorted(set(value) - set(properties))
            if unexpected:
                fail(f"{field}.{unexpected[0]}", "unexpected key", "extra_key")
        for key, child_schema in properties.items():
            if key in value:
                if not isinstance(child_schema, dict):
                    raise ValidationError(f"schema for property {key!r} is not an object")
                _validate(value[key], child_schema, root_schema, f"{field}.{key}")
    if type(value) is list:
        minimum_items = schema.get("minItems")
        if minimum_items is not None and len(value) < minimum_items:
            fail(field, f"must contain at least {minimum_items} items", "range")
        if schema.get("uniqueItems") is True:
            canonical = [json.dumps(item, sort_keys=True, separators=(",", ":")) for item in value]
            if len(set(canonical)) != len(canonical):
                fail(field, "array items must be unique")
        item_schema = schema.get("items")
        if item_schema is not None:
            if not isinstance(item_schema, dict):
                raise ValidationError("schema items must be an object")
            for index, item in enumerate(value):
                _validate(item, item_schema, root_schema, f"{field}[{index}]")
    if type(value) is str:
        minimum_length = schema.get("minLength")
        if minimum_length is not None and len(value) < minimum_length:
            fail(field, f"must contain at least {minimum_length} characters", "range")
        pattern = schema.get("pattern")
        if pattern is not None:
            if not isinstance(pattern, str):
                raise ValidationError("schema pattern must be a string")
            if re.search(pattern, value) is None:
                fail(field, f"does not match {pattern!r}", "format")
        if schema.get("format") == "date-time":
            _validate_datetime(value, field)
    if _is_number(value):
        for keyword, comparator, token in (
            ("minimum", lambda left, right: left < right, ">="),
            ("maximum", lambda left, right: left > right, "<="),
            ("exclusiveMinimum", lambda left, right: left <= right, ">"),
        ):
            boundary = schema.get(keyword)
            if boundary is not None and comparator(value, boundary):
                fail(field, f"must be {token} {boundary}", "range")


def validate_schema(document: object, schema: object) -> None:
    """Validate ``document`` against the supported strict schema subset."""

    if not isinstance(schema, dict):
        raise ValidationError("root schema must be an object")
    _validate(document, schema, schema)
