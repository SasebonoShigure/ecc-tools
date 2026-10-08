#!/usr/bin/env python3
"""Hand-built valid and malformed Step 4 wires, independent of every decoder."""
# ruff: noqa: FBT001, FBT003

from dataclasses import replace

import reference_input_model as model
from reference_input_canonical import CanonicalWriter

CANDIDATE_DOMAIN = "ista-csr-reference-input-test-wire/2.0.0"
RAW_DOMAIN = "ista-csr-reference-input-raw/3.0.0"
CANARY_PREFIX = "ista-csr-reference-input-canary/"
CANARY_SUFFIX = "/1.0.0"
CONTEXT = model.AnalysisContext("fresh-liberty-corner", "25", "__no_rc_corner__")
ZERO_HASH = "0" * 64

DEEP_CANDIDATE_HEX = (
    "0000000000000028697374612d6373722d7265666572656e63652d696e7075742d746573742d776972652f322e302e300000000000000005312e"
    "302e300000000000000005312e302e30000000000000000100000000000000000000000000000040653937653434646634613636323032613132"
    "63623663386638623062333439336666373131633361343064396233643431656139643764336638346239346266000000000000004062363263"
    "33353166303532396561393036643735643561653765333630653337633234386665393639353665653034643330396133393132313833623666"
    "35390000000000000040383136623539303264306339363531653831613637333066663936316136323662333163616534306438323132616535"
    "66323936616236303262646432653636000000000000004035353033363139346539623236343431333033363537326461313032343438303539"
    "39323034356435376463633161623531323736643361386638373937636400000000000000403939313264336436623230316363396463333064"
    "61303432396234623734333166386564343339373433326131386632363530633630333537663137626465640000000000000040643461306566"
    "38613432643532346462376663333632653961316434646337323232346665383332366164623061643232613766353664373064633136366335"
    "000000000000001466726573682d6c6962657274792d636f726e65720000000000000002323500000000000000105f5f6e6f5f72635f636f726e"
    "65725f5f00010001000100000000000000040000000000000000000000000000000002696e000000000000000000000000000000000100000000"
    "000000000000000000000009696e7075745f6e65740101000000000000000000000000010000000000000000036f757400000000000000000000"
    "000000000000020000000000000000000000000000000a6f75747075745f6e657401000100000000000000000000000201000000000000000375"
    "3a4100000000000000017500000000000000014101000000000000000443454c4c0000000000000009696e7075745f6e65740000000000000000"
    "00000000000003010000000000000003753a5900000000000000017500000000000000015902000000000000000443454c4c000000000000000a"
    "6f75747075745f6e6574000000000000010000000000000004000000000000000002696e00000000000000000000000000000000000000000000"
    "00000000000000000000036f7574000000000000000000000000000000000000000000000001010000000000000003753a410000000000000001"
    "750000000000000001410000000000000002010000000000000003753a5900000000000000017500000000000000015900000000000000030000"
    "000000000001000000000000000175000000000000000443454c4c00000000000000020000000000000002000000000000000300000000000000"
    "00000000030000000000000000000000000000000002696e00000000000000000000000000000000010000000000000003753a41000000000000"
    "00017500000000000000014101000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000020100000000000000000000000001010100000000000000000000000000000000000000000000000000000000000000"
    "000000000000000001003fd0000000000000003fd0000000000000003fd0000000000000003fd000000000000000000000000000010100000000"
    "00000003753a41000000000000000175000000000000000141010000000000000003753a59000000000000000175000000000000000159000000"
    "00000000000175000000000000000141000000000000000159000000000000000100000000000000036c69620000000001000200000000000000"
    "0641203d3d2031000000000000000000000000000000000000000000000000000000020000000000000003000000000000000001010000000000"
    "00000000000001010100003ff0000000000000003fe0000000000000003ff8000000000000000000000000000000000100000000000000036c69"
    "6200000000010002000000000000000641203d3d2031000000000000000000000000003ff0000000000000004000000000000000003ff0000000"
    "000000003fb999999999999a003fb999999999999a003feccccccccccccd003feccccccccccccd003fe0000000000000003fe000000000000000"
    "3fe0000000000000003fe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000001003fe8000000000000003fe8000000000000003fe8000000000000003fe8000000000000000000000000"
    "0002010000000000000003753a590000000000000001750000000000000001590000000000000000036f75740000000000000000000000000000"
    "00000100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000300000000000000"
    "01010000000000000002000000000101010000000000000000000000000000000000000000000000000000000000000000000000000000000100"
    "3fe8000000000000003fe8000000000000003fe8000000000000003fe80000000000000000000000000003000000000000000002696e00000000"
    "000000000000000000000000010000000000000003753a4100000000000000017500000000000000014101000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000010000000000000003753a4100000000000000017500000000"
    "0000000141010000000000000003753a590000000000000001750000000000000001590000000000000000017500000000000000014100000000"
    "0000000159000000000000000100000000000000036c696200000000010002000000000000000641203d3d203100000000000000000000000000"
    "000000000000000000000000000001010000000000000003753a590000000000000001750000000000000001590000000000000000036f757400"
    "00000000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000200000000000000050000000000000000000000000000000100000000000000010000000000000002000000000000000300000000"
    "00000003000000000000000000000000000000010000000000000002000000000000000500000000000000000000000000000000000000000000"
    "00010000000000000002000000000000000300000000000000030000000000000002000000000000000000000000000000010000000000000001"
    "0000000000000000000000000000000100000000000000000000000000000001000000000000000100000000000000010000000000000003636c"
    "6b0000000000000000000000000000000400000000000000000000000100000000000000036f757400000000000000036f757401000000000000"
    "0001000000000001003fe80000000000000000000000000003636c6b000000000000000000000000000000000000000000000001000000000000"
    "000443454c4c0100000000000000036c696200000000000000000000000000000000000000000000000000000159003ff0000000000000003fb9"
    "99999999999a003fb999999999999a003feccccccccccccd003feccccccccccccd003fe0000000000000003fe000000000000000000000000000"
    "02000000000000000141010000003fd0000000000000000000000000000000000001590200010000000000000000000000000000000000000000"
    "01000000000000000141000000000000000159003ff0000000000000003fe0000000000000003ff8000000000000000000000000000000000100"
    "000000000000036c696200000000010002000000000000000641203d3d2031000000000000000000000000003ff0000000000000004000000000"
    "000000003ff0000000000000003fb999999999999a003fb999999999999a003feccccccccccccd003feccccccccccccd003fe000000000000000"
    "3fe0000000000000003fe0000000000000003fe00000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003ff000000000000000"
    "3fd3333333333333003fd3333333333333003fe6666666666666003fe66666666666660000000000000000000000000000000100000000000000"
    "00000000000000000000000000000000000003636c6b000000000100000000000000000001000000000000000000010000000000000000000100"
    "00000000000000000000000000000000000100000000000000010000000000000003636c6b000000000000000000000000000000000000020000"
    "000000000002003fd0000000000000000000000000000000000003000000000000000000000000000000000000000000"
)
DEEP_RAW_HEX = (
    "0000000000000022697374612d6373722d7265666572656e63652d696e7075742d7261772f332e302e3000000000000000040000000000000002696e"
    "0000000000000002696e0101000000000000000000000000000000010000000000000000000000000000000000000000000000036f75740000000000"
    "0000036f7574010200000000000000000000000000000000000000000000000100000000000000020000000000000003753a41000000000000000375"
    "3a41000100000000000000017500000000000000010000000000000001000000000000000100000000000000000000000000000003753a5900000000"
    "00000003753a590002000000000000000175000000000000000100000000000000020000000000000001000000000000000100000000000000010000"
    "00000000000175000000000000000443454c4c000000000000000000000000000000000000000000000000000003753a590000000000000002000000"
    "0000000003753a410000000000000003753a5900000000000000000300000000000000000000000000000002696e0000000000000003753a41010000"
    "000000000000000000000000000000000000000000000000000000000000000000010000000000000003753a410000000000000003753a5900000000"
    "00000000017500000000000000014100000000000000015900000001000000000000000000000000000000020000000000000003753a590000000000"
    "0000036f7574010000000000000000000000000000000000000000000000000000000000000000000000020000000000000009696e7075745f6e6574"
    "0000000000000009696e7075745f6e657400000000000000010000000000000002696e00000000000000010000000000000003753a41000000000000"
    "000a6f75747075745f6e6574000000000000000a6f75747075745f6e657400000000000000010000000000000003753a590000000000000001000000"
    "00000000036f75740000000000000000000000000000000100000000000000036f757400000000000000036f75740000000000000003636c6b000000"
    "000001003fe800000000000000000000000000000000000000000000000000000000000100000000000000036c6962000000000000000443454c4c01"
    "000000000000000000000000000000000000000000000000000001590000000000000002504600000000000000024e53003ff0000000000000003fb9"
    "99999999999a003fb999999999999a003feccccccccccccd003feccccccccccccd003fe0000000000000003fe0000000000000003fe0000000000000"
    "003fe00000000000000000000000000002000000000000000141010000003fd000000000000000000000000000000000000159020001000000000000"
    "000000000000000000000000000001000000000000000141000000000000000159003ff0000000000000003fe0000000000000003ff8000000000000"
    "000000000000000000000100000000000000036c696200000000010002000000000000000641203d3d2031003ff00000000000000040000000000000"
    "00003ff0000000000000003fb999999999999a003fb999999999999a003feccccccccccccd003feccccccccccccd003fe0000000000000003fe00000"
    "00000000003fe0000000000000003fe0000000000000000000000000000000000000000000"
)


def _identity() -> model.SnapshotIdentity:
    return model.SnapshotIdentity(
        "1.0.0", 1, 0, ZERO_HASH, ZERO_HASH, ZERO_HASH, ZERO_HASH, ZERO_HASH, ZERO_HASH
    )


def _shape(case_name: str) -> tuple[tuple[str, ...], tuple[tuple[str, str], ...]]:
    if case_name == "chain":
        return ("in", "out"), (("in", "out"),)
    if case_name == "diamond":
        return ("in", "mid", "out"), (("in", "mid"), ("in", "out"))
    raise ValueError(f"unknown fixture case {case_name!r}")


def fixture_case(
    case_name: str, identity: model.SnapshotIdentity | None = None
) -> tuple[model.CandidateInput, model.RawReferenceInput]:
    names, edges = _shape(case_name)
    keys = tuple(model.NodeKey(model.NodeKind.PORT, name, "", "") for name in names)
    node_ids = {name: index for index, name in enumerate(names)}
    arcs: list[model.CandidateArc] = []
    raw_arcs: list[model.RawArc] = []
    for rank, (source_name, sink_name) in enumerate(edges):
        source = node_ids[source_name]
        sink = node_ids[sink_name]
        key = model.ArcKey(keys[source], keys[sink], model.ArcKind.NET, "", "", "", (), 0)
        transfer = model.CellTransfer(0.0, 0.0, 0.0, False, False, False, ())
        arcs.append(
            model.CandidateArc(
                rank,
                key,
                source,
                sink,
                model.ArcKind.NET,
                rank,
                False,
                False,
                False,
                model.ArcEligibility(True, True, True, False),
                transfer,
                ((0.0, 0.0), (0.0, 0.0)),
            )
        )
        raw_arcs.append(
            model.RawArc(
                rank,
                source_name,
                sink_name,
                model.ArcKind.NET,
                "",
                "",
                "",
                False,
                False,
                False,
            )
        )
    outgoing: list[list[int]] = [[] for _name in names]
    incoming: list[list[int]] = [[] for _name in names]
    for arc in arcs:
        outgoing[arc.source].append(arc.arc_id)
        incoming[arc.sink].append(arc.arc_id)
    out_offsets, out_ids = _flatten(outgoing)
    in_offsets, in_ids = _flatten(incoming)
    nodes = tuple(
        model.CandidateNode(
            index,
            key,
            model.Direction.INPUT if index == 0 else model.Direction.OUTPUT,
            "",
            f"{case_name}_net",
            True,
            index == 0,
            index != 0,
            False,
            False,
            False,
            False,
        )
        for index, key in enumerate(keys)
    )
    none_matrix = ((None, None), (None, None))
    zero_matrix = ((0.0, 0.0), (0.0, 0.0))
    candidate = model.CandidateInput(
        "1.0.0",
        _identity() if identity is None else identity,
        CONTEXT,
        (model.AnalysisMode.MAX, model.AnalysisMode.MIN),
        (model.PathSource.INPUT, model.PathSource.REGISTER),
        (model.Transition.RISE, model.Transition.FALL),
        nodes,
        tuple(model.NodeLookup(node.key, node.node_id) for node in nodes),
        (),
        tuple(arcs),
        tuple(model.ArcLookup(arc.key, arc.arc_id) for arc in arcs),
        out_offsets,
        out_ids,
        in_offsets,
        in_ids,
        (0,),
        (0,),
        tuple(range(1, len(nodes))),
        ("clk",),
        (),
        tuple(None for _node in nodes),
        (),
        (),
        (),
        (),
        model.CandidateParasitics("", "", ()),
        (
            model.CandidateStartSeed(
                0,
                0,
                model.StartSeedAvailability.MATERIALIZED,
                None,
                "clk",
                none_matrix,
                zero_matrix,
                None,
                None,
            ),
        ),
        tuple(
            model.CandidateEndpointRequired(index, "clk", 0.0, (None, None))
            for index in range(1, len(nodes))
        ),
        (),
        (),
    )
    raw = model.RawReferenceInput(
        tuple(
            model.RawNode(
                name,
                name,
                True,
                model.Direction.INPUT if index == 0 else model.Direction.OUTPUT,
                "",
                tuple(outgoing[index]),
                tuple(incoming[index]),
            )
            for index, name in enumerate(names)
        ),
        (),
        tuple(raw_arcs),
        (
            model.RawNet(
                f"{case_name}_net",
                f"{case_name}_net",
                (names[0],),
                names[1:],
            ),
        ),
        (),
        (),
        (),
        (),
        (),
        None,
    )
    return candidate, raw


def with_identity(
    candidate: model.CandidateInput, identity: model.SnapshotIdentity
) -> model.CandidateInput:
    return replace(candidate, source_identity=identity)


def _flatten(rows: list[list[int]]) -> tuple[tuple[int, ...], tuple[int, ...]]:
    offsets = [0]
    values: list[int] = []
    for row in rows:
        values.extend(row)
        offsets.append(len(values))
    return tuple(offsets), tuple(values)


def _node_key(writer: CanonicalWriter, key: model.NodeKey) -> None:
    writer.enum(key.kind)
    writer.string(key.full_name)
    writer.string(key.instance_name)
    writer.string(key.pin_name)


def _arc_key(writer: CanonicalWriter, key: model.ArcKey) -> None:
    _node_key(writer, key.source)
    _node_key(writer, key.sink)
    writer.enum(key.kind)
    writer.string(key.owner_name)
    writer.string(key.library_source_port)
    writer.string(key.library_sink_port)
    writer.vector((), lambda _value: None)
    writer.u64(key.parallel_ordinal)


def _identity_wire(writer: CanonicalWriter, identity: model.SnapshotIdentity) -> None:
    writer.string(identity.fingerprint_schema_version)
    writer.u64(identity.session_token)
    writer.u64(identity.graph_generation)
    for digest in (
        identity.topology_fingerprint,
        identity.clock_constraint_fingerprint,
        identity.library_fingerprint,
        identity.corner_fingerprint,
        identity.parasitic_fingerprint,
        identity.timing_input_fingerprint,
    ):
        writer.string(digest)


def _node(writer: CanonicalWriter, node: model.CandidateNode) -> None:
    writer.u64(node.node_id)
    _node_key(writer, node.key)
    writer.enum(node.direction)
    writer.string(node.instance_cell)
    writer.string(node.net_name)
    for value in (
        node.is_port,
        node.is_startpoint,
        node.is_endpoint,
        node.is_sequential,
        node.is_clock_pin,
        node.is_data_pin,
        node.is_output_pin,
    ):
        writer.boolean(value)


def _transfer(writer: CanonicalWriter, transfer: model.CellTransfer) -> None:
    writer.finite_double(transfer.delay)
    writer.finite_double(transfer.delay_min)
    writer.finite_double(transfer.delay_max)
    writer.boolean(transfer.is_clock_arc)
    writer.boolean(transfer.is_disabled)
    writer.boolean(transfer.is_clear_or_preset)
    writer.vector((), lambda _value: None)


def _optional_matrix(
    writer: CanonicalWriter,
    values: tuple[tuple[float | None, float | None], tuple[float | None, float | None]],
) -> None:
    for row in values:
        for value in row:
            writer.optional(value, writer.finite_double)


def _arc(writer: CanonicalWriter, arc: model.CandidateArc) -> None:
    writer.u64(arc.arc_id)
    _arc_key(writer, arc.key)
    writer.u64(arc.source)
    writer.u64(arc.sink)
    writer.enum(arc.kind)
    writer.u64(arc.native_traversal_rank)
    writer.optional(arc.native_cell_arc_ordinal, writer.u64)
    writer.boolean(arc.is_clock_arc)
    writer.boolean(arc.is_disabled)
    writer.boolean(arc.is_loop_disabled)
    for value in arc.eligibility.__dict__.values():
        writer.boolean(value)
    _transfer(writer, arc.transfer)
    writer.optional(
        arc.output_load,
        lambda matrix: tuple(writer.finite_double(value) for row in matrix for value in row),
    )


def _u64_vector(writer: CanonicalWriter, values: tuple[int, ...]) -> None:
    writer.vector(values, writer.u64)


def encode_candidate_wire(candidate: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    writer.string(CANDIDATE_DOMAIN)
    writer.string(candidate.schema_version)
    _identity_wire(writer, candidate.source_identity)
    context = candidate.analysis_context
    writer.string(context.liberty_corner)
    writer.string(context.temperature)
    writer.string(context.rc_corner)
    for value in (
        *candidate.analysis_order,
        *candidate.path_source_order,
        *candidate.transition_order,
    ):
        writer.enum(value)
    writer.vector(candidate.nodes, lambda value: _node(writer, value))
    writer.vector(
        candidate.node_ids_by_key,
        lambda value: (_node_key(writer, value.key), writer.u64(value.node_id)),
    )
    writer.vector((), lambda _value: None)
    writer.vector(candidate.arcs, lambda value: _arc(writer, value))
    writer.vector(
        candidate.arc_ids_by_key,
        lambda value: (_arc_key(writer, value.key), writer.u64(value.arc_id)),
    )
    for values in (
        candidate.out_row_offsets,
        candidate.out_arc_ids,
        candidate.in_row_offsets,
        candidate.in_arc_ids,
        candidate.ordered_root_ids,
        candidate.ordered_start_ids,
        candidate.ordered_end_ids,
    ):
        _u64_vector(writer, values)
    writer.vector(candidate.clock_tags, writer.string)
    writer.vector((), lambda _value: None)
    writer.vector(
        candidate.clock_owner_by_node,
        lambda value: writer.optional(value, writer.string),
    )
    for _empty_domain in range(4):
        writer.vector((), lambda _value: None)
    writer.string(candidate.parasitics.capacitance_unit)
    writer.string(candidate.parasitics.resistance_unit)
    thresholds = candidate.parasitics.input_port_thresholds
    for threshold in (thresholds.slew_derate, *thresholds.lower, *thresholds.upper):
        writer.finite_double(threshold)
    writer.vector((), lambda _value: None)
    writer.vector(
        candidate.start_seeds,
        lambda seed: _start_seed(writer, seed),
    )
    writer.vector(
        candidate.endpoint_required,
        lambda endpoint: _endpoint(writer, endpoint),
    )
    writer.vector((), lambda _value: None)
    writer.vector((), lambda _value: None)
    return writer.result()


def _start_seed(writer: CanonicalWriter, seed: model.CandidateStartSeed) -> None:
    writer.u64(seed.seed_node)
    writer.u64(seed.path_start_node)
    writer.enum(seed.availability)
    writer.optional(seed.path_source, writer.enum)
    writer.string(seed.clock_tag)
    _optional_matrix(writer, seed.arrival)
    _optional_matrix(writer, seed.slew)
    writer.optional(seed.crpr_node, writer.u64)
    writer.optional(seed.crpr_transition, writer.enum)


def _endpoint(writer: CanonicalWriter, endpoint: model.CandidateEndpointRequired) -> None:
    writer.u64(endpoint.node)
    writer.string(endpoint.clock_name)
    writer.finite_double(endpoint.clock_period)
    for value in endpoint.output_delay:
        writer.optional(value, writer.finite_double)


def encode_raw_wire(raw: model.RawReferenceInput) -> bytes:
    writer = CanonicalWriter()
    writer.string(RAW_DOMAIN)
    writer.vector(raw.nodes, lambda value: _raw_node(writer, value))
    writer.vector((), lambda _value: None)
    writer.vector(raw.arcs, lambda value: _raw_arc(writer, value))
    writer.vector(raw.nets, lambda value: _raw_net(writer, value))
    for _empty_domain in range(5):
        writer.vector((), lambda _value: None)
    writer.optional(None, lambda _value: None)
    return writer.result()


def _raw_node(writer: CanonicalWriter, node: model.RawNode) -> None:
    writer.string(node.map_key)
    writer.string(node.full_name)
    writer.boolean(node.is_port)
    writer.enum(node.direction)
    writer.string(node.instance_name)
    writer.vector(node.outgoing_arc_ranks, writer.u64)
    writer.vector(node.incoming_arc_ranks, writer.u64)


def _raw_arc(writer: CanonicalWriter, arc: model.RawArc) -> None:
    writer.u64(arc.native_rank)
    writer.string(arc.source)
    writer.string(arc.sink)
    writer.enum(arc.kind)
    writer.string(arc.owner)
    writer.string(arc.library_source_port)
    writer.string(arc.library_sink_port)
    writer.boolean(arc.is_clock_arc)
    writer.boolean(arc.disabled)
    writer.boolean(arc.loop_disabled)
    writer.optional(arc.native_cell_arc_ordinal, writer.u64)


def _raw_net(writer: CanonicalWriter, net: model.RawNet) -> None:
    writer.string(net.map_key)
    writer.string(net.embedded_name)
    writer.vector(net.ordered_drivers, writer.string)
    writer.vector(net.ordered_loads, writer.string)


def encode_canary_wire(
    family: str,
    case_name: str,
    repeat_index: int,
    run_nonce: str,
    *,
    after: bool,
    pristine: bool = False,
    raw_override: model.RawReferenceInput | None = None,
) -> bytes:
    candidate, raw = fixture_case(case_name)
    del candidate
    if raw_override is not None:
        raw = raw_override
    family_index = model.CANARY_FAMILY_TOKENS.index(family)
    writer = CanonicalWriter()
    writer.string(f"{CANARY_PREFIX}{family}{CANARY_SUFFIX}")
    writer.u64(repeat_index)
    writer.string(run_nonce)
    writer.u64(1)
    if family_index <= model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP:
        subjects = tuple(node.full_name for node in raw.nodes)
        writer.vector(
            subjects,
            lambda key: (writer.string(key), _point_payload(writer, family_index, after)),
        )
    elif family_index <= model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE:
        writer.vector(
            raw.arcs,
            lambda arc: (
                _canary_arc_key(writer, arc),
                _arc_payload(writer, family_index, after, pristine),
            ),
        )
    else:
        writer.vector(
            ("database",),
            lambda key: (writer.string(key), _timing_order(writer, raw, after)),
        )
    return writer.result()


def _at_key(writer: CanonicalWriter) -> None:
    writer.enum(model.AnalysisMode.MAX)
    writer.enum(model.Transition.RISE)


def _at_double(writer: CanonicalWriter, after: bool) -> None:
    _at_key(writer)
    writer.canary_double(2.0 if after else 1.0)


def _paired(writer: CanonicalWriter, item: object, after: bool) -> None:
    if not callable(item):
        raise TypeError("paired fixture writer is not callable")
    for _side in range(2):
        writer.vector((after,), lambda changed: item(writer, changed))


def _point_payload(writer: CanonicalWriter, family: int, after: bool) -> None:
    if family == 0:
        writer.i32(2 if after else 1)
    elif family == 1:
        writer.boolean(after)
    elif family == 2:
        for offset in range(4):
            writer.canary_double(float(offset + (10 if after else 1)))
        writer.string("new-predecessor" if after else "old-predecessor")
        writer.string("new-clock" if after else "old-clock")
        writer.u64(2 if after else 1)
    elif family in (3, 4, 6):
        _paired(writer, _at_double, after)
    elif family == 5:
        _paired(writer, _at_string, after)
    elif family == 7:
        _paired(writer, _at_transition, after)
    elif family == 8:
        writer.vector((after,), lambda changed: _at_double(writer, changed))
    elif family == 9:
        writer.vector((after,), lambda changed: _path_state(writer, changed))
    else:
        raise ValueError("invalid point fixture family")


def _at_string(writer: CanonicalWriter, after: bool) -> None:
    _at_key(writer)
    writer.string("new" if after else "old")


def _at_transition(writer: CanonicalWriter, after: bool) -> None:
    _at_key(writer)
    writer.enum(model.Transition.FALL if after else model.Transition.RISE)


def _path_state(writer: CanonicalWriter, after: bool) -> None:
    writer.enum(model.AnalysisMode.MAX)
    writer.enum(model.PathSource.INPUT)
    writer.enum(model.Transition.RISE)
    writer.string("clk")
    for offset in range(3):
        writer.canary_double(float(offset + (10 if after else 1)))
    for name in ("start", "predecessor", "clock", "crpr"):
        writer.string(f"{'new' if after else 'old'}-{name}")
    writer.u64(2 if after else 1)
    writer.canary_double(2.0 if after else 1.0)
    for _value in range(3):
        writer.enum(model.Transition.FALL if after else model.Transition.RISE)


def _canary_arc_key(writer: CanonicalWriter, arc: model.RawArc) -> None:
    writer.u64(arc.native_rank)
    writer.string(arc.source)
    writer.string(arc.sink)
    writer.enum(arc.kind)
    writer.string(arc.owner)
    writer.string(arc.library_source_port)
    writer.string(arc.library_sink_port)


def _arc_payload(writer: CanonicalWriter, family: int, after: bool, pristine: bool) -> None:
    if family == 10:
        for offset in range(3):
            writer.canary_double(float(offset + (10 if after else 1)))
    elif family == 11:
        writer.vector((after,), lambda changed: _at_double(writer, changed))
        writer.vector((after,), lambda changed: _transition_map(writer, changed))
    elif family in (12, 13):
        writer.vector((after,), lambda changed: _aio_double(writer, changed))
    elif family == 14:
        entries = ((0, model.Transition.FALL, 2.0 if after else 1.0),)
        if not pristine:
            entries = ((-(1 << 31), model.Transition.RISE, 41.0 if after else 40.0), *entries)
        writer.vector(entries, lambda entry: _timing_arc_double(writer, *entry))
    elif family == 15:
        writer.boolean(False)
        writer.boolean(False)
        writer.boolean(not after)
    else:
        raise ValueError("invalid arc fixture family")


def _transition_map(writer: CanonicalWriter, after: bool) -> None:
    writer.enum(model.Transition.RISE)
    writer.enum(model.Transition.FALL if after else model.Transition.RISE)


def _aio_double(writer: CanonicalWriter, after: bool) -> None:
    writer.enum(model.AnalysisMode.MAX)
    writer.enum(model.Transition.RISE)
    writer.enum(model.Transition.FALL)
    writer.canary_double(2.0 if after else 1.0)


def _timing_arc_double(
    writer: CanonicalWriter, index: int, output: model.Transition, value: float
) -> None:
    writer.i32(index)
    writer.enum(model.AnalysisMode.MAX)
    writer.enum(model.Transition.RISE)
    writer.enum(output)
    writer.canary_double(value)


def _timing_order(writer: CanonicalWriter, raw: model.RawReferenceInput, after: bool) -> None:
    names = tuple(node.full_name for node in raw.nodes)
    writer.vector(tuple(reversed(names)) if after else names, writer.string)


def malformed(payload: bytes, mutation: str) -> bytes:
    if mutation == "trailing":
        return payload + b"\x00"
    if mutation == "truncated":
        return payload[:-1]
    if mutation == "wrong-domain":
        result = bytearray(payload)
        result[8] ^= 1
        return bytes(result)
    raise ValueError(f"unknown wire mutation {mutation!r}")
