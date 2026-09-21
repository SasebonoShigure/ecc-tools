#include "CsrCudaPacking.hpp"

#include <algorithm>
#include <bit>
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

bool sameModel(const std::vector<ChildTimingArcModel>& a, const std::vector<ChildTimingArcModel>& b)
{
  const auto bits = [](double x, double y) { return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y); };
  const auto values = [&](const auto& x, const auto& y) { return std::ranges::equal(x, y, bits); };
  const auto table = [&](const TimingTableModel& x, const TimingTableModel& y) {
    return x.present == y.present && x.variable1 == y.variable1 && x.variable2 == y.variable2
           && std::ranges::equal(x.axes, y.axes, values) && values(x.values, y.values);
  };
  return std::ranges::equal(a, b, [&](const ChildTimingArcModel& x, const ChildTimingArcModel& y) {
    return x.signature == y.signature && bits(x.time_unit_scale, y.time_unit_scale)
           && bits(x.capacitance_unit_scale, y.capacitance_unit_scale) && bits(x.slew_derate, y.slew_derate)
           && values(x.slew_lower_threshold_pct, y.slew_lower_threshold_pct)
           && values(x.slew_upper_threshold_pct, y.slew_upper_threshold_pct)
           && values(x.input_threshold_pct, y.input_threshold_pct) && values(x.output_threshold_pct, y.output_threshold_pct)
           && std::ranges::equal(x.delay_tables, y.delay_tables, table)
           && std::ranges::equal(x.slew_tables, y.slew_tables, table) && std::ranges::equal(x.check_tables, y.check_tables, table);
  });
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

Arc packArcFields(const auto& source, ModelStorage& models)
{
  Arc arc;
  arc.source = source.source.value();
  arc.sink = source.sink.value();
  arc.child_begin = models.intern(source.transfer.childrenOwner());
  arc.child_count = source.transfer.children().size();
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
  return arc;
}

void packArcs(const CsrReferenceInput& input, HostData& data, ModelStorage& models)
{
  for (const auto& source : input.arcs) {
    data.arcs.push_back(packArcFields(source, models));
    auto& arc = data.arcs.back();
    arc.child_state_begin = data.child_state_count;
    data.child_state_count += arc.child_count;
    data.layout.arcs.push_back({arc.child_state_begin, arc.child_count, true});
    for (const auto& child : source.transfer.children())
      data.layout.child_indices.push_back(child.signature.arc_index);
  }
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

NodeState& seedNode(HostData& data, Index node)
{
  auto found = std::ranges::lower_bound(data.node_seeds, node, {}, &NodeSeed::node);
  if (found == data.node_seeds.end() || found->node != node)
    found = data.node_seeds.insert(found, {node, {}});
  return found->state;
}

void packSeeds(const CsrReferenceInput& input, HostData& data)
{
  CsrInitialTimingSeeds seeds;
  const FlatTimingPayloadView payloads(input);
  if (collectCsrInitialTimingSeeds(payloads, seeds) != CsrCpuReferenceStageStatus::kSuccess)
    throw std::invalid_argument("CUDA timing seed initialization failed");
  data.tag_count = input.clock_tags.size();
  data.clock_count = input.clocks.size();
  data.node_count = input.nodes.size();
  for (const auto& source : seeds.nodes) {
    const auto i = source.node.value();
    auto& node = seedNode(data, i);
    node.arrival = source.arrival;
    node.launch = source.launch;
    node.clock_name = intern(data.layout.clock_names, source.clock_name);
    node.clock_point = source.clock_point;
    if (input.clock_owner_by_node[i]) {
      const auto owner = std::ranges::find(input.clocks, *input.clock_owner_by_node[i], &ClockConstraintModel::map_key);
      if (owner == input.clocks.end())
        throw std::invalid_argument("CUDA clock owner has no constraint");
      node.clock_owner = owner - input.clocks.begin();
    }
    for (std::size_t lane = 0; lane < 4; ++lane) {
      if (source.data_start)
        node.slew[lane] = {source.slew[lane], true};
      if (source.clock_point) {
        node.effective_arrival[lane] = {0.0, true};
        node.effective_slew[lane] = {0.0, true};
      }
      if (source.physical_clock_source) {
        node.physical_clock[lane].arrival = {0.0, true};
        node.physical_clock[lane].slew = {0.0, true};
      }
    }
  }
  for (const auto& seed : seeds.paths) {
    const auto& source = seed.path;
    if (source.clock_tag.value() >= data.tag_count || source.crpr_node || source.crpr_transition)
      throw std::invalid_argument("CUDA timing seed has unsupported path identity");
    auto& path = data.path_seeds.emplace_back(seed.row, source.clock_tag.value(), PathState{}).state;
    path.present = true;
    path.arrival = source.arrival;
    path.slew = source.slew;
    path.launch = source.launch;
    path.start = source.start_node.value();
  }
  data.layout.clock_tags = input.clock_tags;
  data.schema = input.schema_version;
  data.identity = input.source_identity;
}

void packSequential(const CsrReferenceInput& input, HostData& data, ModelStorage& models)
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
    if (source.seed_node.value() >= data.node_count)
      throw std::invalid_argument("CUDA sequential seed node out of range");
    const auto& node = input.nodes[source.seed_node.value()];
    const auto& cell = cellFor(node);
    const auto selected = std::ranges::find_if(cell.cell_arcs, [](const LibraryCellArcModel& arc) { return arc.transfer.is_clock_arc; });
    const CellTransferModel* ctq = selected == cell.cell_arcs.end() ? nullptr : &selected->transfer;
    auto& state = seedNode(data, source.seed_node.value());
    auto& seed = state.sequential;
    seed.clock_node = clockFor(node, cell);
    seed.kind = seed.clock_node == source.seed_node.value() ? SeedKind::clock : SeedKind::scalar;
    const bool has_output = cell.present && (ctq != nullptr || std::ranges::any_of(cell.ports, [](const LibraryPortModel& port) {
      return port.is_output && !port.is_clock;
    }));
    if (has_output && node.key.pin_name == cell.output_pin_name)
      seed.kind = SeedKind::output;
    seed.trigger = ctq != nullptr && !ctq->children().empty() && ctq->children().front().signature.trigger_transition == Transition::kFall ? 1 : 0;
    state.clock_name = intern(data.layout.clock_names, source.clock_tag);
    if (source.path_source) {
      const auto tag = std::ranges::find(input.clock_tags, source.clock_tag);
      if (*source.path_source != PathSource::kRegister || tag == input.clock_tags.end() || source.path_start_node.value() >= data.node_count
          || seed.clock_node == absent || source.crpr_node != NodeId{seed.clock_node}
          || source.crpr_transition != static_cast<Transition>(seed.trigger) || seed.kind == SeedKind::scalar)
        throw std::invalid_argument("CUDA sequential path identity is malformed");
      seed.path_tag = tag - input.clock_tags.begin();
      seed.path_start = source.path_start_node.value();
      for (Index analysis = 0; analysis < 2; ++analysis)
        for (Index transition = 0; transition < 2; ++transition)
          data.path_seeds.push_back({((source.seed_node.value() * 2 + analysis) * 2 + 1) * 2 + transition, seed.path_tag, {}});
    }
    if (seed.kind == SeedKind::output && ctq != nullptr) {
      seed.delay = ctq->delay;
      seed.delay_min = ctq->delay_min;
      seed.delay_max = ctq->delay_max;
      seed.child_begin = models.intern(ctq->childrenOwner());
      seed.child_count = ctq->children().size();
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
    endpoint.child_begin = models.intern(check.childrenOwner());
    endpoint.child_count = check.children().size();
    const auto* clock = input.clocks.empty() ? nullptr : &input.clocks.front();
    if (endpoint.clock_node != absent) {
      const auto found_seed = std::ranges::lower_bound(data.node_seeds, endpoint.clock_node, {}, &NodeSeed::node);
      const auto name = found_seed == data.node_seeds.end() || found_seed->node != endpoint.clock_node
                            ? absent : found_seed->state.clock_name;
      if (name != absent) {
        const auto found = std::ranges::find(input.clocks, data.layout.clock_names[name], &ClockConstraintModel::map_key);
        if (found != input.clocks.end())
          clock = &*found;
      }
    }
    endpoint.period = clock == nullptr ? 0.0 : clock->period;
    data.endpoints.push_back(endpoint);
  }
}

}  // namespace

std::uint8_t pathTransitions(ArcKind kind, const CellTransferModel& transfer, bool data_arrival)
{
  if (!data_arrival)
    return 0;
  if (kind == ArcKind::kNet || transfer.children().empty())
    return 0b1001;
  bool negative = false, positive = false, non_unate = false;
  for (const auto& child : transfer.children()) {
    negative |= child.signature.sense == TimingSense::kNegativeUnate;
    positive |= child.signature.sense == TimingSense::kPositiveUnate
                || child.signature.sense == TimingSense::kNoneUsesPositiveFallback;
    non_unate |= child.signature.sense == TimingSense::kNonUnate;
  }
  std::uint8_t mask = 0;
  for (unsigned input = 0; input < 2; ++input) {
    if (transfer.is_clock_arc && input != (transfer.children().front().signature.trigger_transition == Transition::kFall ? 1U : 0U))
      continue;
    for (unsigned output = 0; output < 2; ++output) {
      if (!(transfer.is_clock_arc || non_unate || (negative && positive))
          && output != (negative ? 1 - input : input))
        continue;
      for (const auto& child : transfer.children())
        if (child.delay_tables[output].present || child.slew_tables[output].present || child.check_tables[output].present) {
          mask |= 1U << (input * 2 + output);
          break;
        }
    }
  }
  return mask;
}

PathDomain buildPathDomain(std::span<const ArcBinding> arcs, std::span<const PathAdjacency> adjacency, std::span<const PathSeed> seeds)
{
  const auto rows = product(adjacency.size(), 8);
  if (rows == std::numeric_limits<std::size_t>::max())
    throw std::length_error("CUDA path row capacity overflow");
  std::vector<std::vector<Index>> tags(rows);
  std::vector<std::pair<Index, Index>> pending;
  const auto add = [&](Index row, Index tag) {
    if (row >= rows)
      throw std::invalid_argument("CUDA path seed row out of range");
    auto& values = tags[row];
    const auto found = std::ranges::lower_bound(values, tag);
    if (found == values.end() || *found != tag) {
      values.insert(found, tag);
      pending.emplace_back(row, tag);
    }
  };
  for (const auto& seed : seeds)
    add(seed.row, seed.tag);
  for (std::size_t head = 0; head < pending.size(); ++head) {
    const auto [row, tag] = pending[head];
    for (auto id : adjacency[row / 8].outgoing) {
      const auto& arc = arcs[id];
      if (!arc.active || !arc.path_transitions)
        continue;
      for (Index output = 0; output < 2; ++output)
        if (arc.path_transitions & (1U << ((row % 2) * 2 + output)))
          add(arc.sink * 8 + (row % 8 / 2) * 2 + output, tag);
    }
  }
  PathDomain result;
  result.offsets.reserve(rows + 1);
  result.tags.reserve(pending.size());
  result.offsets.push_back(0);
  for (const auto& row : tags) {
    result.tags.insert(result.tags.end(), row.begin(), row.end());
    result.offsets.push_back(result.tags.size());
  }
  return result;
}

std::vector<PathAdjacency> buildPathAdjacency(Index nodes, std::span<const ArcBinding> arcs)
{
  std::vector<PathAdjacency> result(nodes);
  for (Index id = 0; id < arcs.size(); ++id) {
    const auto& arc = arcs[id];
    if (arc.source >= nodes || arc.sink >= nodes)
      throw std::invalid_argument("CUDA path-domain endpoint out of range");
    result[arc.source].outgoing.push_back(id);
    result[arc.sink].incoming.push_back(id);
  }
  return result;
}

void updatePathDomain(std::span<const ArcBinding> arcs, std::span<const PathAdjacency> adjacency,
                      std::span<const PathSeed> seeds, const PathDomain& previous,
                      std::span<const Index> changed_nodes, PathDomain& result)
{
  const auto row_count = product(adjacency.size(), 8);
  if (row_count == std::numeric_limits<std::size_t>::max() || previous.offsets.empty()
      || previous.offsets.size() - 1 > row_count || &previous == &result)
    throw std::invalid_argument("CUDA incremental path-domain storage mismatch");
  // Only affected structural rows own temporary tag payloads. Clear their old
  // identities first so an edge removal can remove the last route to a tag.
  std::map<Index, std::array<std::vector<Index>, 8>> dirty;
  std::vector<Index> nodes;
  const auto mark = [&](Index node) {
    if (node >= adjacency.size())
      throw std::invalid_argument("CUDA changed path-domain node out of range");
    if (dirty.try_emplace(node).second)
      nodes.push_back(node);
  };
  for (Index node : changed_nodes)
    mark(node);
  for (std::size_t head = 0; head < nodes.size(); ++head)
    for (Index id : adjacency[nodes[head]].outgoing) {
      const auto& arc = arcs[id];
      if (arc.active && arc.path_transitions)
        mark(arc.sink);
    }

  std::vector<std::pair<Index, Index>> pending;
  const auto add = [&](Index row, Index tag) {
    auto& values = dirty.at(row / 8)[row % 8];
    const auto found = std::ranges::lower_bound(values, tag);
    if (found == values.end() || *found != tag) {
      values.insert(found, tag);
      pending.emplace_back(row, tag);
    }
  };
  for (const auto& seed : seeds)
    if (dirty.contains(seed.row / 8))
      add(seed.row, seed.tag);
  // Entry identities from unchanged predecessors survive; identities carried
  // solely around a now-unseeded dirty cycle do not.
  for (Index node : nodes)
    for (Index id : adjacency[node].incoming) {
      const auto& arc = arcs[id];
      if (!arc.active || !arc.path_transitions || dirty.contains(arc.source))
        continue;
      for (Index lane = 0; lane < 8; ++lane) {
        const Index source = arc.source * 8 + lane;
        if (source + 1 >= previous.offsets.size())
          continue;
        for (Index output = 0; output < 2; ++output)
          if (arc.path_transitions & (1U << ((lane % 2) * 2 + output)))
            for (Index slot = previous.offsets[source]; slot < previous.offsets[source + 1]; ++slot)
              add(node * 8 + (lane / 2) * 2 + output, previous.tags[slot]);
      }
    }
  for (std::size_t head = 0; head < pending.size(); ++head) {
    const auto [row, tag] = pending[head];
    for (Index id : adjacency[row / 8].outgoing) {
      const auto& arc = arcs[id];
      if (!arc.active || !arc.path_transitions)
        continue;
      for (Index output = 0; output < 2; ++output)
        if (arc.path_transitions & (1U << ((row % 2) * 2 + output)))
          add(arc.sink * 8 + (row % 8 / 2) * 2 + output, tag);
    }
  }
  // Compact offsets and payload positions are a full-domain packing cost,
  // distinct from incremental reachability and from AT/RAT propagation.
  result.offsets.resize(row_count + 1);
  result.tags.clear();
  for (Index node = 0; node < adjacency.size(); ++node) {
    const auto changed = dirty.find(node);
    for (Index lane = 0; lane < 8; ++lane) {
      const Index row = node * 8 + lane;
      result.offsets[row] = result.tags.size();
      if (changed != dirty.end()) {
        const auto& values = changed->second[lane];
        result.tags.insert(result.tags.end(), values.begin(), values.end());
      } else if (row + 1 < previous.offsets.size()) {
        result.tags.insert(result.tags.end(), previous.tags.begin() + previous.offsets[row], previous.tags.begin() + previous.offsets[row + 1]);
      }
    }
  }
  result.offsets.back() = result.tags.size();
}

Index ModelStorage::intern(const std::shared_ptr<const std::vector<ChildTimingArcModel>>& owner)
{
  if (!owner || owner->empty())
    return 0;
  if (const auto found = _owners.find(owner); found != _owners.end())
    return found->second;
  const auto& source = *owner;
  // Cheap bucket selection, followed by exact equality. Different cells may
  // reuse arc indices or even table signatures; neither permits aliasing.
  std::size_t hash = source.size();
  const auto mix = [&](std::size_t value) { hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
  for (const auto& child : source) {
    mix(std::hash<std::string>{}(child.signature.library_name));
    mix(std::hash<std::int32_t>{}(child.signature.arc_index));
    for (const auto* family : {&child.delay_tables, &child.slew_tables, &child.check_tables})
      for (const auto& table : *family) {
        mix(table.values.size());
        if (!table.values.empty()) {
          mix(std::hash<double>{}(table.values.front()));
          mix(std::hash<double>{}(table.values.back()));
        }
      }
  }
  const auto [first, last] = _lookup.equal_range(hash);
  for (auto found = first; found != last; ++found)
    if (sameModel(*_entries[found->second].source, source)) {
      const auto begin = _entries[found->second].begin;
      _owners.emplace(owner, begin);
      return begin;
    }
  Entry entry{owner, children.size()};
  std::vector<Child> packed;
  std::vector<double> values;
  packed.reserve(source.size());
  for (const auto& child : source)
    packed.push_back(packChild(child, values, tables.size()));
  // All allocating work precedes publishing the new immutable range.
  const auto reserveAppend = []<class T>(std::vector<T>& target, std::size_t added) {
    if (added > target.max_size() - target.size())
      throw std::length_error("CUDA immutable model capacity overflow");
    const auto required = target.size() + added;
    if (required > target.capacity())
      target.reserve(std::max(required, target.capacity() + std::min(target.capacity() / 2 + 1, target.max_size() - target.capacity())));
  };
  reserveAppend(_entries, 1);
  reserveAppend(children, packed.size());
  reserveAppend(tables, values.size());
  const auto owner_entry = _owners.emplace(owner, entry.begin).first;
  try {
    _lookup.emplace(hash, _entries.size());
  } catch (...) {
    _owners.erase(owner_entry);
    throw;
  }
  _entries.push_back(std::move(entry));
  children.insert(children.end(), packed.begin(), packed.end());
  tables.insert(tables.end(), values.begin(), values.end());
  return _entries.back().begin;
}

Arc packArc(const ArcSemanticPayload& source, ModelStorage& models)
{
  return packArcFields(source, models);
}

HostData pack(const CsrReferenceInput& input, ModelStorage& models)
{
  HostData data;
  packArcs(input, data, models);
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
  packSequential(input, data, models);
  data.bindings.reserve(input.arcs.size());
  for (const auto& arc : input.arcs) {
    data.bindings.push_back({arc.source.value(), arc.sink.value(), true, arc.eligibility.levelization,
                        EdgeStorageKind::kBaseOut, pathTransitions(arc.kind, arc.transfer, arc.eligibility.data_arrival)});
  }
  data.adjacency = buildPathAdjacency(input.nodes.size(), data.bindings);
  data.paths = buildPathDomain(data.bindings, data.adjacency, data.path_seeds);
  return data;
}

}  // namespace ista::csr::cuda_detail
