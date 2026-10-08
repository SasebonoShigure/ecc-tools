// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "NativeTargetedEditInternal.hpp"

#include "CsrParasiticTree.hpp"
#include "Database.hpp"

namespace ista::timing_edit::detail {
namespace {
void addTree(std::map<csr::NodeId, csr::CsrParasiticBinding>& bindings,
             const std::shared_ptr<const csr::CsrParasiticTree>& tree)
{
  if (!tree || tree->terminals.empty())
    throw std::invalid_argument("targeted RC tree has no driver");
  const auto found = bindings.find(tree->terminals.front());
  if (found != bindings.end()) {
    if (found->second.tree != tree)
      throw std::invalid_argument("targeted RC driver has conflicting owners");
    return;
  }
  for (std::size_t slot = 0; slot < tree->terminals.size(); ++slot)
    if (!bindings.emplace(tree->terminals[slot], csr::CsrParasiticBinding{tree, slot}).second)
      throw std::invalid_argument("targeted RC terminal belongs to multiple nets");
}
csr::ArcOutputLoadMatrix pinLoad(const Database& database, csr::NodeId node, const Pin& pin)
{
  csr::ArcOutputLoadMatrix result{};
  if (pin.get_is_port()) {
    const auto& constraints = database.get_timing_constraint().get_port_constraint_map();
    const auto found = constraints.find(pin.get_full_name());
    const double value = found != constraints.end() && found->second.get_has_load() ? found->second.get_load() : 0.0;
    for (auto& analysis : result) analysis.fill(value);
  } else {
    const csr::NodeKey key{csr::NodeKind::kInstancePin, pin.get_full_name(), pin.get_instance_name(), pin.get_pin_name()};
    const auto value = csr::detail::projectCanonicalCellPinCapacitance(database, node, key);
    for (std::size_t a = 0; a < csr::kAnalysisCount; ++a)
      for (std::size_t t = 0; t < csr::kTransitionCount; ++t)
        result[a][t] = value.transition_capacitance[a][t].value_or(value.default_capacitance);
  }
  return result;
}
}
void bindTargetedBaselineParasitics(NativeTargetedEditState& state, const csr::CsrReferenceInput& input)
{
  for (const auto& arc : input.arcs)
    if (arc.parasitic.tree) addTree(state.parasitics, arc.parasitic.tree);
  for (const auto& seed : input.start_seeds)
    if (seed.parasitic.tree) addTree(state.parasitics, seed.parasitic.tree);
}

std::map<csr::NodeId, csr::CsrParasiticBinding> prepareTargetedParasitics(
    const Database& database, const NativeTargetedEditState& state, const std::vector<std::string>& nets)
{
  std::map<csr::NodeId, csr::CsrParasiticBinding> bindings;
  for (const auto& name : nets) {
    const auto& net = database.get_net_map().at(name);
    const auto driver = state.node_ids.at(net.get_driver_pin());
    std::vector<csr::CsrRcPin> pins;
    pins.reserve(net.get_pin_name_list().size());
    for (const auto& name : net.get_pin_name_list()) {
      const auto node = state.node_ids.at(name);
      const auto& pin = database.get_pin_map().at(name);
      if (pin.get_net_name() != net.get_net_name())
        throw std::invalid_argument("targeted RC pin/net membership mismatch");
      pins.push_back({node, pin.get_full_name(), pinLoad(database, node, pin)});
    }
    const auto raw = csr::detail::projectCanonicalParasiticNet(name, database.get_parasitic_library().get_net_map().at(name));
    addTree(bindings, std::make_shared<const csr::CsrParasiticTree>(csr::prepareCsrParasiticTree(raw, driver, pins)));
  }
  return bindings;
}

void PreparedNativeTargetedMetadata::applyParasitics(std::map<csr::NodeId, csr::CsrParasiticBinding> bindings)
{
  if (!_parasitics.empty() || !_new_parasitics.empty())
    throw std::logic_error("targeted RC bindings already installed");
  std::vector<csr::NodeId> appended;
  appended.reserve(bindings.size());
  for (const auto& [node, binding] : bindings) {
    static_cast<void>(binding);
    if (!_state.parasitics.contains(node)) appended.push_back(node);
  }
  // All allocations precede owner swaps. Old shared owners remain in the undo
  // until every backend consumer has finished or rollback restores them.
  _parasitics = std::move(bindings);
  _new_parasitics = std::move(appended);
  for (auto& [node, binding] : _parasitics)
    if (const auto previous = _state.parasitics.find(node); previous != _state.parasitics.end())
      std::swap(previous->second, binding);
  _state.parasitics.merge(_parasitics);
}
}  // namespace ista::timing_edit::detail
