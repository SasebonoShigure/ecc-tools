#include "CudaTargetedDeviceComparison.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>

#include "CsrCudaPacking.hpp"

namespace ista::csr::test {
namespace {
using namespace cuda_detail;
void require(bool value, const char* message)
{
  if (!value)
    throw std::runtime_error(message);
}
void sameSlice(const std::vector<double>& left, Index begin, Index count, const std::vector<double>& right, Index other_begin, Index other_count)
{
  require(count == other_count && begin <= left.size() && count <= left.size() - begin && other_begin <= right.size() && count <= right.size() - other_begin,
          "device timing table has wrong slice bounds");
  require(std::equal(left.begin() + begin, left.begin() + begin + count, right.begin() + other_begin),
          "device timing table values differ from canonical input");
}
void sameTable(const Table& actual, const Table& expected, const std::vector<double>& device, const std::vector<double>& host)
{
  require(std::tie(actual.present, actual.dimensions, actual.variable1) == std::tie(expected.present, expected.dimensions, expected.variable1),
          "device timing table identity differs");
  for (std::size_t axis = 0; axis < 2; ++axis)
    sameSlice(device, actual.axis_offset[axis], actual.axis_size[axis], host, expected.axis_offset[axis], expected.axis_size[axis]);
  sameSlice(device, actual.value_offset, actual.value_size, host, expected.value_offset, expected.value_size);
}
void appendRow(std::vector<Index>& result, const std::vector<Index>& offsets, const std::vector<Index>& edges, Index node, bool tombstones)
{
  const auto begin = offsets.at(node), end = offsets.at(node + 1);
  require(begin <= end && end <= edges.size(), "device adjacency row has wrong bounds");
  for (auto position = begin; position < end; ++position) {
    const auto arc = edges[position];
    if (tombstones && arc == absent)
      continue;
    require(arc != absent, "device incoming/Aux adjacency contains a tombstone");
    result.push_back(arc);
  }
}
}  // namespace

void requireCudaDeviceMatchesCanonical(const CudaTargetedTraceObservation& observation)
{
  const auto& input = observation.canonical_input;
  const auto& device = observation.device;
  ModelStorage models;
  const auto expected = cuda_detail::pack(input, models);  // Mechanical encoding, no CPU timing or update.
  require(device.out_offsets.size() == input.nodes.size() + 1 && device.in_offsets.size() == input.nodes.size() + 1
              && device.aux_rows.size() == input.nodes.size() && device.arcs.size() == observation.arc_native_ranks.size(),
          "device graph extents differ");
  std::map<std::string, Index> stable_nodes;
  for (Index node = 0; node < observation.node_names.size(); ++node)
    require(stable_nodes.emplace(observation.node_names[node], node).second, "stable node name collision");
  std::map<std::size_t, Index> stable_arcs;
  for (Index arc = 0; arc < observation.arc_native_ranks.size(); ++arc) {
    const auto rank = observation.arc_native_ranks[arc];
    require(device.arcs[arc].active == rank.has_value(), "device active mask differs from canonical binding");
    if (rank)
      require(stable_arcs.emplace(*rank, arc).second, "stable native Arc rank collision");
  }
  require(stable_arcs.size() == input.arcs.size(), "device active Arc cardinality differs");
  std::vector<Index> canonical_to_stable;
  for (const auto& arc : input.arcs)
    canonical_to_stable.push_back(stable_arcs.at(arc.native_traversal_rank));
  for (const auto& node : input.nodes) {
    const auto stable = stable_nodes.at(node.key.full_name);
    std::vector<Index> actual_out, actual_in, wanted_out, wanted_in;
    appendRow(actual_out, device.out_offsets, device.out_arcs, stable, true);
    const auto& aux = device.aux_rows.at(stable);
    for (auto arc : aux) {
      require(arc != absent, "device Aux row contains a tombstone");
      actual_out.push_back(arc);
    }
    appendRow(actual_in, device.in_offsets, device.in_arcs, stable, false);
    for (auto slot = input.out_row_offsets.at(node.id.value()); slot < input.out_row_offsets.at(node.id.value() + 1); ++slot)
      wanted_out.push_back(canonical_to_stable.at(input.out_arc_ids.at(slot).value()));
    for (auto slot = input.in_row_offsets.at(node.id.value()); slot < input.in_row_offsets.at(node.id.value() + 1); ++slot)
      wanted_in.push_back(canonical_to_stable.at(input.in_arc_ids.at(slot).value()));
    require(actual_out == wanted_out && actual_in == wanted_in, "device Base/Aux adjacency differs from canonical order");
  }
  for (const auto& source : input.arcs) {
    const auto& actual = device.arcs.at(canonical_to_stable.at(source.id.value()));
    const auto& wanted = expected.arcs.at(source.id.value());
    require(actual.source == stable_nodes.at(source.key.base.source.full_name) && actual.sink == stable_nodes.at(source.key.base.sink.full_name),
            "device Arc endpoints differ");
    require(std::tie(actual.child_count, actual.delay, actual.delay_min, actual.delay_max, actual.net, actual.clock, actual.has_load, actual.levelization,
                     actual.data_arrival, actual.data_slew, actual.clock_eligible)
                    == std::tie(wanted.child_count, wanted.delay, wanted.delay_min, wanted.delay_max, wanted.net, wanted.clock, wanted.has_load,
                                wanted.levelization, wanted.data_arrival, wanted.data_slew, wanted.clock_eligible)
                && std::equal(std::begin(actual.load), std::end(actual.load), std::begin(wanted.load)),
            "device Arc timing payload differs from canonical input");
    require(actual.child_begin <= device.children.size() && actual.child_count <= device.children.size() - actual.child_begin,
            "device child range outside buffer");
    for (Index index = 0; index < actual.child_count; ++index) {
      const auto& child = device.children.at(actual.child_begin + index);
      const auto& wanted_child = models.children.at(wanted.child_begin + index);
      require(std::tie(child.time_scale, child.capacitance_scale, child.arc_index, child.sense, child.trigger)
                  == std::tie(wanted_child.time_scale, wanted_child.capacitance_scale, wanted_child.arc_index, wanted_child.sense, wanted_child.trigger),
              "device child identity/units differ");
      for (std::size_t transition = 0; transition < 2; ++transition) {
        sameTable(child.check[transition], wanted_child.check[transition], device.table_data, models.tables);
        sameTable(child.delay[transition], wanted_child.delay[transition], device.table_data, models.tables);
        sameTable(child.slew[transition], wanted_child.slew[transition], device.table_data, models.tables);
      }
    }
  }
}
}  // namespace ista::csr::test
