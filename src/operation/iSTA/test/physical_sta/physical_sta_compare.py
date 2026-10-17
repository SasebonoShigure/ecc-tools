#!/usr/bin/env python3
"""Compare explicit ON physical readback with a separately run fresh native/full."""

import argparse
import copy
import json
import sys
from pathlib import Path

TEST = Path(__file__).resolve().parents[1]
for directory in ("timing_dual_csr", "real_differential"):
    sys.path.insert(0, str(TEST / directory))

from real_differential_wire_contract import validate_neutral_wire  # noqa: E402
from trace_process import (  # noqa: E402
    ABS_TOLERANCE,
    REL_TOLERANCE,
    first_difference,
    require,
    require_exact_keys,
)


def functional(value, generation):
    item = require_exact_keys(
        value,
        {
            "schema_version",
            "generation",
            "physical",
            "parasitics",
            "timing",
            "active_level_edges",
            "path_checks",
        },
        "physical diagnostic",
    )
    require(item["schema_version"] == "physical-sta-diagnostic/2.0.0", "diagnostic schema differs")
    require(
        type(item["generation"]) is int and item["generation"] == generation, "generation differs"
    )
    timing = validate_neutral_wire(item["timing"], "physical timing")
    levels = {node["key"]["full_name"]: node["level"] for node in timing["nodes"]}
    for source, sink in item["active_level_edges"]:
        require(
            source in levels and sink in levels and levels[source] < levels[sink],
            "active edge violates levels",
        )
    # Mixed and full may assign different legal levels. All other values and
    # identities, including every predecessor and CRPR identity, are compared.
    result = {
        key: item[key] for key in ("physical", "parasitics", "path_checks")
    }
    result["active_level_edges"] = sorted(item["active_level_edges"])
    result["timing"] = {key: timing[key] for key in ("nodes", "arcs", "out_rows", "in_rows")}
    result["timing"]["nodes"] = [
        {key: value for key, value in node.items() if key != "level"} for node in timing["nodes"]
    ]
    return result


def compare(target, fresh, generation):
    require(type(generation) is int and generation > 0, "a real committed generation is required")
    difference = first_difference(functional(target, generation), functional(fresh, 0))
    require(difference is None, f"physical/fresh mismatch: {difference}")
    return {
        "generation": generation,
        "node_count": len(target["timing"]["nodes"]),
        "arc_count": len(target["timing"]["arcs"]),
        "path_lane_count": sum(len(node["paths"]) for node in target["timing"]["nodes"]),
        "reported_path_count": sum(
            len(paths) for group in target["path_checks"].values() for paths in group.values()
        ),
        "physical_rc_timing_winners_checks_equal": True,
        "levels_valid": True,
        "absolute_tolerance": ABS_TOLERANCE,
        "relative_tolerance": REL_TOLERANCE,
    }


def canaries(target):
    fresh = dict(target, generation=0)
    compare(target, fresh, target["generation"])
    rejected = []
    for label in ("level", "winner", "crpr", "timing"):
        bad = copy.deepcopy(target)
        if label == "level":
            source, sink = bad["active_level_edges"][0]
            by_name = {node["key"]["full_name"]: node for node in bad["timing"]["nodes"]}
            by_name[sink]["level"] = by_name[source]["level"]
            del by_name
        elif label in ("winner", "crpr"):
            field = "predecessor_arc" if label == "winner" else "crpr_node"
            path = next(
                path
                for node in bad["timing"]["nodes"]
                for path in node["paths"]
                if path[field] is not None
            )
            path[field] = None
            del path
        else:
            node = next(
                node for node in bad["timing"]["nodes"] if node["arrival"]["kind"] == "finite"
            )
            node["arrival"]["value"] += 1.0
            node["arrival"]["negative_zero"] = False
            del node
        try:
            compare(bad, fresh, target["generation"])
        except RuntimeError:
            rejected.append(label)
        else:
            raise RuntimeError(f"{label} canary was accepted")
        del bad
    return rejected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--fresh", type=Path, required=True)
    parser.add_argument("--generation", type=int, required=True)
    parser.add_argument("--canaries", action="store_true")
    args = parser.parse_args()
    target = json.loads(args.target.read_text())
    fresh = json.loads(args.fresh.read_text())
    verdict = compare(target, fresh, args.generation)
    if args.canaries:
        verdict["canaries_rejected"] = canaries(target)
    print(json.dumps(verdict, sort_keys=True))


if __name__ == "__main__":
    main()
