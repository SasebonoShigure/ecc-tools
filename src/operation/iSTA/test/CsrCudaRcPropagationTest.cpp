#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

#include "CsrCpuReferenceTestFixture.hpp"
#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceBackendInternal.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"
#include "CudaDualCsrTestReader.hpp"
#include "CsrRcTimingComparison.hpp"

namespace ista::csr {
namespace {
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}

CsrReferenceInput rcFixture(unsigned family, bool cross_library, unsigned mode = 0)
{
  auto input = test::makeCpuReferenceCellFixture();
  auto endpoint = input.nodes[0];
  endpoint.id = NodeId{3};
  endpoint.key.full_name = "out";
  endpoint.key.pin_name.clear();
  endpoint.direction = PinDirection::kOutput;
  endpoint.is_startpoint = false;
  endpoint.is_endpoint = true;
  endpoint.net_name = "n_out";
  input.nodes[2].is_endpoint = false;
  input.nodes.push_back(endpoint);
  auto net = input.arcs[0];
  net.id = ArcId{2}; net.source = NodeId{2}; net.sink = NodeId{3};
  input.arcs.push_back(net);
  input.ordered_end_ids = {NodeId{3}};
  input.clock_owner_by_node.push_back(std::nullopt);
  input.out_row_offsets = {0, 1, 2, 3, 3};
  input.in_row_offsets = {0, 0, 1, 2, 3};
  input.out_arc_ids = input.in_arc_ids = {ArcId{0}, ArcId{1}, ArcId{2}};
  for (auto& arc : input.arcs) arc.output_load.reset();
  auto children = input.arcs[1].transfer.children();
  auto& child = children.front();
  child.slew_derate = 0.8;
  child.slew_lower_threshold_pct = {0.10, 0.20};
  child.slew_upper_threshold_pct = {0.90, 0.80};
  child.output_threshold_pct = {0.45, 0.55};
  child.delay_tables = {TimingTableModel{true, TableVariable::kOutputCapacitance, TableVariable::kNone,
      {{0.0, 1.0}}, {0.03, 2.03}}, TimingTableModel{true, TableVariable::kOutputCapacitance, TableVariable::kNone,
      {{0.0, 1.0}}, {0.04, 2.04}}};
  child.slew_tables = {TimingTableModel{true, TableVariable::kOutputCapacitance, TableVariable::kNone,
      {{0.0, 1.0}}, {0.02, 3.02}}, TimingTableModel{true, TableVariable::kOutputCapacitance, TableVariable::kNone,
      {{0.0, 1.0}}, {0.025, 3.025}}};
  for (unsigned transition = 0; transition < 2; ++transition) {
    const auto& delay = child.delay_tables[transition];
    const auto& slew = child.slew_tables[transition];
    child.signature.delay_tables[transition] = {delay.present, delay.variable1, delay.variable2};
    child.signature.slew_tables[transition] = {slew.present, slew.variable1, slew.variable2};
  }
  input.arcs[1].transfer.setChildren(children);
  input.parasitics.input_port_thresholds = {0.6, {35.0, 30.0}, {85.0, 75.0}};
  const std::vector<CsrRcTreeNode> nodes{{0, 0.0, 0.012}, {0, 2.0, 0.032}};
  const std::vector<std::size_t> terminals{0, 1};
  const auto make_tree = [&](NodeId driver, NodeId sink) {
    auto tree = std::make_shared<CsrParasiticTree>();
    tree->terminals = {driver, sink};
    auto model = std::make_shared<CsrParasiticModel>(buildCsrParasiticModel(nodes, terminals));
    if (family) model->arnoldi.order = 0;
    if (family == 2) model->dmp.valid = false;
    tree->lanes.fill(model);
    return tree;
  };
  input.arcs[0].parasitic = {make_tree(NodeId{0}, NodeId{1}), 1};
  const auto output = make_tree(NodeId{2}, NodeId{3});
  input.arcs[1].parasitic = {output, 0};
  input.arcs[2].parasitic = {output, 1};
  if (cross_library) {
    input.nodes[3].is_port = false;
    input.nodes[3].instance_cell = input.referenced_library_cells.front().cell_name;
    auto& cell = input.referenced_library_cells.front();
    cell.library_name = "different.lib";
    cell.slew_derate = 0.7;
    cell.slew_lower_threshold_pct = {15.0, 25.0};
    cell.slew_upper_threshold_pct = {85.0, 80.0};
    cell.input_threshold_pct = {35.0, 60.0};
  }
  if (mode) {
    input.clocks = {{"clk", "clk", {{"in", NodeId{0}}}, 10.0, {0.0, 5.0}, true}};
    input.clock_owner_by_node.assign(input.nodes.size(), std::string{"clk"});
    for (auto& arc : input.arcs) arc.eligibility.clock = true;
  }
  if (mode == 2) {
    input.clock_owner_by_node[2].reset();
    input.clock_owner_by_node[3].reset();
    input.arcs[0].eligibility = {true, false, false, true};
    input.arcs[1].eligibility = {false, false, false, false};
    input.arcs[2].eligibility = {true, true, true, false};
    input.nodes[1].is_clock_pin = true;
    for (unsigned id : {1U, 2U}) {
      input.nodes[id].is_sequential = true;
      input.nodes[id].is_startpoint = true;
    }
    auto& cell = input.referenced_library_cells.front();
    cell.is_sequential = true;
    cell.clock_pin_name = input.nodes[1].key.pin_name;
    cell.output_pin_name = input.nodes[2].key.pin_name;
    cell.timing_checks.resize(1);
    cell.cell_arcs.front().transfer = input.arcs[1].transfer;
    cell.cell_arcs.front().transfer.is_clock_arc = true;
    input.ordered_start_ids = {NodeId{0}, NodeId{1}, NodeId{2}};
    input.ordered_root_ids = {NodeId{0}, NodeId{2}};
    for (unsigned id : {1U, 2U}) {
      StartSeedModel seed;
      seed.seed_node = NodeId{id};
      seed.path_start_node = NodeId{2};
      seed.availability = StartSeedAvailability::kDeferredSequential;
      seed.path_source = PathSource::kRegister;
      seed.clock_tag = "clk";
      seed.crpr_node = NodeId{1};
      seed.crpr_transition = Transition::kRise;
      if (id == 2) seed.parasitic = {output, 0};
      input.start_seeds.push_back(seed);
    }
  }
  return input;
}


CudaDualCsrPatch rcPatch(const CsrReferenceInput& old, const CsrReferenceInput& next, std::uint64_t generation)
{
  CudaDualCsrPatch patch;
  patch.session_token = timing_edit::TimingEditSessionToken{old.source_identity.session_token};
  patch.expected_generation = timing_edit::TransactionGeneration{generation};
  patch.target_generation = timing_edit::TransactionGeneration{generation + 1};
  patch.before = {old.nodes.size(), 0, old.arcs.size()};
  patch.after = {next.nodes.size(), 0, next.arcs.size()};
  std::map<NodeId, CsrParasiticBinding> before, after;
  const auto collect = [](const CsrReferenceInput& input, auto& nodes) {
    for (const auto& arc : input.arcs)
      if (arc.parasitic.tree)
        for (std::size_t i = 0; i < arc.parasitic.tree->terminals.size(); ++i)
          nodes.try_emplace(arc.parasitic.tree->terminals[i], CsrParasiticBinding{arc.parasitic.tree, i});
  };
  collect(old, before);
  collect(next, after);
  for (const auto& [node, value] : after) {
    const auto found = before.find(node);
    patch.rc_bindings.push_back({node, found == before.end() ? CsrParasiticBinding{} : found->second, value});
  }
  FlatCsrEdgeView previous(old), replacement(next);
  for (const auto& arc : old.arcs)
    patch.payload_changes.push_back({arc.id, previous.arc(arc.id).value(), replacement.arc(arc.id).value()});
  for (std::size_t i = old.arcs.size(); i < next.arcs.size(); ++i) {
    const auto& arc = next.arcs[i];
    patch.arc_appends.push_back({arc.id, arc.key, arc.source, arc.sink, replacement.arc(arc.id).value()});
  }
  for (std::size_t i = old.nodes.size(); i < next.nodes.size(); ++i) {
    const auto& node = next.nodes[i];
    patch.node_appends.push_back({node.id, node.key, node});
    patch.appended_base_out_rows.push_back({node.id, {next.out_arc_ids.begin() + next.out_row_offsets[i],
                                                     next.out_arc_ids.begin() + next.out_row_offsets[i + 1]}});
    patch.appended_base_in_rows.push_back({node.id, {next.in_arc_ids.begin() + next.in_row_offsets[i],
                                                    next.in_arc_ids.begin() + next.in_row_offsets[i + 1]}});
  }
  for (const auto& cell : next.referenced_library_cells)
    patch.rc_load_cells.emplace(cell.cell_name, std::cref(cell));
  if (old.ordered_root_ids != next.ordered_root_ids)
    patch.roots = timing_dual_csr::ValueReplacement<std::vector<NodeId>>{old.ordered_root_ids, next.ordered_root_ids};
  if (old.ordered_end_ids != next.ordered_end_ids)
    patch.ends = timing_dual_csr::ValueReplacement<std::vector<NodeId>>{old.ordered_end_ids, next.ordered_end_ids};
  return patch;
}
void rcGrowth(CudaLevelizationMode mode)
{
  const auto input = rcFixture(0, false);
  auto next = input;
  for (unsigned original : {1U, 2U, 3U, 3U}) {
    auto node = input.nodes[original];
    node.id = NodeId{next.nodes.size()};
    node.key.full_name = "new_" + std::to_string(node.id.value()) + "_" + node.key.full_name;
    if (!node.is_port) node.key.instance_name = "new_u";
    node.is_startpoint = false;
    node.is_endpoint = node.id == NodeId{6};
    next.nodes.push_back(node);
    next.clock_owner_by_node.push_back(std::nullopt);
  }
  const auto append = [&](unsigned original, unsigned source, unsigned sink) {
    auto arc = input.arcs[original];
    arc.id = ArcId{next.arcs.size()};
    arc.source = NodeId{source}; arc.sink = NodeId{sink};
    arc.native_traversal_rank = arc.id.value();
    next.arcs.push_back(arc);
  };
  append(2, 2, 4); append(1, 4, 5); append(2, 5, 6);
  auto tree = std::make_shared<CsrParasiticTree>();
  tree->terminals = {NodeId{2}, NodeId{3}, NodeId{4}};
  tree->lanes.fill(std::make_shared<CsrParasiticModel>(buildCsrParasiticModel(
      std::vector<CsrRcTreeNode>{{0, 0.0, 0.012}, {0, 2.0, 0.032}, {0, 1.0, 0.023}}, std::vector<std::size_t>{0, 1, 2})));
  next.arcs[1].parasitic = {tree, 0};
  next.arcs[2].parasitic = {tree, 1};
  next.arcs[3].parasitic = {tree, 2};
  auto created = std::make_shared<CsrParasiticTree>(*input.arcs[2].parasitic.tree);
  created->terminals = {NodeId{5}, NodeId{6}};
  next.arcs[4].parasitic = {created, 0}; next.arcs[5].parasitic = {created, 1};
  next.out_row_offsets = {0, 1, 2, 4, 4, 5, 6, 6, 6};
  next.in_row_offsets = {0, 0, 1, 2, 3, 4, 5, 6, 6};
  next.out_arc_ids = next.in_arc_ids = {ArcId{0}, ArcId{1}, ArcId{2}, ArcId{3}, ArcId{4}, ArcId{5}};
  next.ordered_root_ids = {NodeId{0}, NodeId{7}};
  next.ordered_end_ids = {NodeId{3}, NodeId{6}};
  auto owner = CudaDualCsrTestReader::computeRc(input, 64);
  const auto saved = owner.timingValues();
  auto patch = rcPatch(input, next, 0);
  patch.aux_rows = {{NodeId{2}, {}, {ArcId{3}}}};
  if constexpr (incregpusta_debug_checks_enabled) {
    std::size_t allocations;
    CsrTimingValues expected;
    {
      auto measured = CudaDualCsrTestReader::computeRc(input, 64);
      ista_test_allocation::armCountOnly();
      auto candidate = measured.stage(patch, mode);
      allocations = ista_test_allocation::disable();
      require(!candidate.computationCode(), "RC allocation sweep fixture failed");
      expected = candidate.timingValues();
    }
    for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
      auto measured = CudaDualCsrTestReader::computeRc(input, 64);
      bool rejected = false;
      ista_test_allocation::armFailure(ordinal);
      try { auto candidate = measured.stage(patch, mode); }
      catch (const std::bad_alloc&) { rejected = true; }
      static_cast<void>(ista_test_allocation::disable());
      require(rejected && !measured.poisoned() && measured.generation().value() == 0,
              "RC host allocation failure lost committed ownership");
      auto retry = measured.stage(patch, mode);
      require(!retry.computationCode() && retry.timingValues() == expected && measured.publish(std::move(retry)),
              "RC allocation failure damaged retry or actual device result");
      require(measured.close(), "RC allocation sweep cleanup failed");
    }
  }
  {
    auto candidate = owner.stage(patch, mode);
    require(!candidate.computationCode(), "RC/new-driver growth failed");
    if constexpr (incregpusta_debug_checks_enabled) {
      FlatCsrEdgeView edges(next);
      FlatTimingPayloadView payload(next);
      const auto result = detail::computeCsrCpuReferenceForStage(edges, payload).computation;
      const auto* expected = std::get_if<CsrTimingResult>(&result.outcome);
      require(expected != nullptr, "RC growth CPU reference failed");
      test::compareRcTiming(candidate.timingValues(), *expected);
    }
  }
  CudaDualCsrPatch noop;
  noop.session_token = patch.session_token;
  noop.expected_generation = patch.expected_generation;
  noop.target_generation = patch.target_generation;
  noop.before = noop.after = patch.before;
  {
    auto replay = owner.stage(noop, mode);
    require(!replay.computationCode() && replay.timingValues() == saved, "RC growth rollback did not restore actual device buffers");
  }
  auto committed = owner.stage(patch, mode);
  require(!committed.computationCode() && owner.publish(std::move(committed)), "RC growth after rollback did not commit");
  const auto current = owner.timingValues();
  auto full = CudaDualCsrTestReader::stageRc(owner, next);
  require(!full.computationCode() && full.timingValues() == current && owner.publish(std::move(full)),
          "full after RC capacity growth differs");
  require(owner.close(), "RC capacity growth close failed");
}
void targetedRc()
{
  for (auto mode : {CudaLevelizationMode::kFull, CudaLevelizationMode::kMixed})
    for (unsigned seed_mode : {0U, 2U}) {
      auto input = rcFixture(0, false, seed_mode);
      auto owner = CudaDualCsrTestReader::computeRc(input, 64);
      for (unsigned batch = 0; batch < 4; ++batch) {
        auto changed = rcFixture((batch + 1) % 3, false, seed_mode);
        auto patch = rcPatch(input, changed, owner.generation().value());
        auto omitted = patch;
        omitted.payload_changes.pop_back();
        bool rejected = false;
        try { auto candidate = owner.stage(omitted, mode); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && !owner.poisoned(), "incomplete RC update was accepted or poisoned the owner");
        const auto saved = owner.timingValues();
        {
          auto abandoned = owner.stage(patch, mode);
          require(!abandoned.computationCode(), "targeted RC preparation failed");
        }
        require(owner.timingValues() == saved, "abandoned targeted RC update lost the committed result");
        auto partial = patch;
        partial.payload_changes[1].expected.transfer.delay += 0.5;
        auto failed = owner.stage(partial, mode);
        require(failed.computationCode() == CsrComputationCode::kInvariantViolation && !owner.poisoned(),
                "RC partial patch failure was not rolled back");
        require(failed.close(), "RC failed patch cleanup failed");
        CudaDualCsrTestReader::setKernelThreads(owner, batch % 2 ? 256 : 64);
        auto candidate = owner.stage(patch, mode);
        require(!candidate.computationCode() && candidate.telemetry().materialize_ms == 0.0,
                "targeted RC required eager result materialization");
        if constexpr (incregpusta_debug_checks_enabled) {
          FlatCsrEdgeView edges(changed);
          FlatTimingPayloadView payload(changed);
          const auto computed = detail::computeCsrCpuReferenceForStage(edges, payload).computation;
          const auto* reference = std::get_if<CsrTimingResult>(&computed.outcome);
          require(reference != nullptr, "targeted RC CPU oracle failed");
          test::compareRcTiming(candidate.timingValues(), *reference);
        }
        require(owner.publish(std::move(candidate)), "targeted RC publication failed");
        input = std::move(changed);
      }
      const auto saved = owner.timingValues();
      {
        auto full = CudaDualCsrTestReader::stageRc(owner, input);
        require(!full.computationCode() && full.timingValues() == saved,
                "full after targeted RC used stale model lanes or rows");
      }
      auto full = CudaDualCsrTestReader::stageRc(owner, input);
      require(!full.computationCode() && owner.publish(std::move(full)), "full RC commit after targeted failed");
      auto changed = rcFixture(2, false, seed_mode);
      auto final = owner.stage(rcPatch(input, changed, owner.generation().value()), mode);
      require(!final.computationCode() && owner.publish(std::move(final)), "targeted RC after full failed");
      require(owner.close(), "targeted RC close failed");
    }
}
}  // namespace

void runCudaRcPropagationContracts()
{
  targetedRc();
  rcGrowth(CudaLevelizationMode::kFull);
  rcGrowth(CudaLevelizationMode::kMixed);
  for (unsigned family = 0; family < 3; ++family)
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto input = rcFixture(family, mode == 0, mode);
      CsrTimingValues previous;
      for (unsigned threads : {64, 256}) {
        auto session = CudaDualCsrTestReader::computeRc(input, threads);
        require(!session.computationCode(), "RC CUDA full propagation failed");
        const auto& actual = session.timingValues();
        require(actual.nodes.arrivals.presence[3] && std::isfinite(actual.nodes.arrivals.values[3]),
            "RC CUDA endpoint not propagated");
        require(&session.timingValues() == &actual, "RC repeated query did not reuse current result");
        if constexpr (incregpusta_debug_checks_enabled) {
          FlatCsrEdgeView edges(input);
          FlatTimingPayloadView payload(input);
          const auto expected = detail::computeCsrCpuReferenceForStage(edges, payload).computation;
          const auto* timing = std::get_if<CsrTimingResult>(&expected.outcome);
          require(timing != nullptr, "RC CPU internal computation failed");
          test::compareRcTiming(actual, *timing);
        }
        if (mode == 1)
          require(actual.physical_clock.arrivals.presence.back(), "RC propagated clock did not reach endpoint");
        if (mode == 2)
          require(actual.paths.crpr_nodes.presence.end() != std::ranges::find(actual.paths.crpr_nodes.presence, 1),
                  "RC CTQ lost register launch identity");
        if (threads == 256)
          require(actual == previous, "RC full result changed with launch shape");
        previous = actual;
        require(session.close(), "RC CUDA close failed");
      }
    }
  auto input = rcFixture(0, false);
  auto owner = CudaDualCsrTestReader::computeRc(input, 64);
  const auto expected = owner.timingValues();
  for (unsigned batch = 0; batch < 4; ++batch) {
    auto next = CudaDualCsrTestReader::stageRc(owner, input);
    require(!next.computationCode() && next.telemetry().materialize_ms == 0.0,
            "RC full commit required eager timing materialization");
    if (batch >= 2)
      require(next.telemetry().cuda.allocations == 0 && next.telemetry().cuda.d2d_bytes == 0,
              "unchanged RC full rebuild reallocated or copied graph");
    require(owner.publish(std::move(next)), "RC full commit failed");
  }
  const auto generation = owner.generation();
  const auto changed = rcFixture(1, false);
  {
    auto abandoned = CudaDualCsrTestReader::stageRc(owner, changed);
    require(!abandoned.computationCode(), "changed RC full prepare failed");
  }
  require(owner.generation() == generation && !owner.poisoned() && owner.timingValues() == expected,
          "abandoned RC model change lost committed result");
  auto invalid = rcFixture(2, false);
  auto bad_tree = std::make_shared<CsrParasiticTree>(*invalid.arcs[2].parasitic.tree);
  auto bad_model = std::make_shared<CsrParasiticModel>(*bad_tree->lanes[0]);
  bad_model->fallback[1].delay = std::numeric_limits<double>::infinity();
  bad_tree->lanes.fill(bad_model);
  invalid.arcs[1].parasitic = {bad_tree, 0};
  invalid.arcs[2].parasitic = {bad_tree, 1};
  auto failed = CudaDualCsrTestReader::stageRc(owner, invalid);
  require(failed.computationCode() == CsrComputationCode::kNumericFailure
              && !owner.publish(std::move(failed)) && !owner.poisoned() && owner.timingValues() == expected,
          "RC numeric failure lost old result or published partial state");
  require(failed.close(), "RC failed candidate cleanup damaged owner");
  auto recovered = CudaDualCsrTestReader::stageRc(owner, changed);
  require(!recovered.computationCode() && owner.publish(std::move(recovered)), "RC full commit after rollback failed");
  if constexpr (incregpusta_debug_checks_enabled) {
    FlatCsrEdgeView edges(changed);
    FlatTimingPayloadView payload(changed);
    const auto reference = detail::computeCsrCpuReferenceForStage(edges, payload).computation;
    const auto* result = std::get_if<CsrTimingResult>(&reference.outcome);
    require(result != nullptr, "RC recovered CPU reference failed");
    test::compareRcTiming(owner.timingValues(), *result);
  }
  require(owner.close(), "RC full lifecycle close failed");
}
}  // namespace ista::csr
