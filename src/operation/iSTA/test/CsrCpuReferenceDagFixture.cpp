#include <algorithm>
#include <numeric>
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrValidatorTestFixture.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr::test {
CsrReferenceInput makeCpuReferenceDagFixture(std::size_t node_count, const std::vector<DagArc>& specs)
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.out_row_offsets.assign(node_count + 1, 0);
  input.in_row_offsets.assign(node_count + 1, 0);
  input.clock_owner_by_node.assign(node_count, std::nullopt);
  if (node_count == 0) {
    refreshCpuReferenceIdentity(input);
    requireCpuReference(validateReferenceInput(input).empty(), "empty DAG fixture is invalid");
    return input;
  }

  std::vector<std::string> names;
  for (std::size_t node = 0; node < node_count; ++node) {
    const auto number = std::to_string(node);
    names.push_back("p" + std::string(std::to_string(node_count - 1).size() - number.size(), '0') + number);
    const bool output = node + 1 == node_count;
    input.nodes.push_back(NodeRecord{.id = NodeId{node},
                                     .key = pin("u1:" + names.back(), names.back()),
                                     .direction = output ? PinDirection::kOutput : PinDirection::kInput,
                                     .instance_cell = "DAG_X1",
                                     .net_name = {},
                                     .is_output_pin = output});
    input.node_ids_by_key.push_back({input.nodes.back().key, NodeId{node}});
  }

  std::vector<std::uint8_t> incoming(node_count, 0);
  std::vector<std::uint8_t> outgoing(node_count, 0);
  std::vector<ArcKeyBase> bases;
  for (const DagArc& spec : specs) {
    ArcKeyBase base;
    base.source = input.nodes[spec.source].key;
    base.sink = input.nodes[spec.sink].key;
    base.kind = ArcKind::kCell;
    base.owner_name = "u1";
    base.library_source_port = names[spec.source];
    base.library_sink_port = names[spec.sink];
    bases.push_back(std::move(base));
    incoming[spec.sink] = 1;
    outgoing[spec.source] = 1;
  }
  const std::vector<ArcKey> keys = assignParallelOrdinals(bases);
  for (std::size_t rank = 0; rank < specs.size(); ++rank) {
    const DagArc& spec = specs[rank];
    ArcRecord arc;
    arc.key = keys[rank];
    arc.source = NodeId{spec.source};
    arc.sink = NodeId{spec.sink};
    arc.kind = ArcKind::kCell;
    arc.native_traversal_rank = rank;
    arc.is_disabled = spec.disabled;
    arc.is_loop_disabled = spec.loop_disabled;
    if (!spec.disabled && !spec.loop_disabled)
      arc.eligibility = {true, true, true, false};
    arc.transfer.delay = 1.0;
    arc.transfer.delay_min = 1.0;
    arc.transfer.delay_max = 1.0;
    arc.transfer.is_disabled = spec.disabled;
    arc.output_load = outputLoad(0.0);
    input.arcs.push_back(std::move(arc));
  }
  std::ranges::sort(input.arcs, [](const ArcRecord& lhs, const ArcRecord& rhs) { return arcKeyLess(lhs.key, rhs.key); });
  for (std::size_t arc = 0; arc < input.arcs.size(); ++arc) {
    input.arcs[arc].id = ArcId{arc};
    input.arc_ids_by_key.push_back({input.arcs[arc].key, ArcId{arc}});
  }
  for (std::size_t node = 0; node < node_count; ++node) {
    input.nodes[node].is_startpoint = incoming[node] == 0;
    input.nodes[node].is_endpoint = outgoing[node] == 0;
    if (input.nodes[node].is_startpoint)
      input.ordered_start_ids.push_back(NodeId{node});
    if (input.nodes[node].is_endpoint)
      input.ordered_end_ids.push_back(NodeId{node});
  }
  for (std::size_t rank = 0; rank < specs.size(); ++rank) {
    const auto found = std::ranges::find(input.arcs, rank, &ArcRecord::native_traversal_rank);
    input.out_arc_ids.push_back(found->id);
    input.in_arc_ids.push_back(found->id);
  }
  std::ranges::stable_sort(input.out_arc_ids, [&](ArcId lhs, ArcId rhs) {
    const auto& left = input.arcs[lhs.value()];
    const auto& right = input.arcs[rhs.value()];
    return left.source == right.source ? left.native_traversal_rank < right.native_traversal_rank : left.source.value() < right.source.value();
  });
  std::ranges::stable_sort(input.in_arc_ids, [&](ArcId lhs, ArcId rhs) {
    const auto& left = input.arcs[lhs.value()];
    const auto& right = input.arcs[rhs.value()];
    return left.sink == right.sink ? left.native_traversal_rank < right.native_traversal_rank : left.sink.value() < right.sink.value();
  });
  for (ArcId arc : input.out_arc_ids)
    ++input.out_row_offsets[input.arcs[arc.value()].source.value() + 1];
  for (ArcId arc : input.in_arc_ids)
    ++input.in_row_offsets[input.arcs[arc.value()].sink.value() + 1];
  std::partial_sum(input.out_row_offsets.begin(), input.out_row_offsets.end(), input.out_row_offsets.begin());
  std::partial_sum(input.in_row_offsets.begin(), input.in_row_offsets.end(), input.in_row_offsets.begin());

  std::vector<std::uint64_t> indegree(node_count, 0);
  for (const ArcRecord& arc : input.arcs)
    if (!arc.is_disabled && !arc.is_loop_disabled)
      ++indegree[arc.sink.value()];
  for (std::size_t node = 0; node < node_count; ++node) {
    if (indegree[node] == 0)
      input.ordered_root_ids.push_back(NodeId{node});
    if (incoming[node] == 0) {
      StartSeedModel seed;
      seed.seed_node = NodeId{node};
      seed.path_start_node = NodeId{node};
      seed.clock_tag = "clk";
      for (auto& analysis : seed.slew)
        for (auto& value : analysis)
          value = 0.0;
      input.start_seeds.push_back(std::move(seed));
    }
  }
  if (!input.start_seeds.empty())
    input.clock_tags = {"clk"};

  input.instances = {InstanceRecord{"u1", "DAG_X1", {}, false, false, false, false}};
  ReferencedLibraryCellModel cell;
  cell.cell_name = "DAG_X1";
  cell.present = true;
  cell.library_name = "slow.lib";
  cell.output_pin_name = names.back();
  for (std::size_t node = 0; node < node_count; ++node) {
    input.instances.front().ordered_pins.push_back(NodeId{node});
    const bool output = node + 1 == node_count;
    cell.ports.push_back(LibraryPortModel{.name = names[node],
                                          .direction = output ? PinDirection::kOutput : PinDirection::kInput,
                                          .is_output = output,
                                          .default_capacitance = 0.0,
                                          .transition_capacitance = {}});
    input.cell_pin_capacitances.push_back({NodeId{node}, 0.0, {}});
  }
  for (const ArcRecord& arc : input.arcs)
    cell.cell_arcs.push_back({arc.key.base.library_source_port, arc.key.base.library_sink_port, arc.transfer});
  input.referenced_library_cells = {std::move(cell)};
  refreshCpuReferenceIdentity(input);
  requireCpuReference(validateReferenceInput(input).empty(), "cell-only DAG fixture is invalid");
  return input;
}

}  // namespace ista::csr::test
