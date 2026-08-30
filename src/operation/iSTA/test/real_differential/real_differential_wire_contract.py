#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Strict JSON, schema, and neutral-wire primitives for Step 8."""

import json
import math
import re
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import real_differential_model as model


def _load_baseline_error():
    import baseline_contract

    return baseline_contract.BaselineError


BaselineError = _load_baseline_error()
sha256_bytes = model.sha256_bytes


class DifferentialContractError(BaselineError):
    """A Step 8 schema, wire, process, or comparison contract violation."""


def require(condition, message: str) -> None:
    if not condition:
        raise DifferentialContractError(message)


def mapping(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise DifferentialContractError(f"{where} must be an object")
    return value


def sequence(value: Any, where: str) -> list[Any]:
    if not isinstance(value, list):
        raise DifferentialContractError(f"{where} must be an array")
    return value


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DifferentialContractError(f"duplicate-key: {key!r}")
        result[key] = value
    return result


def _reject_constant(token: str) -> None:
    raise DifferentialContractError(f"non-finite JSON token: {token}")


def parse_strict_json_for_test(payload: bytes) -> Any:
    if type(payload) is not bytes:
        raise DifferentialContractError("strict JSON input must be bytes")
    try:
        text = payload.decode("utf-8", errors="strict")
        return json.loads(
            text,
            object_pairs_hook=_object_without_duplicates,
            parse_constant=_reject_constant,
        )
    except json.JSONDecodeError as exc:
        if exc.msg == "Extra data":
            raise DifferentialContractError(f"trailing-data: {exc}") from exc
        raise DifferentialContractError(f"strict JSON decode failed: {exc}") from exc
    except UnicodeError as exc:
        raise DifferentialContractError(f"strict JSON decode failed: {exc}") from exc


def load_json(path: Path) -> Any:
    try:
        return parse_strict_json_for_test(path.read_bytes())
    except (OSError, DifferentialContractError) as exc:
        raise DifferentialContractError(f"cannot read strict JSON from {path}: {exc}") from exc


_DRAFT = "https://json-schema.org/draft/2020-12/schema"
_TYPES = {"null", "boolean", "integer", "number", "string", "array", "object"}
_KEYWORDS = {
    "$schema",
    "$id",
    "title",
    "$defs",
    "$ref",
    "type",
    "required",
    "oneOf",
    "prefixItems",
    "items",
    "additionalProperties",
    "properties",
    "pattern",
    "minimum",
    "maximum",
    "minItems",
    "maxItems",
    "const",
    "enum",
}


def _schema_error(message: str) -> None:
    raise DifferentialContractError(f"schema-definition: {message}")


def _strict_json(value: Any, where: str) -> None:
    if value is None or type(value) in (bool, int):
        return
    if type(value) is float:
        if math.isfinite(value):
            return
        _schema_error(f"{where} contains a non-finite number")
    if isinstance(value, str):
        try:
            value.encode("utf-8", errors="strict")
        except UnicodeError as exc:
            raise DifferentialContractError(
                f"schema-definition: {where} contains invalid UTF-8"
            ) from exc
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _strict_json(item, f"{where}[{index}]")
        return
    if isinstance(value, dict) and all(isinstance(key, str) for key in value):
        for key, item in value.items():
            _strict_json(item, f"{where}.{key}")
        return
    _schema_error(f"{where} is not a strict JSON value")


def _marker(value: Any) -> Any:
    if isinstance(value, dict):
        return ("object", tuple((key, _marker(item)) for key, item in sorted(value.items())))
    if isinstance(value, list):
        return ("array", tuple(_marker(item) for item in value))
    return (type(value).__name__, value)


class _Schemas:
    def __init__(self, store):
        if not isinstance(store, Mapping) or not store:
            _schema_error("exact schema store must be a non-empty mapping")
        self.store = store

    @staticmethod
    def _declared(schema):
        value = schema.get("type")
        return None if value is None else ({value} if isinstance(value, str) else set(value))

    @classmethod
    def _compatible(cls, schema, expected, keyword):
        declared = cls._declared(schema)
        compatible = {expected} if expected != "number" else {"integer", "number"}
        if declared is not None and declared.isdisjoint(compatible):
            _schema_error(f"{keyword} is incompatible with declared type")

    def audit(self):
        for identifier, document in self.store.items():
            if not isinstance(identifier, str) or not isinstance(document, dict):
                _schema_error("exact schema store entry has invalid shape")
            if document.get("$id") != identifier:
                _schema_error("exact schema store key/$id differs")
            self._audit(document, identifier, root=True)
        for identifier, document in self.store.items():
            self._audit_refs(document, identifier, ((identifier, ""),), set())

    def _audit(self, raw, where, *, root=False, boolean_allowed=False):
        if type(raw) is bool:
            if boolean_allowed:
                return
            _schema_error(f"{where} may not be a boolean schema")
        if not isinstance(raw, dict):
            _schema_error(f"{where} must be a schema object")
        schema = raw
        unknown = set(schema) - _KEYWORDS
        if unknown:
            _schema_error(f"{where} has unknown keywords {sorted(unknown)}")
        if root:
            identifier = schema.get("$id")
            if schema.get("$schema") != _DRAFT:
                _schema_error(f"{where} has an unsupported $schema")
            if not isinstance(identifier, str) or not identifier or identifier not in self.store:
                _schema_error(f"{where} has a missing or foreign $id")
            if _marker(self.store[identifier]) != _marker(schema):
                _schema_error(f"{where} differs from its exact store entry")
        elif "$schema" in schema or "$id" in schema:
            _schema_error(f"{where} contains nested $schema/$id")
        if "title" in schema and not isinstance(schema["title"], str):
            _schema_error(f"{where}.title must be a string")
        for keyword in ("$defs", "properties"):
            if keyword not in schema:
                continue
            children = schema[keyword]
            valid = isinstance(children, dict) and all(isinstance(key, str) for key in children)
            if not valid:
                _schema_error(f"{where}.{keyword} must be an object of schemas")
            for key, child in children.items():
                self._audit(child, f"{where}.{keyword}.{key}")
        if "$ref" in schema and not isinstance(schema["$ref"], str):
            _schema_error(f"{where}.$ref must be a string")
        if "type" in schema:
            declared = schema["type"]
            values = [declared] if isinstance(declared, str) else declared
            valid = (
                isinstance(values, list)
                and bool(values)
                and all(isinstance(item, str) and item in _TYPES for item in values)
                and len(values) == len(set(values))
            )
            if not valid:
                _schema_error(f"{where}.type is not a unique recognized type list")
        if "required" in schema:
            required = schema["required"]
            valid = (
                isinstance(required, list)
                and all(isinstance(item, str) for item in required)
                and len(required) == len(set(required))
            )
            if not valid:
                _schema_error(f"{where}.required must contain unique strings")
            self._compatible(schema, "object", "required")
        for keyword in ("oneOf", "prefixItems"):
            if keyword not in schema:
                continue
            children = schema[keyword]
            if not isinstance(children, list) or not children:
                _schema_error(f"{where}.{keyword} must be a non-empty schema list")
            for index, child in enumerate(children):
                self._audit(child, f"{where}.{keyword}[{index}]")
            if keyword == "prefixItems":
                self._compatible(schema, "array", keyword)
        for keyword in ("items", "additionalProperties"):
            if keyword in schema:
                self._audit(schema[keyword], f"{where}.{keyword}", boolean_allowed=True)
                self._compatible(schema, "array" if keyword == "items" else "object", keyword)
        if "properties" in schema:
            self._compatible(schema, "object", "properties")
        if "pattern" in schema:
            if not isinstance(schema["pattern"], str):
                _schema_error(f"{where}.pattern must be a string")
            try:
                re.compile(schema["pattern"])
            except re.error as exc:
                raise DifferentialContractError(
                    f"schema-definition: {where}.pattern is invalid"
                ) from exc
            self._compatible(schema, "string", "pattern")
        for keyword in ("minimum", "maximum"):
            if keyword not in schema:
                continue
            value = schema[keyword]
            if type(value) not in (int, float) or not math.isfinite(value):
                _schema_error(f"{where}.{keyword} must be a finite non-bool number")
            self._compatible(schema, "number", keyword)
        if schema.get("minimum", -math.inf) > schema.get("maximum", math.inf):
            _schema_error(f"{where} has minimum greater than maximum")
        for keyword in ("minItems", "maxItems"):
            value = schema.get(keyword)
            if value is None:
                continue
            if type(value) is not int or value < 0:
                _schema_error(f"{where}.{keyword} must be a non-negative integer")
            self._compatible(schema, "array", keyword)
        if schema.get("minItems", 0) > schema.get("maxItems", math.inf):
            _schema_error(f"{where} has minItems greater than maxItems")
        if "const" in schema:
            _strict_json(schema["const"], f"{where}.const")
        if "enum" in schema:
            values = schema["enum"]
            if not isinstance(values, list) or not values:
                _schema_error(f"{where}.enum must be non-empty")
            for index, item in enumerate(values):
                _strict_json(item, f"{where}.enum[{index}]")
            markers = [_marker(item) for item in values]
            if len(markers) != len(set(markers)):
                _schema_error(f"{where}.enum has type-sensitive duplicates")

    @staticmethod
    def _pointer(token):
        if re.search(r"~(?:[^01]|$)", token):
            _schema_error("JSON Pointer contains an illegal escape")
        return token.replace("~1", "/").replace("~0", "~")

    def resolve(self, ref, current_id):
        base, marker, fragment = ref.partition("#")
        document_id = base or current_id
        if document_id not in self.store:
            _schema_error(f"foreign or missing $ref {ref!r}")
        if marker and fragment and not fragment.startswith("/"):
            _schema_error(f"$ref has a non-pointer fragment {ref!r}")
        target = self.store[document_id]
        if marker and fragment:
            for raw_token in fragment[1:].split("/"):
                token = self._pointer(raw_token)
                if isinstance(target, dict) and token in target:
                    target = target[token]
                elif isinstance(target, list) and token.isdecimal() and int(token) < len(target):
                    target = target[int(token)]
                else:
                    _schema_error(f"$ref pointer does not exist {ref!r}")
        if not isinstance(target, dict) and type(target) is not bool:
            _schema_error(f"$ref target is not a schema {ref!r}")
        return target, document_id, fragment

    @staticmethod
    def _children(schema):
        for keyword in ("$defs", "properties"):
            for key, child in schema.get(keyword, {}).items():
                yield child, f"{keyword}/{key}"
        for keyword in ("oneOf", "prefixItems"):
            for index, child in enumerate(schema.get(keyword, ())):
                yield child, f"{keyword}/{index}"
        for keyword in ("items", "additionalProperties"):
            if keyword in schema:
                yield schema[keyword], keyword

    def _audit_refs(self, raw, current_id, stack, visited):
        if type(raw) is bool:
            return
        if "$ref" in raw:
            target, target_id, fragment = self.resolve(raw["$ref"], current_id)
            token = target_id, fragment
            if token in stack:
                _schema_error(f"cyclic $ref {raw['$ref']!r}")
            if token not in visited:
                visited.add(token)
                self._audit_refs(target, target_id, (*stack, token), visited)
        for child, _where in self._children(raw):
            self._audit_refs(child, current_id, stack, visited)

    @staticmethod
    def _type(value, expected):
        return {
            "null": value is None,
            "boolean": type(value) is bool,
            "integer": type(value) is int,
            "number": type(value) in (int, float) and (type(value) is int or math.isfinite(value)),
            "string": isinstance(value, str),
            "array": isinstance(value, list),
            "object": isinstance(value, dict),
        }[expected]

    def validate(self, raw, value, where, current_id, stack=()):
        if type(raw) is bool:
            require(raw, f"{where} is rejected by a false schema")
            return
        schema = raw
        if "$ref" in schema:
            target, target_id, fragment = self.resolve(schema["$ref"], current_id)
            token = target_id, fragment
            require(token not in stack, f"{where} encountered cyclic schema reference")
            self.validate(target, value, where, target_id, (*stack, token))
        if "oneOf" in schema:
            accepted = 0
            for choice in schema["oneOf"]:
                try:
                    self.validate(choice, value, where, current_id, stack)
                except DifferentialContractError:
                    continue
                accepted += 1
            require(accepted == 1, f"{where} must match exactly one schema")
        expected = schema.get("type")
        choices = expected if isinstance(expected, list) else ([expected] if expected else [])
        require(
            not choices or any(self._type(value, item) for item in choices),
            f"{where} has invalid JSON type",
        )
        if "const" in schema:
            require(
                _marker(value) == _marker(schema["const"]), f"{where} differs from schema constant"
            )
        if "enum" in schema:
            valid = any(_marker(value) == _marker(item) for item in schema["enum"])
            require(valid, f"{where} is outside schema enum")
        if isinstance(value, str):
            try:
                value.encode("utf-8", errors="strict")
            except UnicodeError as exc:
                raise DifferentialContractError(f"{where} is not canonical UTF-8") from exc
            require(
                "pattern" not in schema or re.search(schema["pattern"], value) is not None,
                f"{where} does not match schema pattern",
            )
        if type(value) in (int, float):
            require(type(value) is int or math.isfinite(value), f"{where} must be finite")
            valid = value >= schema.get("minimum", value)
            valid = valid and value <= schema.get("maximum", value)
            require(valid, f"{where} is outside schema bounds")
        if isinstance(value, dict):
            properties = schema.get("properties", {})
            missing = set(schema.get("required", ())) - set(value)
            require(not missing, f"{where} has missing keys {sorted(missing)}")
            for key in value.keys() & properties.keys():
                self.validate(properties[key], value[key], f"{where}.{key}", current_id, stack)
            extra = set(value) - set(properties)
            additional = schema.get("additionalProperties", True)
            require(additional is not False or not extra, f"{where} has extra keys {sorted(extra)}")
            if isinstance(additional, dict):
                for key in extra:
                    self.validate(additional, value[key], f"{where}.{key}", current_id, stack)
        if isinstance(value, list):
            valid = len(value) >= schema.get("minItems", 0)
            valid = valid and len(value) <= schema.get("maxItems", len(value))
            require(valid, f"{where} has invalid array length")
            prefix = schema.get("prefixItems", ())
            for index, item in enumerate(value[: len(prefix)]):
                self.validate(prefix[index], item, f"{where}[{index}]", current_id, stack)
            items = schema.get("items", True)
            require(
                items is not False or len(value) <= len(prefix),
                f"{where} has forbidden trailing items",
            )
            if isinstance(items, dict):
                for index, item in enumerate(value[len(prefix) :], len(prefix)):
                    self.validate(items, item, f"{where}[{index}]", current_id, stack)


def validate_schema_for_test(schema, instance, exact_store):
    schemas = _Schemas(exact_store)
    schemas.audit()
    identifier = schema.get("$id") if isinstance(schema, dict) else None
    if not isinstance(identifier, str) or identifier not in exact_store:
        _schema_error("public schema is absent from the exact store")
    if _marker(exact_store[identifier]) != _marker(schema):
        _schema_error("public schema differs from its exact store entry")
    schemas.validate(schema, instance, "instance", identifier)


_SCHEMA_DIR = Path(__file__).resolve().parent / "schema"
PROBE_SCHEMA_DOCUMENT = load_json(_SCHEMA_DIR / "real_differential_probe_v1.schema.json")
CONTROLLER_SCHEMA_DOCUMENT = load_json(_SCHEMA_DIR / "real_differential_controller_v1.schema.json")
SCHEMA_STORE = {
    PROBE_SCHEMA_DOCUMENT["$id"]: PROBE_SCHEMA_DOCUMENT,
    CONTROLLER_SCHEMA_DOCUMENT["$id"]: CONTROLLER_SCHEMA_DOCUMENT,
}
_SCHEMAS = _Schemas(SCHEMA_STORE)
_SCHEMAS.audit()


def schema_ref(ref: str, current_id: str | None = None) -> dict[str, Any]:
    document_id = current_id or PROBE_SCHEMA_DOCUMENT["$id"]
    target, _target_id, _fragment = _SCHEMAS.resolve(ref, document_id)
    if not isinstance(target, dict):
        _schema_error(f"$ref target is not an object {ref!r}")
    return target


def validate_schema_fragment(ref: str, value: Any, where: str) -> None:
    target, target_id, fragment = _SCHEMAS.resolve(ref, PROBE_SCHEMA_DOCUMENT["$id"])
    _SCHEMAS.validate(target, value, where, target_id, ((target_id, fragment),))


def validate_probe_document(value: Any, where: str) -> None:
    identifier = PROBE_SCHEMA_DOCUMENT["$id"]
    _SCHEMAS.validate(PROBE_SCHEMA_DOCUMENT, value, where, identifier)


def validate_controller_document(value: Any) -> None:
    identifier = CONTROLLER_SCHEMA_DOCUMENT["$id"]
    _SCHEMAS.validate(CONTROLLER_SCHEMA_DOCUMENT, value, "controller summary", identifier)


def hex_bytes(value: str, where: str) -> bytes:
    if not isinstance(value, str) or re.fullmatch(r"(?:[0-9a-f]{2})+", value) is None:
        raise DifferentialContractError(f"{where} is not lowercase non-empty even hex")
    return bytes.fromhex(value)


def wire_number(value: Any, where: str) -> float:
    record = mapping(value, where)
    kind = record.get("kind")
    if kind in {"pos_inf", "neg_inf"}:
        require(set(record) == {"kind"}, f"{where} infinite wrapper keys differ")
        return math.inf if kind == "pos_inf" else -math.inf
    require(
        set(record) == {"kind", "value", "negative_zero"} and kind == "finite",
        f"{where} finite wrapper keys differ",
    )
    number = record["value"]
    negative = record["negative_zero"]
    require(
        type(number) in (int, float) and type(negative) is bool, f"{where} wrapper types differ"
    )
    require(type(number) is int or math.isfinite(number), f"{where} is not finite")
    require(not negative or number == 0.0, f"{where} has negative-zero on nonzero")
    require(
        not (number == 0.0 and math.copysign(1.0, number) < 0),
        f"{where} stores raw signed zero",
    )
    return math.copysign(0.0, -1.0) if negative else float(number)


def canonical_numbers(value: Any, where: str) -> None:
    if isinstance(value, dict):
        if set(value) in ({"kind"}, {"kind", "value", "negative_zero"}):
            wire_number(value, where)
        for key, item in value.items():
            canonical_numbers(item, f"{where}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            canonical_numbers(item, f"{where}[{index}]")


_require = require
_mapping = mapping
_canonical_numbers = canonical_numbers


_NODE_FIELDS = ("kind", "full_name", "instance_name", "pin_name")
_ARC_FIELDS = ("kind", "owner_name", "library_source_port", "library_sink_port")
_SIGNATURE_FIELDS = ("sense", "trigger_transition", "check_transition", "sdf_condition")
_TABLE_VARIABLES = {"none", "input_transition", "output_capacitance", "constrained_transition"}
_TRANSITIONS = {"rise", "fall", "none"}


def _packed_string(value):
    encoded = value.encode("utf-8", errors="strict")
    return len(encoded).to_bytes(8, "big") + encoded


def _packed_fields(record, names):
    return b"".join(_packed_string(record[name]) for name in names)


def _node_bytes(key):
    instance, pin = key["instance_name"], key["pin_name"]
    valid = bool(key["full_name"]) and (
        (key["kind"] == "port" and not instance and not pin)
        or (
            key["kind"] == "instance_pin"
            and bool(instance and pin)
            and key["full_name"] == f"{instance}:{pin}"
        )
    )
    _require(valid, "NeutralWire contains a non-canonical NodeKey")
    return _packed_fields(key, _NODE_FIELDS)


def _table_bytes(table):
    variables = table["variable1"], table["variable2"]
    valid = all(item in _TABLE_VARIABLES for item in variables)
    _require(valid, "ArcKey table variable is unknown")
    valid = table["present"] or variables == ("none", "none")
    _require(valid, "absent ArcKey table retains variables")
    valid = variables[0] != "none" or variables[1] == "none"
    _require(valid, "ArcKey table has variable2 without variable1")
    return bytes((int(table["present"]),)) + b"".join(map(_packed_string, variables))


def _signature_bytes(signature):
    senses = {"none_positive_fallback", "positive_unate", "negative_unate", "non_unate"}
    _require(signature["sense"] in senses, "ArcKey timing sense is unknown")
    transitions = signature["trigger_transition"], signature["check_transition"]
    _require(all(item in _TRANSITIONS for item in transitions), "ArcKey transition is unknown")
    result = _packed_string(signature["library_name"])
    result += signature["arc_index"].to_bytes(4, "big", signed=True)
    result += _packed_fields(signature, _SIGNATURE_FIELDS)
    tables = *signature["delay_tables"], *signature["slew_tables"]
    return result + b"".join(map(_table_bytes, tables))


def _arc_bytes(key):
    library = key["library_source_port"], key["library_sink_port"]
    signatures = key["ordered_library_arc_semantic_signature"]
    _require(bool(key["owner_name"]), "ArcKey owner_name is empty")
    _require(
        key["kind"] != "net" or (not any(library) and not signatures),
        "net ArcKey contains library identity",
    )
    _require(key["kind"] != "cell" or all(library), "cell ArcKey lacks library ports")
    result = _node_bytes(key["source"]) + _node_bytes(key["sink"])
    result += _packed_fields(key, _ARC_FIELDS)
    result += len(signatures).to_bytes(8, "big")
    result += b"".join(map(_signature_bytes, signatures))
    return result + key["parallel_ordinal"].to_bytes(8, "big")


def _key_id(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def wire_leaves(value):
    if isinstance(value, list):
        for item in value:
            yield from wire_leaves(item)
    else:
        yield value


def _resolve_predecessor(current, record, nodes, arcs, keys, where):
    predecessor, predecessor_arc = record["predecessor_node"], record["predecessor_arc"]
    valid = (predecessor is None) == (predecessor_arc is None)
    _require(valid, f"{where} predecessor node/Arc presence differs")
    if predecessor is None:
        return
    arc = arcs.get(_key_id(predecessor_arc), -1)
    valid = _key_id(predecessor) in nodes and arc >= 0
    valid = valid and keys[arc]["source"] == predecessor and keys[arc]["sink"] == current
    _require(valid, f"{where} predecessor does not resolve to its Arc endpoints")


def _validate_timing(value, where):
    validate_schema_fragment("#/$defs/neutralWire", value, where)
    timing = _mapping(value, where)
    valid = bool(timing["nodes"] and timing["arcs"] and timing["timing_order"])
    _require(valid, f"{where} must be non-empty")
    _canonical_numbers(timing, where)
    nodes, arcs = timing["nodes"], timing["arcs"]
    node_keys, arc_keys = ([item["key"] for item in items] for items in (nodes, arcs))
    node_bytes = [_node_bytes(key) for key in node_keys]
    arc_bytes = [_arc_bytes(key) for key in arc_keys]
    valid = node_bytes == sorted(set(node_bytes)) and arc_bytes == sorted(set(arc_bytes))
    _require(valid, f"{where} semantic keys are not strict canonical order")
    node_ids = {_key_id(key): index for index, key in enumerate(node_keys)}
    arc_ids = {_key_id(key): index for index, key in enumerate(arc_keys)}
    order_ids = [_key_id(key) for key in timing["timing_order"]]
    valid = len(node_ids) == len(node_keys) == len(order_ids) == len(set(order_ids))
    valid = valid and len(arc_ids) == len(arc_keys)
    valid = valid and len(timing["out_rows"]) == len(timing["in_rows"]) == len(node_keys)
    valid = valid and set(order_ids) == set(node_ids)
    _require(valid, f"{where} row/order cardinality differs")
    levels = [nodes[node_ids[item]]["level"] for item in order_ids]
    valid = all(level > 0 for level in levels) and levels == sorted(levels)
    _require(valid, f"{where} timing order/levels differ")
    counts = ([0] * len(arc_keys), [0] * len(arc_keys))
    for index, rows in enumerate(zip(timing["out_rows"], timing["in_rows"], strict=True)):
        valid = all(row["node"] == node_keys[index] for row in rows)
        _require(valid, f"{where} rows are not in canonical node order")
        for row, owned, endpoint in zip(rows, counts, ("source", "sink"), strict=True):
            for key in row["arcs"]:
                arc = arc_ids.get(_key_id(key), -1)
                valid = arc >= 0 and arc_keys[arc][endpoint] == node_keys[index]
                _require(valid, f"{where} adjacency key does not resolve")
                owned[arc] += 1
    valid = all(count == 1 for count in (*counts[0], *counts[1]))
    _require(valid, f"{where} adjacency does not own every Arc once")
    for node in nodes:
        key = node["key"]
        _resolve_predecessor(key, node, node_ids, arc_ids, arc_keys, f"{where}.node")
        for state_name in ("effective_clock", "physical_clock"):
            state = node[state_name]
            valid = all(
                item is None or _key_id(item) in node_ids
                for item in wire_leaves(state["predecessor_nodes"])
            )
            _require(valid, f"{where}.{state_name} predecessor does not resolve")
            valid = all(
                item is None or item in _TRANSITIONS
                for item in wire_leaves(state["predecessor_transitions"])
            )
            _require(valid, f"{where}.{state_name} transition is unknown")
        order = []
        for path in node["paths"]:
            names = ("state_transition", "predecessor_transition", "crpr_transition")
            valid = path["transition"] in {"rise", "fall"}
            valid = valid and all(path[name] in _TRANSITIONS for name in names)
            _require(valid, f"{where}.path transition is unknown")
            crpr = path["crpr_node"]
            valid = _key_id(path["start_node"]) in node_ids
            valid = valid and (crpr is None or _key_id(crpr) in node_ids)
            _require(valid, f"{where}.path start/crpr node does not resolve")
            _resolve_predecessor(key, path, node_ids, arc_ids, arc_keys, f"{where}.path")
            names = ("analysis", "path_source", "transition", "clock_tag")
            order.append(tuple(path[name] for name in names))
        valid = all(left < right for left, right in zip(order, order[1:], strict=False))
        _require(valid, f"{where} paths are not strict canonical order")
    for arc in arcs:
        valid = all(item is None or item in _TRANSITIONS for item in arc["transition_types"])
        _require(valid, f"{where} Arc transition type is unknown")
        indices = [entry["timing_arc_index"] for entry in arc["timing_arcs"]]
        valid = all(left < right for left, right in zip(indices, indices[1:], strict=False))
        _require(valid, f"{where} timing Arc indices are not strict")
    return timing


node_key_bytes = _node_bytes
arc_key_bytes = _arc_bytes
key_id = _key_id
validate_neutral_wire = _validate_timing

CHECKPOINTS = model.CHECKPOINTS
CONTROLLER_SCHEMA = model.CONTROLLER_SCHEMA
FINGERPRINT_KEYS = model.FINGERPRINT_KEYS
NATIVE_BACKED = model.NATIVE_BACKED
NATIVE_FATAL = model.NATIVE_FATAL
NOT_CLAIMED = model.NOT_CLAIMED
POISON_FAMILIES = model.POISON_FAMILIES
PROBE_SCHEMA = model.PROBE_SCHEMA
SYNTHETIC_ONLY = model.SYNTHETIC_ONLY
ControllerRun = model.ControllerRun
CaseDescriptor = model.CaseDescriptor
LaneSlot = model.LaneSlot
ManifestBundle = model.ManifestBundle
PairSlot = model.PairSlot
canonical_clock_json = model.canonical_clock_json
load_manifest = model.load_manifest
