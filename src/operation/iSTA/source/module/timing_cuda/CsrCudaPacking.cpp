#include "CsrCudaPacking.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "CsrCpuReferenceBackendInternal.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr::cuda_detail {
namespace {

std::size_t product(std::size_t a, std::size_t b)
{
  if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
    throw std::length_error("CUDA timing state capacity overflow");
  return a * b;
}

Table packTable(const TimingTableModel& source, std::vector<double>& values)
{
  Table table;
  table.present = source.present;
  table.variable1 = static_cast<std::uint8_t>(source.variable1);
  if (source.axes.size() > 2)
    throw std::invalid_argument("CUDA timing table has more than two axes");
  table.dimensions = static_cast<std::uint8_t>(source.axes.size());
  for (std::size_t i = 0; i < source.axes.size(); ++i) {
    table.axis_offset[i] = values.size();
    table.axis_size[i] = source.axes[i].size();
    values.insert(values.end(), source.axes[i].begin(), source.axes[i].end());
  }
  table.value_offset = values.size();
  table.value_size = source.values.size();
  values.insert(values.end(), source.values.begin(), source.values.end());
  return table;
}

void packArcs(const CsrReferenceInput& input, HostData& data)
{
  for (const auto& source : input.arcs) {
    ArcSemanticPayload payload;
    payload.key = source.key;
    payload.source = source.source;
    payload.sink = source.sink;
    payload.kind = source.kind;
    payload.transfer = source.transfer;
    payload.eligibility = source.eligibility;
    payload.output_load = source.output_load;
    data.arcs.push_back(packArc(payload, data.children, data.table_data));
    const auto& arc = data.arcs.back();
    data.layout.arcs.push_back({arc.child_begin, arc.child_count, true});
  }
  for (const auto& child : data.children)
    data.layout.child_indices.push_back(child.arc_index);
  data.arc_state.resize(data.arcs.size());
  data.child_state.resize(data.children.size());
}

template <class Id>
std::vector<Index> indices(const std::vector<Id>& source)
{
  std::vector<Index> result;
  result.reserve(source.size());
  for (auto id : source)
    result.push_back(id.value());
  return result;
}

Index intern(std::vector<std::string>& names, const std::string& name)
{
  auto found = std::find(names.begin(), names.end(), name);
  if (found != names.end())
    return static_cast<Index>(found - names.begin());
  names.push_back(name);
  return names.size() - 1;
}

void packSeeds(const CsrReferenceInput& input, HostData& data)
{
  CsrCpuReferenceWorkState seeds;
  const FlatTimingPayloadView payloads(input);
  if (initializeCsrCpuReferenceSeeds(payloads, seeds) != CsrCpuReferenceStageStatus::kSuccess)
    throw std::invalid_argument("CUDA timing seed initialization failed");
  data.tag_count = input.clock_tags.size();
  data.nodes.resize(input.nodes.size());
  data.paths.resize(product(product(input.nodes.size(), 8), data.tag_count));
  for (std::size_t i = 0; i < data.nodes.size(); ++i) {
    auto& node = data.nodes[i];
    node.arrival = seeds.result.nodes.arrivals.values[i];
    node.launch = seeds.result.nodes.launches.values[i];
    if (seeds.result.nodes.clock_names.presence[i])
      node.clock_name = intern(data.layout.clock_names, seeds.result.nodes.clock_names.values[i]);
    for (std::size_t lane = 0; lane < 4; ++lane)
      node.slew[lane] = {seeds.result.data_slews.values[i * 4 + lane], seeds.result.data_slews.presence[i * 4 + lane] != 0};
  }
  for (std::size_t row = 0; row < seeds.path_rows.size(); ++row) {
    for (const auto& source : seeds.path_rows[row]) {
      if (source.clock_tag.value() >= data.tag_count || source.crpr_node || source.crpr_transition)
        throw std::invalid_argument("CUDA timing seed has unsupported path identity");
      auto& path = data.paths[row * data.tag_count + source.clock_tag.value()];
      path.present = true;
      path.arrival = source.arrival;
      path.slew = source.slew;
      path.launch = source.launch;
      path.start = source.start_node.value();
    }
  }
  data.layout.is_clock_point = std::move(seeds.result.nodes.is_clock_point.values);
  data.layout.effective_clock = std::move(seeds.result.effective_clock);
  data.layout.physical_clock = std::move(seeds.result.physical_clock);
  data.layout.clock_tags = input.clock_tags;
  data.schema = input.schema_version;
  data.identity = input.source_identity;
}

}  // namespace

Arc packArc(const ArcSemanticPayload& source, std::vector<Child>& children, std::vector<double>& tables,
            Index child_base, Index table_base)
{
  Arc arc;
  arc.source = source.source.value();
  arc.sink = source.sink.value();
  arc.child_begin = child_base + children.size();
  arc.child_count = source.transfer.children.size();
  arc.delay = source.transfer.delay;
  arc.delay_min = source.transfer.delay_min;
  arc.delay_max = source.transfer.delay_max;
  arc.net = source.kind == ArcKind::kNet;
  arc.clock = source.transfer.is_clock_arc;
  arc.has_load = source.output_load.has_value();
  if (source.output_load)
    for (std::size_t a = 0; a < 2; ++a)
      for (std::size_t t = 0; t < 2; ++t)
        arc.load[a * 2 + t] = (*source.output_load)[a][t];
  arc.levelization = source.eligibility.levelization;
  arc.data_arrival = source.eligibility.data_arrival;
  arc.data_slew = source.eligibility.data_slew;
  for (const auto& source_child : source.transfer.children) {
    Child child;
    child.time_scale = source_child.time_unit_scale;
    child.capacitance_scale = source_child.capacitance_unit_scale;
    child.arc_index = source_child.signature.arc_index;
    child.sense = static_cast<std::uint8_t>(source_child.signature.sense);
    child.trigger = static_cast<std::uint8_t>(source_child.signature.trigger_transition);
    for (std::size_t t = 0; t < 2; ++t) {
      child.delay[t] = packTable(source_child.delay_tables[t], tables);
      child.slew[t] = packTable(source_child.slew_tables[t], tables);
      for (Table* table : {&child.delay[t], &child.slew[t]}) {
        table->axis_offset[0] += table_base;
        table->axis_offset[1] += table_base;
        table->value_offset += table_base;
      }
      child.check_present[t] = source_child.check_tables[t].present;
    }
    children.push_back(child);
  }
  return arc;
}

HostData pack(const CsrReferenceInput& input)
{
  HostData data;
  packArcs(input, data);
  packSeeds(input, data);
  data.out_offsets = input.out_row_offsets;
  data.in_offsets = input.in_row_offsets;
  data.out_arcs = indices(input.out_arc_ids);
  data.in_arcs = indices(input.in_arc_ids);
  data.roots = indices(input.ordered_root_ids);
  data.ends = indices(input.ordered_end_ids);
  for (const auto& source : input.endpoint_required)
    data.endpoints.push_back({source.node.value(), source.clock_period,
                              {source.output_delay[0].value_or(0.0), source.output_delay[0].has_value()}});
  data.order.resize(data.nodes.size());
  data.node_levels.resize(data.nodes.size());
  data.level_offsets.resize(data.nodes.size() + 1);
  return data;
}

}  // namespace ista::csr::cuda_detail
