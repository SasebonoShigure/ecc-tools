#include "CsrCudaPacking.hpp"
#include "FlatTimingPayloadView.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace ista::csr::cuda_detail {

void packRcTree(const CsrParasiticTree& source, PackedRcTree& target)
{
  if (source.terminals.empty())
    throw std::invalid_argument("CUDA RC tree has no driver");
  // At most four immutable lanes. Equal owners share terminal payload without
  // hashing or copying the tree; changed owners are prepared in the work slot.
  std::array<const CsrParasiticModel*, 4> owners{};
  target.models.clear();
  target.terminals.clear();
  target.models.reserve(4);
  for (std::size_t lane = 0; lane < 4; ++lane) {
    const auto* owner = source.lanes[lane].get();
    if (!owner)
      throw std::invalid_argument("CUDA RC lane has no model");
    const auto existing = std::find(owners.begin(), owners.begin() + target.models.size(), owner);
    if (existing != owners.begin() + target.models.size()) {
      target.lanes[lane] = existing - owners.begin();
      continue;
    }
    const auto& arnoldi = owner->arnoldi;
    const auto& dmp = owner->dmp;
    const auto count = source.terminals.size();
    if (arnoldi.order < 0 || arnoldi.order > 5 || owner->fallback.size() != count
        || (arnoldi.order && arnoldi.projection.size() != count)
        || (dmp.valid && dmp.loads.size() != count))
      throw std::invalid_argument("CUDA RC terminal extent mismatch");
    if (count > target.terminals.max_size() - target.terminals.size())
      throw std::length_error("CUDA RC terminal capacity overflow");
    RcModel model;
    model.terminal_begin = target.terminals.size();
    model.terminal_count = count;
    model.order = arnoldi.order;
    std::copy(arnoldi.diagonal.begin(), arnoldi.diagonal.end(), model.diagonal);
    std::copy(arnoldi.off_diagonal.begin(), arnoldi.off_diagonal.end(), model.off_diagonal);
    model.total_capacitance = arnoldi.total_capacitance;
    model.sqrt_total_capacitance = arnoldi.sqrt_total_capacitance;
    model.root_load = owner->root_load;
    model.dmp_valid = dmp.valid;
    model.driver_capacitance = dmp.driver_capacitance;
    model.resistance = dmp.resistance;
    model.load_capacitance = dmp.load_capacitance;
    for (std::size_t index = 0; index < count; ++index) {
      RcTerminal terminal;
      if (arnoldi.order)
        std::copy(arnoldi.projection[index].begin(), arnoldi.projection[index].end(), terminal.projection);
      if (dmp.valid) {
        const auto& load = dmp.loads[index];
        terminal.dmp_order = load.order;
        std::copy(load.poles.begin(), load.poles.end(), terminal.poles);
        std::copy(load.residues.begin(), load.residues.end(), terminal.residues);
      }
      const auto& fallback = owner->fallback[index];
      terminal.fallback_delay = fallback.delay;
      terminal.fallback_impulse = fallback.impulse;
      terminal.fallback_valid = fallback.valid;
      target.terminals.push_back(terminal);
    }
    target.lanes[lane] = target.models.size();
    owners[target.models.size()] = owner;
    target.models.push_back(model);
  }
}

Index ModelStorage::internLoadCell(const ReferencedLibraryCellModel& cell)
{
  RcLoadCell value;
  value.library = libraryIndex(cell.library_name);
  value.slew_derate = cell.slew_derate;
  std::copy(cell.slew_lower_threshold_pct.begin(), cell.slew_lower_threshold_pct.end(), value.lower);
  std::copy(cell.slew_upper_threshold_pct.begin(), cell.slew_upper_threshold_pct.end(), value.upper);
  std::copy(cell.input_threshold_pct.begin(), cell.input_threshold_pct.end(), value.input_threshold);
  if (!std::isfinite(value.slew_derate))
    throw std::invalid_argument("CUDA RC load-cell derate is non-finite");
  for (unsigned transition = 0; transition < 2; ++transition)
    if (!std::isfinite(value.lower[transition]) || !std::isfinite(value.upper[transition])
        || !std::isfinite(value.input_threshold[transition]))
      throw std::invalid_argument("CUDA RC load-cell threshold is non-finite");
  const auto key = std::pair{cell.library_name, cell.cell_name};
  if (const auto found = _load_cells.find(key); found != _load_cells.end()) {
    const auto& old = rc_load_cells[found->second];
    if (old.slew_derate != value.slew_derate || !std::ranges::equal(old.lower, value.lower)
        || !std::ranges::equal(old.upper, value.upper) || !std::ranges::equal(old.input_threshold, value.input_threshold))
      throw std::invalid_argument("CUDA immutable load-cell thresholds changed");
    return found->second;
  }
  if (rc_load_cells.size() == rc_load_cells.capacity())
    rc_load_cells.reserve(rc_load_cells.size() + std::min(rc_load_cells.size() + 1, rc_load_cells.max_size() - rc_load_cells.size()));
  const Index index = rc_load_cells.size();
  _load_cells.emplace(key, index);
  rc_load_cells.push_back(value);
  return index;
}

RcBinding packRcBinding(HostRcNet& net, Index net_index, Index terminal,
                        bool instance, Index ordinal, Index child_begin, Index child_count)
{
  RcBinding binding;
  binding.net = net_index;
  binding.terminal = terminal;
  if (child_count) {
    const auto [group, added] = net.groups.emplace(std::pair{instance, ordinal},
        RcChildGroup{net.child_count, child_count, child_begin});
    if (!added && (group->second.count != child_count || group->second.model != child_begin))
      throw std::invalid_argument("CUDA native child identity has different models");
    binding.child_begin = group->second.begin;
    if (added) {
      if (child_count > absent - net.child_count)
        throw std::length_error("CUDA RC child capacity overflow");
      net.child_count += child_count;
    }
  }
  return binding;
}

void finishRcNet(HostRcNet& net)
{
  if (net.child_count > net.rows.max_size() / 4)
    throw std::length_error("CUDA RC cache row capacity overflow");
  net.rows.resize(net.child_count * 4);
  net.record_count = 0;
  for (unsigned lane = 0; lane < 4; ++lane)
    for (const auto& [identity, group] : net.groups) {
      // Graph initialization, two clock and two data input transitions.
      const Index capacity = identity.first ? 1 : 5;
      for (Index child = 0; child < group.count; ++child) {
        if (capacity > absent - net.record_count)
          throw std::length_error("CUDA RC cache record capacity overflow");
        net.rows[lane * net.child_count + group.begin + child] = {net.record_count, capacity, 0};
        net.record_count += capacity;
      }
    }
}

namespace {
template <class ArcAt>
void packRcGraph(const TimingPayloadView& input, std::size_t arc_count, ArcAt arc_at, HostData& data, ModelStorage& models)
{
  std::map<Index, std::shared_ptr<const CsrParasiticTree>> owners;
  const auto retain = [&](const CsrParasiticBinding& binding) {
    if (!binding.tree) return;
    if (binding.tree->terminals.empty() || binding.terminal >= binding.tree->terminals.size())
      throw std::invalid_argument("CUDA RC binding outside tree");
    const auto driver = binding.tree->terminals.front().value();
    if (driver >= input.nodeRecords().size())
      throw std::invalid_argument("CUDA RC driver outside graph");
    const auto [found, added] = owners.emplace(driver, binding.tree);
    if (!added && found->second != binding.tree)
      throw std::invalid_argument("CUDA driver has inconsistent RC owner");
  };
  for (std::size_t id = 0; id < arc_count; ++id)
    if (const auto* arc = arc_at(id)) retain(arc->parasitic);
  for (const auto& seed : input.startSeeds()) retain(seed.parasitic);
  if (owners.empty()) return;
  std::map<std::string_view, const ReferencedLibraryCellModel*, std::less<>> cells;
  for (const auto& cell : input.referencedLibraryCells())
    cells.emplace(cell.cell_name, &cell);
  data.rc.net_by_node.assign(input.nodeRecords().size(), absent);
  data.rc_nets.reserve(owners.size());
  for (const auto& [driver, owner] : owners) {
    data.rc.net_by_node[driver] = data.rc_nets.size();
    auto& net = data.rc_nets.emplace_back();
    net.owner = owner;
    for (std::size_t terminal = 0; terminal < owner->terminals.size(); ++terminal) {
      const auto id = owner->terminals[terminal].value();
      const auto& node = input.nodeRecords().at(id);
      HostRcNode value{{owner, terminal}, node.is_port, absent};
      if (!node.is_port) {
        const auto found = cells.find(node.instance_cell);
        if (found == cells.end())
          throw std::invalid_argument("CUDA RC terminal has no referenced cell");
        if (found->second->present) value.load_cell = models.internLoadCell(*found->second);
      }
      if (!data.rc.nodes.emplace(id, std::move(value)).second)
        throw std::invalid_argument("CUDA RC terminal belongs to multiple nets");
    }
  }

  for (std::size_t id = 0; id < arc_count; ++id) {
    const auto* active = arc_at(id);
    if (!active || !active->parasitic.tree) continue;
    const auto& source = *active;
    auto& arc = data.arcs.at(id);
    const auto driver = arc.net ? arc.source : arc.sink;
    if (!arc.net && source.parasitic.terminal != 0)
      throw std::invalid_argument("CUDA RC cell is not bound to its driver terminal");
    if (!arc.net && arc.child_count && !source.native_cell_arc_ordinal)
      throw std::invalid_argument("CUDA RC cell lacks native ordinal");
    const auto net_index = data.rc.net_by_node.at(driver);
    auto binding = packRcBinding(data.rc_nets.at(net_index), net_index, source.parasitic.terminal,
        false, source.native_cell_arc_ordinal.value_or(0), arc.child_begin, arc.child_count);
    if (arc.net) {
      binding.input_port = data.rc.nodes.at(arc.source).port;
      binding.load_cell = data.rc.nodes.at(arc.sink).load_cell;
    } else if (arc.child_count) {
      data.rc_nets[binding.net].initialize_arcs.push_back(id);
    }
    arc.rc_binding = data.rc.bindings.size();
    data.rc.bindings.push_back(binding);
  }
  for (const auto& source : input.startSeeds()) {
    if (!source.parasitic.tree || source.availability != StartSeedAvailability::kDeferredSequential)
      continue;
    const auto found = std::ranges::lower_bound(data.node_seeds, source.seed_node.value(), {}, &NodeSeed::node);
    if (found == data.node_seeds.end() || found->node != source.seed_node.value())
      throw std::invalid_argument("CUDA RC deferred seed has no node");
    auto& seed = found->state.sequential;
    if (seed.kind != SeedKind::output) continue;
    const auto net_index = data.rc.net_by_node.at(source.seed_node.value());
    const auto binding = packRcBinding(data.rc_nets.at(net_index), net_index, source.parasitic.terminal,
                                       true, 0, seed.child_begin, seed.child_count);
    seed.rc_binding = data.rc.bindings.size();
    data.rc.bindings.push_back(binding);
  }
  for (std::size_t index = 0; index < data.rc_nets.size(); ++index) {
    auto& net = data.rc_nets[index];
    std::ranges::sort(net.initialize_arcs, [&](Index a, Index b) {
      return std::pair{*arc_at(a)->native_cell_arc_ordinal, a}
          < std::pair{*arc_at(b)->native_cell_arc_ordinal, b};
    });
    finishRcNet(net);
  }
}

}  // namespace

void packRcGraph(const CsrReferenceInput& input, HostData& data, ModelStorage& models)
{
  packRcGraph(FlatTimingPayloadView(input), input.arcs.size(),
      [&](std::size_t id) { return &input.arcs[id]; }, data, models);
}

}  // namespace ista::csr::cuda_detail
