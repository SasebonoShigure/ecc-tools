// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrReferenceInputValidatorInternal.hpp"
#include "CsrReferenceValidatorDetail.hpp"
#include "CsrParasiticTree.hpp"

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ista::csr::detail {
namespace {
std::map<std::string, CsrParasiticTree> expectedTrees(const CsrReferenceInput& input)
{
  std::vector<ArcOutputLoadMatrix> caps(input.nodes.size());
  for (const auto& cap : input.cell_pin_capacitances) {
    if (cap.node.value() >= caps.size()) throw std::invalid_argument("RC pin is out of range");
    for (std::size_t a = 0; a < kAnalysisCount; ++a)
      for (std::size_t t = 0; t < kTransitionCount; ++t)
        caps[cap.node.value()][a][t] = cap.transition_capacitance[a][t].value_or(cap.default_capacitance);
  }
  for (const auto& port : input.port_constraints) {
    if (!port.resolved_node) continue;
    if (port.resolved_node->value() >= caps.size()) throw std::invalid_argument("RC port is out of range");
    for (auto& row : caps[port.resolved_node->value()])
      row.fill(port.output_load.value_or(0.0));
  }
  struct Net { std::vector<CsrRcPin> pins; std::optional<NodeId> driver; };
  std::map<std::string, Net> nets;
  for (const auto& raw : input.parasitics.nets) nets.try_emplace(raw.map_key);
  for (const auto& node : input.nodes) {
    const auto found = nets.find(node.net_name);
    if (found == nets.end()) continue;
    if (node.id.value() >= caps.size()) throw std::invalid_argument("RC node is out of range");
    auto& net = found->second;
    net.pins.push_back({node.id, node.key.full_name, caps[node.id.value()]});
    if (node.direction == (node.is_port ? PinDirection::kInput : PinDirection::kOutput)) {
      if (net.driver) throw std::invalid_argument("RC net has multiple drivers");
      net.driver = node.id;
    }
  }
  std::map<std::string, CsrParasiticTree> result;
  for (const auto& raw : input.parasitics.nets) {
    const auto net = nets.find(raw.map_key);
    if (net == nets.end() || !net->second.driver)
      throw std::invalid_argument("RC net has no driver");
    if (!result.emplace(raw.map_key, prepareCsrParasiticTree(raw, *net->second.driver, net->second.pins)).second)
      throw std::invalid_argument("duplicate RC net");
  }
  return result;
}
}  // namespace

bool validateReferenceParasitics(const CsrReferenceInput& input, Violations& errors)
{
  // Public arbitrary input / ON proof only. Internal export already prepares
  // each affected tree; OFF does not perform a second model reduction.
  std::map<std::string, CsrParasiticTree> expected;
  bool supported = true;
  try {
    if (!input.parasitics.nets.empty()) expected = expectedTrees(input);
  } catch (const std::invalid_argument&) {
    // Unsupported raw parasitics remain exportable, without prepared bindings.
    supported = false;
  } catch (const std::overflow_error&) {
    reject(errors, "parasitics", "RC reduction overflow");
    supported = false;
  }
  std::set<std::pair<const CsrParasiticTree*, const CsrParasiticTree*>> compared;
  const auto check = [&](const CsrParasiticBinding& binding, NodeId sink, std::optional<NodeId> driver,
                         const char* field) {
    const CsrParasiticTree* tree = nullptr;
    if (supported && sink.value() < input.nodes.size()) {
      const auto found = expected.find(input.nodes[sink.value()].net_name);
      if (found != expected.end()) tree = &found->second;
    }
    if (!tree) {
      if (binding.tree || binding.terminal != 0)
        reject(errors, field, "binding exists without a supported RC tree");
      return;
    }
    if (!binding.tree || binding.terminal >= tree->terminals.size()
        || tree->terminals[binding.terminal] != sink
        || (driver && tree->terminals.front() != *driver)) {
      reject(errors, field, "RC driver or terminal binding differs from raw net");
      return;
    }
    const auto& actual = *binding.tree;
    if (!compared.emplace(&actual, tree).second) return;
    if (actual.net_name != tree->net_name || actual.terminals != tree->terminals)
      reject(errors, field, "RC tree identity differs from raw net");
    for (std::size_t lane = 0; lane < tree->lanes.size(); ++lane)
      if (!actual.lanes[lane] || !(*actual.lanes[lane] == *tree->lanes[lane]))
        reject(errors, field, "RC model differs from raw tree and pin capacitance");
  };
  for (const auto& arc : input.arcs)
    check(arc.parasitic, arc.sink, arc.kind == ArcKind::kNet ? arc.source : arc.sink, "arcs.parasitic");
  for (const auto& seed : input.start_seeds) {
    const auto tree = seed.seed_node.value() < input.nodes.size()
        ? expected.find(input.nodes[seed.seed_node.value()].net_name) : expected.end();
    if (supported && seed.availability == StartSeedAvailability::kDeferredSequential
        && tree != expected.end() && tree->second.terminals.front() == seed.seed_node)
      check(seed.parasitic, seed.seed_node, seed.seed_node, "start_seeds.parasitic");
    else if (seed.parasitic.tree || seed.parasitic.terminal != 0)
      reject(errors, "start_seeds.parasitic", "only deferred sequential drivers own RC");
  }
  return supported;
}
}  // namespace ista::csr::detail
