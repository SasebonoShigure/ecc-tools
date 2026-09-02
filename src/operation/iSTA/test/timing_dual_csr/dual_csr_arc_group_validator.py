"""Field-addressable validation for the Stage 5 ArcGroupIndex receipt."""

import json
from collections.abc import Callable
from typing import Any

Require = Callable[[bool, str], None]


def exact(value: object, keys: set[str], label: str,
          require: Require) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def count(value: object, label: str, require: Require) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value


def group_ids(groups: object, group_keys: set[str], active: set[int],
              label: str, require: Require) -> set[int]:
    require(isinstance(groups, list), f"{label} must be an array")
    flattened: list[int] = []
    for position, group in enumerate(groups):
        item = exact(group, group_keys, f"{label}[{position}]", require)
        ids = item["active_arc_ids"]
        require(isinstance(ids, list) and ids,
                f"{label}[{position}].active_arc_ids must be nonempty")
        require(all(type(arc_id) is int and arc_id in active for arc_id in ids),
                f"{label}[{position}] contains a non-active ArcId")
        require(len(set(ids)) == len(ids),
                f"{label}[{position}] repeats an ArcId")
        flattened.extend(ids)
    require(len(flattened) == len(set(flattened)),
            f"{label} repeats an ArcId between groups")
    return set(flattened)


def validate_arc_group_index(value: object, generation: int, arc_slots: int,
                             active: set[int], label: str,
                             require: Require) -> dict[str, Any]:
    keys = {
        "session_token", "generation", "arc_high_water", "slots",
        "active_groups", "active_sink_net_groups", "active_net_sink_groups",
    }
    item = exact(value, keys, label, require)
    require(count(item["session_token"], f"{label}.session_token", require) > 0,
            f"{label}.session_token must be nonzero")
    require(item["generation"] == generation, f"{label}.generation differs")
    require(item["arc_high_water"] == arc_slots,
            f"{label}.arc_high_water differs")
    slots = item["slots"]
    require(isinstance(slots, list) and len(slots) == arc_slots,
            f"{label}.slots size differs")
    for arc_id, slot in enumerate(slots):
        slot_item = exact(slot, {"arc_id", "ref", "current_active_key"},
                          f"{label}.slots[{arc_id}]", require)
        require(slot_item["arc_id"] == arc_id,
                f"{label}.slots[{arc_id}].arc_id differs")
        require(isinstance(slot_item["ref"], dict),
                f"{label}.slots[{arc_id}].ref must be semantic")
        key = slot_item["current_active_key"]
        require((arc_id in active) == isinstance(key, dict),
                f"{label}.slots[{arc_id}] active state differs")

    base_ids = group_ids(item["active_groups"],
                         {"base", "active_arc_ids"}, active,
                         f"{label}.active_groups", require)
    require(base_ids == active, f"{label}.active_groups coverage differs")
    bases: set[str] = set()
    for position, group in enumerate(item["active_groups"]):
        base = group["base"]
        require(isinstance(base, dict),
                f"{label}.active_groups[{position}].base must be semantic")
        signature = json.dumps(base, sort_keys=True, separators=(",", ":"))
        require(signature not in bases,
                f"{label}.active_groups repeats a semantic base")
        bases.add(signature)
        ordinals: list[int] = []
        for arc_id in group["active_arc_ids"]:
            key = dict(slots[arc_id]["current_active_key"])
            ordinal = key.pop("parallel_ordinal", None)
            require(type(ordinal) is int and ordinal >= 0 and key == base,
                    f"{label}.active_groups[{position}] key differs")
            ordinals.append(ordinal)
        require(ordinals == sorted(ordinals)
                and len(ordinals) == len(set(ordinals)),
                f"{label}.active_groups[{position}] ordinals differ")

    sink_ids = group_ids(
        item["active_sink_net_groups"],
        {"sink_net_name", "active_arc_ids"}, active,
        f"{label}.active_sink_net_groups", require,
    )
    require(sink_ids == active,
            f"{label}.active_sink_net_groups coverage differs")
    sink_names = [group["sink_net_name"]
                  for group in item["active_sink_net_groups"]]
    require(all(isinstance(name, str) for name in sink_names)
            and len(sink_names) == len(set(sink_names)),
            f"{label}.active_sink_net_groups keys differ")

    net_active = {arc_id for arc_id in active
                  if slots[arc_id]["current_active_key"].get("kind") == "net"}
    net_ids = group_ids(
        item["active_net_sink_groups"], {"key", "active_arc_ids"}, active,
        f"{label}.active_net_sink_groups", require,
    )
    require(net_ids == net_active,
            f"{label}.active_net_sink_groups coverage differs")
    for position, group in enumerate(item["active_net_sink_groups"]):
        group_key = exact(group["key"], {"owner_name", "sink"},
                          f"{label}.active_net_sink_groups[{position}].key",
                          require)
        for arc_id in group["active_arc_ids"]:
            key = slots[arc_id]["current_active_key"]
            require(key["owner_name"] == group_key["owner_name"]
                    and key["sink"] == group_key["sink"],
                    f"{label}.active_net_sink_groups[{position}] key differs")
    return item


def _signature(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _actual_groups(groups: list[dict[str, Any]], key_field: str,
                   label: str, require: Require) -> dict[str, list[int]]:
    result: dict[str, list[int]] = {}
    for position, group in enumerate(groups):
        key = group[key_field]
        signature = _signature(key)
        require(signature not in result,
                f"{label}[{position}] repeats/splits a semantic group")
        result[signature] = group["active_arc_ids"]
    return result


def validate_arc_group_semantics(value: object,
                                 functional_graph: dict[str, Any],
                                 label: str, require: Require) -> None:
    """Rebuild every group from full slot keys plus semantic node net names."""
    require(isinstance(value, dict) and isinstance(functional_graph, dict),
            f"{label} semantic inputs must be objects")
    nodes = functional_graph.get("nodes")
    require(isinstance(nodes, list), f"{label} functional nodes are absent")
    net_by_node: dict[str, str] = {}
    for position, record in enumerate(nodes):
        require(isinstance(record, dict)
                and isinstance(record.get("key"), dict)
                and isinstance(record.get("net_name"), str),
                f"{label}.functional.nodes[{position}] is malformed")
        signature = _signature(record["key"])
        require(signature not in net_by_node,
                f"{label} functional graph repeats a NodeKey")
        net_by_node[signature] = record["net_name"]

    expected_base: dict[str, list[tuple[int, int]]] = {}
    expected_sink: dict[str, list[int]] = {}
    expected_net_sink: dict[str, list[int]] = {}
    for slot in value["slots"]:
        key = slot["current_active_key"]
        if key is None:
            continue
        arc_id = slot["arc_id"]
        base = dict(key)
        ordinal = base.pop("parallel_ordinal")
        expected_base.setdefault(_signature(base), []).append((ordinal, arc_id))
        sink_signature = _signature(key["sink"])
        require(sink_signature in net_by_node,
                f"{label}.slots[{arc_id}] sink NodeKey is not published")
        sink_net = net_by_node[sink_signature]
        expected_sink.setdefault(_signature(sink_net), []).append(arc_id)
        if key["kind"] == "net":
            net_sink_key = {"owner_name": key["owner_name"],
                            "sink": key["sink"]}
            expected_net_sink.setdefault(
                _signature(net_sink_key), []).append(arc_id)

    expected_base_ids: dict[str, list[int]] = {}
    for signature, members in expected_base.items():
        members.sort()
        ordinals = [ordinal for ordinal, _ in members]
        require(ordinals == list(range(len(ordinals))),
                f"{label} base parallel ordinals are not contiguous")
        expected_base_ids[signature] = [arc_id for _, arc_id in members]
    for members in expected_sink.values():
        members.sort()
    for members in expected_net_sink.values():
        members.sort()

    actual_base = _actual_groups(value["active_groups"], "base",
                                 f"{label}.active_groups", require)
    actual_sink = _actual_groups(value["active_sink_net_groups"],
                                 "sink_net_name",
                                 f"{label}.active_sink_net_groups", require)
    actual_net_sink = _actual_groups(value["active_net_sink_groups"], "key",
                                     f"{label}.active_net_sink_groups", require)
    require(actual_base == expected_base_ids,
            f"{label}.active_groups differ from slot semantics")
    require(actual_sink == expected_sink,
            f"{label}.active_sink_net_groups differ from sink net semantics")
    require(actual_net_sink == expected_net_sink,
            f"{label}.active_net_sink_groups differ from net/sink semantics")
