#!/usr/bin/env python3
"""Generate the fixed Stage 12 ICS55 mapped-netlist scale set."""

import argparse
import hashlib
import json
from pathlib import Path

SCALES = (10_000, 100_000, 1_000_000)


def write_design(output: Path, kind: str, target_nodes: int) -> dict[str, object]:
    sequential = kind == "multiclock"
    per_group, ports = (32, 4) if sequential else (40, 2)
    groups = max(1, round((target_nodes - ports) / per_group))
    name = f"{kind}_{target_nodes}"
    root = output / name
    root.mkdir(parents=True, exist_ok=False)
    verilog = root / "mapped.v"
    inputs = ["clk_a", "clk_b", "data_a", "data_b"] if sequential else ["clk", "data"]
    outputs = (
        [f"out_{i:06d}_{lane}" for i in range(groups) for lane in ("a", "b")]
        if sequential
        else [f"out_{i:06d}" for i in range(groups)]
    )
    with verilog.open("w") as stream:
        stream.write(f"module {name}({', '.join(inputs + outputs)});\n")
        stream.write(f"input {', '.join(inputs)};\noutput {', '.join(outputs)};\n")
        for group in range(groups):
            prefix = f"g{group:06d}"
            if sequential:
                wires = ["ca", "cb", "qa", "qb", "a0", "a1", "b0", "b1", "m0", "m1"]
                stream.write("wire " + ", ".join(f"{prefix}_{wire}" for wire in wires) + ";\n")
                for lane in ("a", "b"):
                    stream.write(
                        f"BUFX1H7L {prefix}_clock_{lane} (.A(clk_{lane}), .Y({prefix}_c{lane}));\n"
                    )
                    stream.write(
                        f"DFFQX1H7L {prefix}_launch_{lane}"
                        f" (.CK({prefix}_c{lane}), .D(data_{lane}), .Q({prefix}_q{lane}));\n"
                    )
                    for fork in range(2):
                        stream.write(
                            f"BUFX1H7L {prefix}_fork_{lane}{fork}"
                            f" (.A({prefix}_q{lane}), .Y({prefix}_{lane}{fork}));\n"
                        )
                for fork, lane in enumerate(("a", "b")):
                    stream.write(
                        f"AND2X1H7L {prefix}_merge_{fork}"
                        f" (.A({prefix}_a{fork}), .B({prefix}_b{fork}), .Y({prefix}_m{fork}));\n"
                    )
                    stream.write(
                        f"DFFQX1H7L {prefix}_capture_{lane}"
                        f" (.CK({prefix}_c{lane}), .D({prefix}_m{fork}),"
                        f" .Q(out_{group:06d}_{lane}));\n"
                    )
            else:
                wires = [f"b{i}" for i in range(8)] + [f"m{i}" for i in range(7)]
                stream.write("wire " + ", ".join(f"{prefix}_{wire}" for wire in wires) + ";\n")
                for branch in range(8):
                    stream.write(
                        f"BUFX1H7L {prefix}_branch_{branch} (.A(data), .Y({prefix}_b{branch}));\n"
                    )
                current = [f"b{i}" for i in range(8)]
                index = 0
                while len(current) > 1:
                    following = []
                    for offset in range(0, len(current), 2):
                        sink = f"m{index}"
                        stream.write(
                            f"AND2X1H7L {prefix}_merge_{index}"
                            f" (.A({prefix}_{current[offset]}), .B({prefix}_{current[offset + 1]}),"
                            f" .Y({prefix}_{sink}));\n"
                        )
                        following.append(sink)
                        index += 1
                    current = following
                stream.write(f"BUFX1H7L {prefix}_output (.A({prefix}_m6), .Y(out_{group:06d}));\n")
        stream.write("endmodule\n")
    sdc = root / "design.sdc"
    if sequential:
        constraints = [
            "create_clock -name c_a -period 20 [get_ports clk_a]",
            "create_clock -name c_b -period 25 [get_ports clk_b]",
            "set_input_delay 0.1 -clock c_a [get_ports data_a]",
            "set_input_delay 0.1 -clock c_b [get_ports data_b]",
        ]
    else:
        constraints = [
            "create_clock -name c -period 20 [get_ports clk]",
            "set_input_delay 0.1 -clock c [get_ports data]",
        ]
    # The native reader resolves explicit names, not Tcl collection expansion.
    for clock, ports_for_clock in (
        (("c_a", outputs[::2]), ("c_b", outputs[1::2])) if sequential else (("c", outputs),)
    ):
        constraints.append(
            f"set_output_delay 0.1 -clock {clock} [get_ports {{{' '.join(ports_for_clock)}}}]"
        )
    constraints += [
        f"set_input_transition 0.05 [get_ports {{{' '.join(inputs)}}}]",
        f"set_load 0.001 [get_ports {{{' '.join(outputs)}}}]",
    ]
    sdc.write_text("\n".join(constraints) + "\n")
    return {
        "name": name,
        "kind": kind,
        "target_timing_nodes": target_nodes,
        "estimated_timing_nodes": per_group * groups + ports,
        "groups": groups,
        "top": name,
        "trace_side": "q" if sequential else "data",
        "constraint_ports": {
            "inputs": inputs,
            "output_count": len(outputs),
            "output_delay": 0.1,
            "input_transition": 0.05,
            "output_load": 0.001,
        },
        "required_performance_case": target_nodes == 100_000,
        "artifacts": {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (verilog, sdc)
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    designs = [
        write_design(args.output, kind, scale)
        for kind in ("wide", "multiclock")
        for scale in SCALES
    ]
    (args.output / "generation.json").write_text(
        json.dumps(
            {
                "schema_version": "ista-fixed-scale-inputs/1.0.0",
                "technology": "ICS55",
                "parasitics": "none",
                "designs": designs,
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
