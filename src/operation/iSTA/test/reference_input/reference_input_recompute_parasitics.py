"""Independent support check for raw pre-route tree connectivity and pin coverage."""

import reference_input_model as model


def supports_pre_route_trees(candidate: model.CandidateInput) -> bool:
    if candidate.analysis_context.rc_corner != "pre_route":
        return False
    pins_by_net: dict[str, list[model.CandidateNode]] = {
        net.map_key: [] for net in candidate.parasitics.nets
    }
    for node in candidate.nodes:
        if node.net_name in pins_by_net:
            pins_by_net[node.net_name].append(node)
    for net in candidate.parasitics.nets:
        pins = pins_by_net[net.map_key]
        drivers = [
            pin for pin in pins
            if pin.direction == (model.Direction.INPUT if pin.is_port else model.Direction.OUTPUT)
        ]
        if len(drivers) != 1:
            return False
        explicit = {node.map_key for node in net.nodes}
        adjacency: dict[str, list[str]] = {name: [] for name in explicit}
        for edge in net.resistors:
            adjacency.setdefault(edge.source, []).append(edge.sink)
            adjacency.setdefault(edge.sink, []).append(edge.source)
        if not adjacency or len(net.resistors) != len(adjacency) - 1:
            return False
        root = drivers[0].key.full_name
        if root not in explicit:
            root = root.replace(":", "/")
        if root not in explicit:
            return False
        seen = {root}
        pending = [root]
        while pending:
            for other in adjacency[pending.pop()]:
                if other not in seen:
                    seen.add(other)
                    pending.append(other)
        if len(seen) != len(adjacency):
            return False
        by_name = {pin.key.full_name: pin.node_id for pin in pins}
        covered: set[int] = set()
        for name in adjacency:
            node_id = by_name.get(name)
            if node_id is None:
                node_id = by_name.get(name.replace("/", ":"))
            if node_id is not None:
                if name not in explicit or node_id in covered:
                    return False
                covered.add(node_id)
        if covered != {pin.node_id for pin in pins}:
            return False
    return True


def self_test() -> None:
    from dataclasses import replace

    from reference_input_wire_fixture import fixture_case

    candidate, _ = fixture_case("chain")
    nodes = tuple(replace(node, net_name="n") for node in candidate.nodes)
    net = model.CandidateParasiticNet(
        "n", "n", 0.02,
        tuple(model.RawParasiticNode(name, name, 0.01, 0.0, 0.0) for name in ("in", "out")),
        (model.RawParasiticResistor("in", "out", 10.0),),
    )
    candidate = replace(
        candidate, nodes=nodes,
        analysis_context=replace(candidate.analysis_context, rc_corner="pre_route"),
        parasitics=model.CandidateParasitics("PF", "OHM", (net,)),
    )
    assert supports_pre_route_trees(candidate)
    assert not supports_pre_route_trees(replace(
        candidate, analysis_context=replace(candidate.analysis_context, rc_corner="spef"),
    ))
    for invalid in (
        replace(net, resistors=()),
        replace(net, resistors=net.resistors * 2),
        replace(net, nodes=net.nodes[:1]),
        replace(net, nodes=(*net.nodes, model.RawParasiticNode("stray", "stray", 0.0, 0.0, 0.0))),
    ):
        assert not supports_pre_route_trees(replace(
            candidate, parasitics=replace(candidate.parasitics, nets=(invalid,)),
        ))


if __name__ == "__main__":
    self_test()
