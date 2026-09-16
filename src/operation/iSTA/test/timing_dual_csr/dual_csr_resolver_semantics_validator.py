#!/usr/bin/env python3
"""Semantic closure for stable refs, native resolver names, and edits."""

import copy
import json
from typing import Any, Callable

Require = Callable[[bool, str], None]


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _materializations(resolver: dict[str, Any], label: str,
                      require: Require
                      ) -> dict[str, tuple[dict[str, Any], dict[str, Any]]]:
    result: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {}
    for index, record in enumerate(resolver["materializations"]):
        token = _canonical(record["key"])
        require(token not in result,
                f"{label}.materializations[{index}] repeats a token")
        result[token] = (record["key"], record["value"])
    return result


def _resolve_node(ref: dict[str, Any], materializations: dict[str, tuple[
                  dict[str, Any], dict[str, Any]]], label: str,
                  require: Require) -> str:
    if ref["kind"] == "existing":
        return ref["baseline_key"]["full_name"]
    token = _canonical(ref["creation_token"])
    require(token in materializations, f"{label} has no materialization")
    value = materializations[token][1]
    field = {"input": "input_pin_name", "output": "output_pin_name"}.get(
        ref["role"])
    require(field is not None, f"{label} has an invalid created-node role")
    return value[field]


def _resolve_net(ref: dict[str, Any], materializations: dict[str, tuple[
                 dict[str, Any], dict[str, Any]]], label: str,
                 require: Require) -> str:
    if ref["kind"] == "existing":
        return ref["baseline_name"]
    token = _canonical(ref["creation_token"])
    require(token in materializations and ref["role"] == "output_net",
            f"{label} has no output-net materialization")
    return materializations[token][1]["output_net_name"]


def _semantic_edits(semantic_trace: list[dict[str, Any]], label: str,
                    require: Require) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for batch_index, batch in enumerate(semantic_trace):
        for edit_index, edit in enumerate(batch["edits"]):
            token = _canonical(edit["creation_token"])
            require(token not in result,
                    f"{label}.semantic_trace[{batch_index}].edits[{edit_index}] "
                    "repeats a creation token")
            result[token] = edit
    return result


def _functional_maps(functional: dict[str, Any], label: str,
                     require: Require
                     ) -> tuple[dict[str, dict[str, Any]],
                                dict[str, dict[str, Any]], set[str]]:
    graph = functional["graph"]
    nodes: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(graph["nodes"]):
        name = record["key"]["full_name"]
        require(name not in nodes,
                f"{label}.functional.nodes[{index}] repeats a name")
        nodes[name] = record
    instances: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(graph["reference_payload"]["instances"]):
        name = record["instance_name"]
        require(name not in instances,
                f"{label}.functional.instances[{index}] repeats a name")
        instances[name] = record
    nets = {record["net_name"] for record in graph["nodes"]
            if record["net_name"]}
    return nodes, instances, nets


def _indexed_arc_keys(structural: dict[str, Any], label: str,
                      require: Require) -> dict[int, dict[str, Any]]:
    historical: dict[int, dict[str, Any]] = {}
    for index, record in enumerate(structural["inactive_arcs"]):
        arc_id = record["id"]
        require(arc_id not in historical,
                f"{label}.inactive_arcs[{index}] repeats an ArcId")
        historical[arc_id] = record["historical_payload"]["key"]
    result: dict[int, dict[str, Any]] = {}
    for index, slot in enumerate(structural["arc_group_index"]["slots"]):
        arc_id = slot["arc_id"]
        require(arc_id not in result,
                f"{label}.arc_group_index.slots[{index}] repeats an ArcId")
        key = slot["current_active_key"]
        if key is None:
            key = historical.get(arc_id)
        require(key is not None,
                f"{label}.arc_group_index.slots[{index}] has no current/history key")
        if key is not None:
            result[arc_id] = key
    return result


def _assign_creation_ordinals(refs: list[dict[str, Any]]) -> None:
    """Assign immutable ordinals from creation-semantic encounter order."""
    counts: dict[str, int] = {}
    for ref in refs:
        field = ("stable_equivalence_ordinal"
                 if ref["kind"] == "created_cell" else "stable_ordinal")
        base = copy.deepcopy(ref)
        base.pop(field)
        encoded = _canonical(base)
        ordinal = counts.get(encoded, 0)
        ref[field] = ordinal
        counts[encoded] = ordinal + 1


def _expected_created_arc_refs(
        resolver: dict[str, Any], edits: dict[str, dict[str, Any]],
        materializations: dict[str, tuple[dict[str, Any], dict[str, Any]]],
        slot_keys: dict[int, dict[str, Any]], label: str,
        require: Require) -> list[dict[str, Any]]:
    node_refs = {record["binding"]["native_name"]: record["ref"]
                 for record in resolver["node_bindings"]}
    created = [record for record in resolver["arc_bindings"]
               if record["ref"]["kind"] != "existing"]
    expected: list[dict[str, Any]] = []
    for token, edit in edits.items():
        token_value, _ = materializations[token]
        library = edit["library_cell"]
        expected.append({
            "kind": "created_cell", "creation_token": token_value,
            "library_source_port": library["input_port"],
            "library_sink_port": library["output_port"],
            "ordered_library_arcs": library["ordered_arc_signatures"],
            "stable_equivalence_ordinal": 0})
        input_ref = {"kind": "created", "creation_token": token_value,
                     "role": "input"}
        old = [record for record in created
               if record["ref"]["kind"] == "created_old_net"
               and record["ref"]["net"] == edit["original_net"]
               and record["ref"]["sink"] == input_ref]
        require(len(old) == 1,
                f"{label}.semantic_edit[{token}] old-net arc is not unique")
        if len(old) == 1:
            slot = old[0]["binding"]["slot"]
            key = slot_keys.get(slot)
            require(key is not None,
                    f"{label}.semantic_edit[{token}] old-net ArcId is absent")
            driver = None if key is None else node_refs.get(
                key["source"]["full_name"])
            require(driver is not None,
                    f"{label}.semantic_edit[{token}] old-net driver is absent")
            if driver is not None:
                expected.append({
                    "kind": "created_old_net", "net": edit["original_net"],
                    "driver": driver, "sink": input_ref,
                    "stable_ordinal": 0})
        output_ref = {"kind": "created", "creation_token": token_value,
                      "role": "output"}
        for sink in edit["moved_sinks"]:
            expected.append({
                "kind": "created_new_net", "creation_token": token_value,
                "driver": output_ref, "sink": sink, "stable_ordinal": 0})
    _assign_creation_ordinals(expected)
    return expected


def validate_resolver_semantics(resolver: dict[str, Any],
                                functional: dict[str, Any],
                                structural: dict[str, Any],
                                baseline_reference_input: dict[str, Any],
                                semantic_trace: list[dict[str, Any]],
                                label: str, require: Require) -> None:
    materializations = _materializations(resolver, label, require)
    edits = _semantic_edits(semantic_trace, label, require)
    require(set(materializations) == set(edits),
            f"{label} materialization tokens differ from semantic edits")
    nodes, instances, nets = _functional_maps(functional, label, require)

    node_names: set[str] = set()
    for index, record in enumerate(resolver["node_bindings"]):
        item_label = f"{label}.node_bindings[{index}]"
        expected = _resolve_node(record["ref"], materializations,
                                 f"{item_label}.ref", require)
        binding = record["binding"]
        require(binding["native_name"] == expected
                and not binding["tombstone"],
                f"{item_label}.binding does not match its stable ref")
        require(expected not in node_names and expected in nodes,
                f"{item_label} is not a unique functional node")
        node_names.add(expected)
        key = nodes[expected]["key"]
        if record["ref"]["kind"] == "existing":
            require(key == record["ref"]["baseline_key"],
                    f"{item_label} existing NodeKey differs")
    require(node_names == set(nodes),
            f"{label}.node_bindings do not cover functional nodes")

    net_names: set[str] = set()
    for index, record in enumerate(resolver["net_bindings"]):
        item_label = f"{label}.net_bindings[{index}]"
        expected = _resolve_net(record["ref"], materializations,
                                f"{item_label}.ref", require)
        binding = record["binding"]
        require(binding["native_name"] == expected
                and not binding["tombstone"],
                f"{item_label}.binding does not match its stable ref")
        require(expected not in net_names,
                f"{item_label} repeats a live net name")
        net_names.add(expected)
    baseline_nets = set(semantic_trace[0]["baseline"]["canonical_pre_state"]["nets"])
    created_nets = {value["output_net_name"] for _, value in materializations.values()}
    require(net_names == baseline_nets | created_nets and nets <= net_names,
            f"{label}.net_bindings differ from baseline and created nets or omit functional nodes")

    materialized_names: set[str] = set()
    final_sink_nets: dict[str, str] = {}
    for token, (_, value) in materializations.items():
        edit = edits[token]
        fields = ("instance_name", "output_net_name", "input_pin_name",
                  "output_pin_name")
        names = [value[field] for field in fields]
        require(all(isinstance(name, str) and name for name in names)
                and len(set(names)) == len(names),
                f"{label}.materializations[{token}] has colliding names")
        require(not materialized_names.intersection(names),
                f"{label}.materializations[{token}] reuses a created name")
        materialized_names.update(names)
        library = edit["library_cell"]
        require(value["input_pin_name"]
                == f'{value["instance_name"]}:{library["input_port"]}'
                and value["output_pin_name"]
                == f'{value["instance_name"]}:{library["output_port"]}',
                f"{label}.materializations[{token}] pin names differ from edit")
        require(value["instance_name"] in instances,
                f"{label}.materializations[{token}] instance is absent")
        instance = instances[value["instance_name"]]
        require(instance["cell_name"] == library["cell_name"]
                and {pin["full_name"] for pin in instance["ordered_pins"]}
                == {value["input_pin_name"], value["output_pin_name"]},
                f"{label}.materializations[{token}] instance differs")
        require(value["input_pin_name"] in nodes
                and value["output_pin_name"] in nodes
                and value["output_net_name"] in nets,
                f"{label}.materializations[{token}] functional names are absent")
        input_key = nodes[value["input_pin_name"]]["key"]
        output_key = nodes[value["output_pin_name"]]["key"]
        require(input_key["instance_name"] == value["instance_name"]
                and input_key["pin_name"] == library["input_port"]
                and output_key["instance_name"] == value["instance_name"]
                and output_key["pin_name"] == library["output_port"],
                f"{label}.materializations[{token}] functional pins differ")
        original_net = _resolve_net(
            edit["original_net"], materializations,
            f"{label}.semantic_edit[{token}].original_net", require)
        require(nodes[value["input_pin_name"]]["net_name"] == original_net
                and nodes[value["output_pin_name"]]["net_name"]
                == value["output_net_name"],
                f"{label}.materializations[{token}] pin nets differ")
        for sink_index, sink_ref in enumerate(edit["moved_sinks"]):
            sink = _resolve_node(
                sink_ref, materializations,
                f"{label}.semantic_edit[{token}].moved_sinks[{sink_index}]",
                require)
            require(sink in nodes, f"{label}.semantic_edit[{token}] sink is absent")
            final_sink_nets[sink] = value["output_net_name"]
    for sink, net_name in final_sink_nets.items():
        require(nodes[sink]["net_name"] == net_name,
                f"{label} final moved-sink net differs")
    slot_keys = _indexed_arc_keys(structural, label, require)
    expected_created = _expected_created_arc_refs(
        resolver, edits, materializations, slot_keys, label, require)
    actual_created = [record["ref"] for record in resolver["arc_bindings"]
                      if record["ref"]["kind"] != "existing"]
    require(sorted(map(_canonical, actual_created))
            == sorted(map(_canonical, expected_created)),
            f"{label}.arc_bindings created refs differ from semantic edits")
    baseline_arcs = baseline_reference_input["arcs"]

    for index, record in enumerate(resolver["arc_bindings"]):
        ref = record["ref"]
        item_label = f"{label}.arc_bindings[{index}].ref"
        kind = ref["kind"]
        require(record["binding"]["native_name"] == "",
                f"{item_label} native name must be empty")
        slot = record["binding"]["slot"]
        key = slot_keys.get(slot)
        require(key is not None, f"{item_label} has no structural ArcId")
        if key is None:
            continue
        if kind == "existing":
            require(slot < len(baseline_arcs),
                    f"{item_label} has no generation-zero ArcId")
            if slot >= len(baseline_arcs):
                continue
            require(ref["baseline_key"] == baseline_arcs[slot]["key"],
                    f"{item_label} differs from its generation-zero ArcKey")
            base = lambda value: {name: field for name, field in value.items()
                                  if name != "parallel_ordinal"}
            require(base(key) == base(ref["baseline_key"]),
                    f"{item_label} current/history ArcKey base differs")
            continue
        if kind == "created_cell":
            token = _canonical(ref["creation_token"])
            require(token in edits and token in materializations,
                    f"{item_label} has no semantic edit/materialization")
            if token not in edits or token not in materializations:
                continue
            value = materializations[token][1]
            require(key["source"]["full_name"] == value["input_pin_name"]
                    and key["sink"]["full_name"] == value["output_pin_name"]
                    and key["owner_name"] == value["instance_name"]
                    and key["kind"] == "cell"
                    and ref["library_source_port"]
                    == edits[token]["library_cell"]["input_port"]
                    and ref["library_sink_port"]
                    == edits[token]["library_cell"]["output_port"]
                    and ref["ordered_library_arcs"]
                    == edits[token]["library_cell"]["ordered_arc_signatures"]
                    and key["library_source_port"] == ref["library_source_port"]
                    and key["library_sink_port"] == ref["library_sink_port"]
                    and key["library_arcs"] == ref["ordered_library_arcs"],
                    f"{item_label} differs from its edit/index ArcKey")
        elif kind == "created_old_net":
            net = _resolve_net(ref["net"], materializations,
                               f"{item_label}.net", require)
            driver = _resolve_node(ref["driver"], materializations,
                                   f"{item_label}.driver", require)
            sink = _resolve_node(ref["sink"], materializations,
                                 f"{item_label}.sink", require)
            require(key["source"]["full_name"] == driver
                    and key["sink"]["full_name"] == sink
                    and key["owner_name"] == net and key["kind"] == "net"
                    and key["library_source_port"] == ""
                    and key["library_sink_port"] == ""
                    and key["library_arcs"] == [],
                    f"{item_label} differs from the indexed old-net ArcKey")
        elif kind == "created_new_net":
            token = _canonical(ref["creation_token"])
            require(token in edits and token in materializations,
                    f"{item_label} has no semantic edit/materialization")
            if token not in edits or token not in materializations:
                continue
            driver = _resolve_node(ref["driver"], materializations,
                                   f"{item_label}.driver", require)
            sink = _resolve_node(ref["sink"], materializations,
                                 f"{item_label}.sink", require)
            require(key["source"]["full_name"] == driver
                    and key["sink"]["full_name"] == sink
                    and key["owner_name"]
                    == materializations[token][1]["output_net_name"]
                    and key["kind"] == "net" and key["library_arcs"] == []
                    and key["library_source_port"] == ""
                    and key["library_sink_port"] == "",
                    f"{item_label} differs from the indexed new-net ArcKey")


def run_resolver_semantics_self_test(require: Require) -> None:
    node = {"kind": "port", "full_name": "p", "instance_name": "",
            "pin_name": ""}
    arc = {"source": node, "sink": node, "kind": "net",
           "owner_name": "n", "library_source_port": "",
           "library_sink_port": "", "library_arcs": [],
           "parallel_ordinal": 0}
    baseline_arc = copy.deepcopy(arc)
    baseline_arc["parallel_ordinal"] = 1
    resolver = {"materializations": [], "arc_bindings": [{
        "ref": {"kind": "existing", "baseline_key": baseline_arc},
        "binding": {"slot": 0, "native_name": "", "tombstone": False}}],
        "node_bindings": [{"ref": {"kind": "existing",
            "baseline_key": node}, "binding": {"slot": 0,
            "native_name": "p", "tombstone": False}}],
        "net_bindings": [{"ref": {"kind": "existing",
            "baseline_name": "n"}, "binding": {"slot": 0,
            "native_name": "n", "tombstone": False}}]}
    functional = {"graph": {"nodes": [{"key": node, "net_name": "n"}],
        "reference_payload": {"instances": []}}}
    structural = {"inactive_arcs": [], "arc_group_index": {"slots": [{
        "arc_id": 0, "ref": {"kind": "existing", "baseline_key": baseline_arc},
        "current_active_key": arc}]}}
    baseline = {"arcs": [{"key": baseline_arc}]}
    baseline_trace = [{"baseline": {"canonical_pre_state": {"nets": ["n"]}}, "edits": []}]
    errors: list[str] = []
    validate_resolver_semantics(
        resolver, functional, structural, baseline, baseline_trace, "valid-resolver-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors, f"valid resolver semantics was rejected: {errors}")
    orphan_resolver = copy.deepcopy(resolver)
    orphan_resolver["net_bindings"].append({
        "ref": {"kind": "existing", "baseline_name": "orphan"},
        "binding": {"slot": 1, "native_name": "orphan", "tombstone": False}})
    orphan_trace = copy.deepcopy(baseline_trace)
    orphan_trace[0]["baseline"]["canonical_pre_state"]["nets"].append("orphan")
    errors = []
    validate_resolver_semantics(
        orphan_resolver, functional, structural, baseline, orphan_trace, "orphan-net",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors, f"unreferenced baseline net was rejected: {errors}")
    orphan_resolver["net_bindings"][-1]["ref"]["baseline_name"] = "forged"
    orphan_resolver["net_bindings"][-1]["binding"]["native_name"] = "forged"
    errors = []
    validate_resolver_semantics(
        orphan_resolver, functional, structural, baseline, orphan_trace, "forged-orphan-net",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "coordinated unreferenced net rename escaped baseline identity")
    tampered = copy.deepcopy(resolver)
    tampered["net_bindings"][0]["binding"]["native_name"] = "WRONG_NET"
    errors = []
    validate_resolver_semantics(
        tampered, functional, structural, baseline, baseline_trace, "resolver-native-name-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "resolver native-name tamper was accepted")
    tampered = copy.deepcopy(resolver)
    tampered["arc_bindings"][0]["binding"]["native_name"] = "WRONG_ARC"
    errors = []
    validate_resolver_semantics(
        tampered, functional, structural, baseline, baseline_trace, "arc-native-name-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "arc binding native-name tamper was accepted")

    token = {"schema_version": 1, "case_id": "canary",
             "profile_id": "smoke", "seed": 7, "batch_ordinal": 1,
             "accepted_ordinal": 0}
    driver = {"kind": "port", "full_name": "d", "instance_name": "",
              "pin_name": ""}
    sink = {"kind": "instance_pin", "full_name": "u:A",
            "instance_name": "u", "pin_name": "A"}
    input_key = {"kind": "instance_pin", "full_name": "b:A",
                 "instance_name": "b", "pin_name": "A"}
    output_key = {"kind": "instance_pin", "full_name": "b:Y",
                  "instance_name": "b", "pin_name": "Y"}
    driver_ref = {"kind": "existing", "baseline_key": driver}
    sink_ref = {"kind": "existing", "baseline_key": sink}
    input_ref = {"kind": "created", "creation_token": token,
                 "role": "input"}
    output_ref = {"kind": "created", "creation_token": token,
                  "role": "output"}
    old_net = {"kind": "existing", "baseline_name": "n"}
    new_net = {"kind": "created", "creation_token": token,
               "role": "output_net"}
    library = {"cell_name": "BUF", "input_port": "A",
               "output_port": "Y", "ordered_arc_signatures": []}
    edit = {"creation_token": token, "library_cell": library,
            "original_net": old_net, "moved_sinks": [sink_ref]}
    materialized = {"creation_token": token, "instance_name": "b",
                    "output_net_name": "bn", "input_pin_name": "b:A",
                    "output_pin_name": "b:Y"}
    cell_ref = {"kind": "created_cell", "creation_token": token,
                "library_source_port": "A", "library_sink_port": "Y",
                "ordered_library_arcs": [],
                "stable_equivalence_ordinal": 0}
    old_arc_ref = {"kind": "created_old_net", "net": old_net,
                   "driver": driver_ref, "sink": input_ref,
                   "stable_ordinal": 0}
    new_arc_ref = {"kind": "created_new_net", "creation_token": token,
                   "driver": output_ref, "sink": sink_ref,
                   "stable_ordinal": 0}
    def arc_key(source: dict[str, Any], target: dict[str, Any], kind: str,
                owner: str) -> dict[str, Any]:
        return {"source": source, "sink": target, "kind": kind,
                "owner_name": owner, "library_source_port": "A" if kind
                == "cell" else "", "library_sink_port": "Y" if kind
                == "cell" else "", "library_arcs": [],
                "parallel_ordinal": 0}
    cell_key = arc_key(input_key, output_key, "cell", "b")
    old_key = arc_key(driver, input_key, "net", "n")
    new_key = arc_key(output_key, sink, "net", "bn")
    created_resolver = {
        "materializations": [{"key": token, "value": materialized}],
        "node_bindings": [
            {"ref": driver_ref, "binding": {"slot": 0,
             "native_name": "d", "tombstone": False}},
            {"ref": sink_ref, "binding": {"slot": 1,
             "native_name": "u:A", "tombstone": False}},
            {"ref": input_ref, "binding": {"slot": 2,
             "native_name": "b:A", "tombstone": False}},
            {"ref": output_ref, "binding": {"slot": 3,
             "native_name": "b:Y", "tombstone": False}}],
        "net_bindings": [
            {"ref": old_net, "binding": {"slot": 0,
             "native_name": "n", "tombstone": False}},
            {"ref": new_net, "binding": {"slot": 1,
             "native_name": "bn", "tombstone": False}}],
        "arc_bindings": [{"ref": ref, "binding": {"slot": slot,
            "native_name": "", "tombstone": False}}
            for slot, ref in enumerate((cell_ref, old_arc_ref, new_arc_ref))]}
    created_functional = {"graph": {"nodes": [
        {"key": driver, "net_name": "n"},
        {"key": sink, "net_name": "bn"},
        {"key": input_key, "net_name": "n"},
        {"key": output_key, "net_name": "bn"}],
        "reference_payload": {"instances": [{"instance_name": "b",
            "cell_name": "BUF", "ordered_pins": [input_key, output_key]}]}}}
    created_structural = {"inactive_arcs": [], "arc_group_index": {"slots": [
        {"arc_id": 0, "ref": cell_ref, "current_active_key": cell_key},
        {"arc_id": 1, "ref": old_arc_ref, "current_active_key": old_key},
        {"arc_id": 2, "ref": new_arc_ref, "current_active_key": new_key}]}}
    errors = []
    validate_resolver_semantics(created_resolver, created_functional,
        created_structural, {"arcs": []}, [{**baseline_trace[0], "edits": [edit]}],
        "created-ref-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors, f"valid created refs were rejected: {errors}")
    survivor_resolver = copy.deepcopy(created_resolver)
    survivor_structural = copy.deepcopy(created_structural)
    survivor_edit = copy.deepcopy(edit)
    survivor_edit["moved_sinks"].append(copy.deepcopy(sink_ref))
    stable_one = copy.deepcopy(new_arc_ref)
    stable_one["stable_ordinal"] = 1
    survivor_resolver["arc_bindings"][2]["binding"]["tombstone"] = True
    survivor_resolver["arc_bindings"].append({
        "ref": stable_one, "binding": {"slot": 3, "native_name": "",
                                        "tombstone": False}})
    survivor_structural["arc_group_index"]["slots"][2][
        "current_active_key"] = None
    survivor_structural["arc_group_index"]["slots"].append({
        "arc_id": 3, "ref": stable_one,
        "current_active_key": copy.deepcopy(new_key)})
    survivor_structural["inactive_arcs"] = [{
        "id": 2, "historical_payload": {"key": copy.deepcopy(new_key)}}]
    errors = []
    validate_resolver_semantics(
        survivor_resolver, created_functional, survivor_structural,
        {"arcs": []}, [{**baseline_trace[0], "edits": [survivor_edit]}],
        "created-survivor-reordinal-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors,
            f"created survivor stable ordinal 1/current ordinal 0 was rejected: {errors}")
    tampered = copy.deepcopy(created_resolver)
    tampered["arc_bindings"][2]["ref"]["stable_ordinal"] = 1
    tampered_structural = copy.deepcopy(created_structural)
    tampered_structural["arc_group_index"]["slots"][2]["ref"][
        "stable_ordinal"] = 1
    tampered_structural["arc_group_index"]["slots"][2][
        "current_active_key"]["parallel_ordinal"] = 1
    errors = []
    validate_resolver_semantics(tampered, created_functional,
        tampered_structural, {"arcs": []}, [{**baseline_trace[0], "edits": [edit]}],
        "stable-ordinal-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(bool(errors), "coordinated created StableArcRef ordinal tamper was accepted")
