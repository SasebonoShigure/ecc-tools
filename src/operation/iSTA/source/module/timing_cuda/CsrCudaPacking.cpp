#include "CsrCudaPacking.hpp"

#include <algorithm>
#include <limits>
#include <map>
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

Child packChild(const ChildTimingArcModel& source, std::vector<double>& tables, Index table_base = 0)
{
  Child child;
  child.time_scale = source.time_unit_scale;
  child.capacitance_scale = source.capacitance_unit_scale;
  child.arc_index = source.signature.arc_index;
  child.sense = static_cast<std::uint8_t>(source.signature.sense);
  child.trigger = static_cast<std::uint8_t>(source.signature.trigger_transition);
  for (std::size_t t = 0; t < 2; ++t) {
    child.delay[t] = packTable(source.delay_tables[t], tables);
    child.slew[t] = packTable(source.slew_tables[t], tables);
    child.check[t] = packTable(source.check_tables[t], tables);
    for (Table* table : {&child.delay[t], &child.slew[t], &child.check[t]}) {
      table->axis_offset[0] += table_base;
      table->axis_offset[1] += table_base;
      table->value_offset += table_base;
    }
  }
  return child;
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
  data.arc_state.resize(data.arcs.size());
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
  data.clock_count = input.clocks.size();
  data.nodes.resize(input.nodes.size());
  data.paths.resize(product(product(input.nodes.size(), 8), data.tag_count));
  for (std::size_t i = 0; i < data.nodes.size(); ++i) {
    auto& node = data.nodes[i];
    node.arrival = seeds.result.nodes.arrivals.values[i];
    node.launch = seeds.result.nodes.launches.values[i];
    if (seeds.result.nodes.clock_names.presence[i])
      node.clock_name = intern(data.layout.clock_names, seeds.result.nodes.clock_names.values[i]);
    if (input.clock_owner_by_node[i]) {
      const auto owner = std::ranges::find(input.clocks, *input.clock_owner_by_node[i], &ClockConstraintModel::map_key);
      if (owner == input.clocks.end())
        throw std::invalid_argument("CUDA clock owner has no constraint");
      node.clock_owner = owner - input.clocks.begin();
    }
    for (std::size_t lane = 0; lane < 4; ++lane) {
      const auto flat = i * 4 + lane;
      node.slew[lane] = {seeds.result.data_slews.values[flat], seeds.result.data_slews.presence[flat] != 0};
      node.effective_arrival[lane] = {seeds.result.effective_clock.arrivals.values[flat], seeds.result.effective_clock.arrivals.presence[flat] != 0};
      node.effective_slew[lane] = {seeds.result.effective_clock.slews.values[flat], seeds.result.effective_clock.slews.presence[flat] != 0};
      node.physical_clock[lane].arrival = {seeds.result.physical_clock.arrivals.values[flat], seeds.result.physical_clock.arrivals.presence[flat] != 0};
      node.physical_clock[lane].slew = {seeds.result.physical_clock.slews.values[flat], seeds.result.physical_clock.slews.presence[flat] != 0};
    }
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
  data.layout.clock_tags = input.clock_tags;
  data.schema = input.schema_version;
  data.identity = input.source_identity;
}

void packSequential(const CsrReferenceInput& input, HostData& data)
{
  if (input.timing_check_endpoints.empty() && std::ranges::none_of(input.start_seeds, [](const StartSeedModel& seed) {
        return seed.availability == StartSeedAvailability::kDeferredSequential;
      }))
    return;
  std::map<std::string, const ReferencedLibraryCellModel*, std::less<>> cells;
  for (const auto& cell : input.referenced_library_cells)
    cells.emplace(cell.cell_name, &cell);
  std::map<std::string, Index, std::less<>> nodes;
  for (const auto& node : input.nodes)
    nodes.emplace(node.key.full_name, node.id.value());
  const auto cellFor = [&](const NodeRecord& node) -> const ReferencedLibraryCellModel& {
    const auto found = cells.find(node.instance_cell);
    if (found == cells.end())
      throw std::invalid_argument("CUDA sequential node has no referenced cell");
    return *found->second;
  };
  const auto clockFor = [&](const NodeRecord& node, const ReferencedLibraryCellModel& cell) {
    if (!cell.present || cell.timing_checks.empty())
      return absent;
    const auto found = nodes.find(node.key.instance_name + ":" + cell.clock_pin_name);
    return found == nodes.end() ? absent : found->second;
  };
  for (const auto& source : input.start_seeds) {
    if (source.availability != StartSeedAvailability::kDeferredSequential)
      continue;
    if (source.seed_node.value() >= data.nodes.size())
      throw std::invalid_argument("CUDA sequential seed node out of range");
    const auto& node = input.nodes[source.seed_node.value()];
    const auto& cell = cellFor(node);
    const auto selected = std::ranges::find_if(cell.cell_arcs, [](const LibraryCellArcModel& arc) { return arc.transfer.is_clock_arc; });
    const CellTransferModel* ctq = selected == cell.cell_arcs.end() ? nullptr : &selected->transfer;
    auto& state = data.nodes[source.seed_node.value()];
    auto& seed = state.sequential;
    seed.clock_node = clockFor(node, cell);
    seed.kind = seed.clock_node == source.seed_node.value() ? SeedKind::clock : SeedKind::scalar;
    const bool has_output = cell.present && (ctq != nullptr || std::ranges::any_of(cell.ports, [](const LibraryPortModel& port) {
      return port.is_output && !port.is_clock;
    }));
    if (has_output && node.key.pin_name == cell.output_pin_name)
      seed.kind = SeedKind::output;
    seed.trigger = ctq != nullptr && !ctq->children.empty() && ctq->children.front().signature.trigger_transition == Transition::kFall ? 1 : 0;
    state.clock_name = intern(data.layout.clock_names, source.clock_tag);
    if (source.path_source) {
      const auto tag = std::ranges::find(input.clock_tags, source.clock_tag);
      if (*source.path_source != PathSource::kRegister || tag == input.clock_tags.end() || source.path_start_node.value() >= data.nodes.size()
          || seed.clock_node == absent || source.crpr_node != NodeId{seed.clock_node}
          || source.crpr_transition != static_cast<Transition>(seed.trigger) || seed.kind == SeedKind::scalar)
        throw std::invalid_argument("CUDA sequential path identity is malformed");
      seed.path_tag = tag - input.clock_tags.begin();
      seed.path_start = source.path_start_node.value();
    }
    if (seed.kind == SeedKind::output && ctq != nullptr) {
      seed.delay = ctq->delay;
      seed.delay_min = ctq->delay_min;
      seed.delay_max = ctq->delay_max;
      seed.child_begin = data.children.size();
      seed.child_count = ctq->children.size();
      for (const auto& child : ctq->children)
        data.children.push_back(packChild(child, data.table_data));
    }
  }
  std::vector<bool> selected(input.nodes.size(), false);
  for (const auto& source : input.timing_check_endpoints) {
    if (!source.resolved_data_node || (source.check_kind != TimingCheckKind::kSetup && source.check_kind != TimingCheckKind::kRecovery))
      continue;
    const auto id = source.resolved_data_node->value();
    if (id >= input.nodes.size())
      throw std::invalid_argument("CUDA check endpoint node out of range");
    const auto& node = input.nodes[id];
    if (selected[id] || node.is_port || !node.is_sequential || !node.is_endpoint)
      continue;
    selected[id] = true;
    const auto& cell = cellFor(node);
    if (source.referenced_check_ordinal >= cell.timing_checks.size())
      throw std::invalid_argument("CUDA check ordinal out of range");
    const auto& check = cell.timing_checks[source.referenced_check_ordinal];
    Endpoint endpoint;
    endpoint.node = id;
    endpoint.sequential_check = true;
    endpoint.clock_node = clockFor(node, cell);
    endpoint.clock_transition = static_cast<std::uint8_t>(check.clock_transition);
    endpoint.check_time = check.check_time;
    endpoint.child_begin = data.children.size();
    endpoint.child_count = check.children.size();
    const auto* clock = input.clocks.empty() ? nullptr : &input.clocks.front();
    if (endpoint.clock_node != absent) {
      const auto name = data.nodes[endpoint.clock_node].clock_name;
      if (name != absent) {
        const auto found = std::ranges::find(input.clocks, data.layout.clock_names[name], &ClockConstraintModel::map_key);
        if (found != input.clocks.end())
          clock = &*found;
      }
    }
    endpoint.period = clock == nullptr ? 0.0 : clock->period;
    for (const auto& child : check.children)
      data.children.push_back(packChild(child, data.table_data));
    data.endpoints.push_back(endpoint);
  }
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
  arc.clock_eligible = source.eligibility.clock;
  for (const auto& child : source.transfer.children)
    children.push_back(packChild(child, tables, table_base));
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
  packSequential(input, data);
  data.child_state.resize(data.children.size());
  for (const auto& child : data.children)
    data.layout.child_indices.push_back(child.arc_index);
  data.order.resize(data.nodes.size());
  data.node_levels.resize(data.nodes.size());
  data.level_offsets.resize(data.nodes.size() + 1);
  return data;
}

}  // namespace ista::csr::cuda_detail
