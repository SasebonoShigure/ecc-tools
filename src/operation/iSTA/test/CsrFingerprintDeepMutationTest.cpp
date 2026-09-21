// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "CsrModelTestMutation.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CsrFingerprint.hpp"

namespace {

using namespace ista::csr;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

NodeKey pin(std::string full_name, std::string pin_name)
{
  return NodeKey{NodeKind::kInstancePin, std::move(full_name), "u1", std::move(pin_name)};
}

LibraryArcSignature fullSignature()
{
  LibraryArcSignature signature;
  signature.library_name = "slow.lib";
  signature.arc_index = -1;
  signature.sense = TimingSense::kPositiveUnate;
  signature.trigger_transition = Transition::kRise;
  signature.check_transition = Transition::kFall;
  signature.sdf_condition = "A & EN";
  for (auto& table : signature.delay_tables)
    table = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance};
  for (auto& table : signature.slew_tables)
    table = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance};
  return signature;
}

TimingTableModel fullTable(double offset)
{
  TimingTableModel table;
  table.present = true;
  table.variable1 = TableVariable::kInputTransition;
  table.variable2 = TableVariable::kOutputCapacitance;
  table.axes = {{offset + 1.0, offset + 2.0}, {offset + 3.0, offset + 4.0}};
  table.values = {offset + 5.0, offset + 6.0, offset + 7.0, offset + 8.0};
  return table;
}

CsrReferenceInput inputFixture()
{
  CsrReferenceInput input;
  input.nodes = {NodeRecord{.id = NodeId{0}, .key = port("a"), .direction = PinDirection::kInput, .is_port = true},
                 NodeRecord{.id = NodeId{1}, .key = pin("u1:Y", "Y"), .direction = PinDirection::kOutput, .is_output_pin = true}};

  ChildTimingArcModel child;
  child.signature = fullSignature();
  for (std::size_t index = 0; index < kTransitionCount; ++index) {
    child.delay_tables[index] = fullTable(static_cast<double>(index) * 10.0);
    child.slew_tables[index] = fullTable(20.0 + static_cast<double>(index) * 10.0);
    child.check_tables[index] = fullTable(40.0 + static_cast<double>(index) * 10.0);
  }
  LibraryArcSignature second_signature = child.signature;
  second_signature.library_name = "fast.lib";
  second_signature.arc_index = 1;
  ChildTimingArcModel second_child = child;
  second_child.signature = second_signature;

  ArcKeyBase base;
  base.source = input.nodes[0].key;
  base.sink = input.nodes[1].key;
  base.kind = ArcKind::kCell;
  base.owner_name = "u1";
  base.library_source_port = "A";
  base.library_sink_port = "Y";
  base.library_arcs = {child.signature, second_signature};

  ArcRecord arc;
  arc.id = ArcId{0};
  arc.key = {base, 0};
  arc.source = NodeId{0};
  arc.sink = NodeId{1};
  arc.kind = ArcKind::kCell;
  arc.transfer.setChildren({child, second_child});
  input.arcs = {arc};
  input.arc_ids_by_key = {{arc.key, arc.id}};
  return input;
}

template <typename Mutate>
void requireMutation(const CsrReferenceInput& baseline, const std::string& name, Mutate mutate)
{
  const auto baseline_bytes = canonicalReferenceInputBytes(baseline);
  CsrReferenceInput changed = baseline;
  mutate(changed);
  const auto changed_bytes = canonicalReferenceInputBytes(changed);
  require(changed_bytes != baseline_bytes, name + " did not change canonical bytes");
}

enum class KeySite
{
  kArcRecord,
  kReverseLookup
};

ArcKey& keyAt(CsrReferenceInput& input, KeySite site)
{
  if (site == KeySite::kArcRecord)
    return input.arcs[0].key;
  return input.arc_ids_by_key[0].key;
}

void testArcKeyFieldFamilies()
{
  const CsrReferenceInput baseline = inputFixture();
  const std::array<std::pair<KeySite, std::string_view>, 2> sites
      = {std::pair{KeySite::kArcRecord, std::string_view{"arc key"}}, std::pair{KeySite::kReverseLookup, std::string_view{"lookup key"}}};
  for (const auto& [site, prefix] : sites) {
    const auto check = [&](std::string_view field, auto mutate) {
      requireMutation(baseline, std::string(prefix) + " " + std::string(field), [=](auto& input) { mutate(keyAt(input, site)); });
    };
    check("source kind", [](auto& key) { key.base.source.kind = NodeKind::kInstancePin; });
    check("source full name", [](auto& key) { key.base.source.full_name = "b"; });
    check("source instance", [](auto& key) { key.base.source.instance_name = "u2"; });
    check("source pin", [](auto& key) { key.base.source.pin_name = "B"; });
    check("sink kind", [](auto& key) { key.base.sink.kind = NodeKind::kPort; });
    check("sink full name", [](auto& key) { key.base.sink.full_name = "u1:Z"; });
    check("sink instance", [](auto& key) { key.base.sink.instance_name = "u2"; });
    check("sink pin", [](auto& key) { key.base.sink.pin_name = "Z"; });
    check("kind", [](auto& key) { key.base.kind = ArcKind::kNet; });
    check("owner", [](auto& key) { key.base.owner_name = "u2"; });
    check("library source port", [](auto& key) { key.base.library_source_port = "B"; });
    check("library sink port", [](auto& key) { key.base.library_sink_port = "Z"; });
    check("library arc count", [](auto& key) { key.base.library_arcs.clear(); });
    check("library arc order", [](auto& key) { std::ranges::reverse(key.base.library_arcs); });
    check("parallel ordinal", [](auto& key) { ++key.parallel_ordinal; });
  }
}

enum class SignatureSite
{
  kArcKey,
  kChild,
  kReverseLookup
};

void mutateSignature(CsrReferenceInput& input, SignatureSite site, const auto& mutate)
{
  switch (site) {
    case SignatureSite::kArcKey:
      mutate(input.arcs[0].key.base.library_arcs[0]);
      return;
    case SignatureSite::kChild:
      ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) { mutate(children[0].signature); });
      return;
    case SignatureSite::kReverseLookup:
      mutate(input.arc_ids_by_key[0].key.base.library_arcs[0]);
      return;
  }
  throw std::runtime_error("unreachable signature site");
}

void testLibrarySignatureFieldFamilies()
{
  const CsrReferenceInput baseline = inputFixture();
  const std::array<std::pair<SignatureSite, std::string_view>, 3> sites
      = {std::pair{SignatureSite::kArcKey, std::string_view{"arc signature"}}, std::pair{SignatureSite::kChild, std::string_view{"child signature"}},
         std::pair{SignatureSite::kReverseLookup, std::string_view{"lookup signature"}}};
  using SignatureTables = std::array<TimingTableSignature, kTransitionCount> LibraryArcSignature::*;
  const std::array<std::pair<SignatureTables, std::string_view>, 2> families
      = {std::pair{&LibraryArcSignature::delay_tables, std::string_view{"delay"}}, std::pair{&LibraryArcSignature::slew_tables, std::string_view{"slew"}}};

  for (const auto& [site, prefix] : sites) {
    const auto check = [&](std::string_view field, auto mutate) {
      requireMutation(baseline, std::string(prefix) + " " + std::string(field), [=](auto& input) { mutateSignature(input, site, mutate); });
    };
    check("library name", [](auto& signature) { signature.library_name = "fast.lib"; });
    check("signed arc index", [](auto& signature) { signature.arc_index = 0; });
    check("sense", [](auto& signature) { signature.sense = TimingSense::kNegativeUnate; });
    check("trigger", [](auto& signature) { signature.trigger_transition = Transition::kFall; });
    check("check", [](auto& signature) { signature.check_transition = Transition::kRise; });
    check("sdf condition", [](auto& signature) { signature.sdf_condition = "!A"; });
    for (const auto& [member, family] : families) {
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
        const std::string lane = std::string(family) + " " + std::to_string(transition);
        check(lane + " presence", [=](auto& signature) { (signature.*member)[transition].present = false; });
        check(lane + " variable1", [=](auto& signature) { (signature.*member)[transition].variable1 = TableVariable::kNone; });
        check(lane + " variable2", [=](auto& signature) { (signature.*member)[transition].variable2 = TableVariable::kConstrainedTransition; });
      }
    }
  }
}

void testTimingTableModelFieldFamilies()
{
  const CsrReferenceInput baseline = inputFixture();
  using ModelTables = std::array<TimingTableModel, kTransitionCount> ChildTimingArcModel::*;
  const std::array<std::pair<ModelTables, std::string_view>, 3> families = {std::pair{&ChildTimingArcModel::delay_tables, std::string_view{"delay model"}},
                                                                            std::pair{&ChildTimingArcModel::slew_tables, std::string_view{"slew model"}},
                                                                            std::pair{&ChildTimingArcModel::check_tables, std::string_view{"check model"}}};
  for (const auto& [member, family] : families) {
    for (std::size_t transition = 0; transition < kTransitionCount; ++transition) {
      const std::string prefix = std::string(family) + " " + std::to_string(transition);
      const auto check = [&](std::string_view field, auto mutate) {
        requireMutation(baseline, prefix + " " + std::string(field), [=](auto& input) {
          ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) { auto& table = (children[0].*member)[transition];
          mutate(table); });
        });
      };
      check("presence", [](auto& table) { table.present = false; });
      check("variable1", [](auto& table) { table.variable1 = TableVariable::kNone; });
      check("variable2", [](auto& table) { table.variable2 = TableVariable::kConstrainedTransition; });
      check("axes outer length", [](auto& table) { table.axes.pop_back(); });
      check("axis inner length", [](auto& table) { table.axes[0].pop_back(); });
      check("axis value", [](auto& table) { table.axes[0][0] += 0.25; });
      check("axis inner order", [](auto& table) { std::ranges::reverse(table.axes[0]); });
      check("axes order", [](auto& table) { std::ranges::reverse(table.axes); });
      check("values length", [](auto& table) { table.values.pop_back(); });
      check("values value", [](auto& table) { table.values[0] += 0.25; });
      check("values order", [](auto& table) { std::ranges::reverse(table.values); });
    }
  }
  requireMutation(baseline, "child arc container length", [](auto& input) { input.arcs[0].transfer.setChildren({}); });
  requireMutation(baseline, "child arc container order", [](auto& input) { ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) { std::ranges::reverse(children); }); });
}

void testTimingTableStateDistinctions()
{
  const CsrReferenceInput baseline = inputFixture();
  const auto encode = [&](const TimingTableModel& table) {
    CsrReferenceInput input = baseline;
    ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0] = table; });
    return canonicalReferenceInputBytes(input);
  };

  TimingTableModel missing;
  TimingTableModel present_empty;
  present_empty.present = true;
  TimingTableModel constant;
  constant.present = true;
  constant.values = {1.0};
  TimingTableModel one_dimensional;
  one_dimensional.present = true;
  one_dimensional.variable1 = TableVariable::kInputTransition;
  one_dimensional.axes = {{1.0, 2.0}};
  one_dimensional.values = {3.0, 4.0};
  const TimingTableModel two_dimensional = fullTable(0.0);

  const std::array states = {encode(missing), encode(present_empty), encode(constant), encode(one_dimensional), encode(two_dimensional)};
  for (std::size_t lhs = 0; lhs < states.size(); ++lhs)
    for (std::size_t rhs = lhs + 1; rhs < states.size(); ++rhs) {
      require(states[lhs] != states[rhs], "missing/present-empty/0D/1D/2D table states must have distinct canonical bytes");
    }
}

}  // namespace

int main()
{
  testArcKeyFieldFamilies();
  testLibrarySignatureFieldFamilies();
  testTimingTableModelFieldFamilies();
  testTimingTableStateDistinctions();
  return 0;
}
