"""Independent level-layout canaries for functional timing comparison."""

import copy
import unittest
from typing import Any

from functional_timing import TIMING_VALUE_FIELDS, validate_functional
from trace_process import ControllerError


def canary_receipt(level_offsets: list[int], level_nodes: list[dict[str, str]],
                   inverse: list[int]
                   ) -> dict[str, Any]:
    keys = [{"kind": "port", "full_name": name,
             "instance_name": "", "pin_name": ""}
            for name in ("a", "b", "c")]
    graph = {"nodes": [{"key": key} for key in keys],
             "active_arcs": [{"key": {"canary": "a-to-c"},
                              "source": keys[0], "sink": keys[2],
                              "eligibility": {"levelization": True}}],
             "reference_payload": {}}
    timing: dict[str, object] = {field: {} for field in TIMING_VALUE_FIELDS}
    timing["schema_version"] = "canary"
    timing.update({
        "level_offsets": level_offsets, "level_nodes": level_nodes,
        "node_to_level": inverse})
    return {"graph": graph, "timing": timing}

class FunctionalTimingTests(unittest.TestCase):
    def test_legal_layouts_preserve_functional_values(self):
        keys = [{"kind": "port", "full_name": name,
                 "instance_name": "", "pin_name": ""} for name in ("a", "b", "c")]
        compact = canary_receipt([0, 2, 3], keys, [1, 1, 2])
        noncompact = canary_receipt([0, 1, 2, 3], keys, [1, 2, 3])
        reordered = canary_receipt([0, 2, 3], [keys[1], keys[0], keys[2]], [1, 1, 2])
        expected, _ = validate_functional(compact, "compact")
        for candidate in (noncompact, reordered):
            self.assertEqual(validate_functional(candidate, "candidate")[0], expected)
        for field, value in (
            ("level_nodes", keys[:-1]),
            ("level_nodes", [keys[0], keys[0], keys[2]]),
            ("level_nodes", [keys[0], keys[1], {"unknown": "node"}]),
            ("node_to_level", [1, 1, 1]),
            ("level_offsets", [0, 0, 3]),
        ):
            bad = copy.deepcopy(compact)
            bad["timing"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ControllerError):
                validate_functional(bad, "malformed")
        reversed_levels = canary_receipt([0, 1, 2, 3], list(reversed(keys)), [3, 2, 1])
        with self.assertRaises(ControllerError):
            validate_functional(reversed_levels, "backward-edge")


if __name__ == "__main__":
    unittest.main()
