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
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "Database.hpp"
#include "Instance.hpp"
#include "NativeBufferCellValidation.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NativeDatabaseValidation.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "Pin.hpp"
#include "TimingCell.hpp"
#include "TimingCellPort.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Callable>
std::string requireInvalid(Callable&& callable, const std::string& message)
{
  try {
    callable();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  throw std::runtime_error(message);
}

TimingEditPrepareContext prepareContext(const BaselineSemanticIdentity& baseline,
                                        const CommittedEditIdentity& identity,
                                        TimingEditSessionToken token)
{
  return TimingEditPrepareContext{token, baseline, identity, editTestAnalysisContext()};
}

void requireBidirectionalRawMembership(const Database& database)
{
  for (const auto& [instance_name, instance] : database.get_instance_map()) {
    for (const std::string& pin_name : instance.get_pin_name_list()) {
      const auto pin = database.get_pin_map().find(pin_name);
      require(pin != database.get_pin_map().end(), "instance contains a missing pin");
      require(pin->second.get_instance_name() == instance_name,
              "instance/pin membership is not bidirectional");
    }
  }
  for (const auto& [net_name, net] : database.get_net_map()) {
    for (const std::string& pin_name : net.get_pin_name_list()) {
      const auto pin = database.get_pin_map().find(pin_name);
      require(pin != database.get_pin_map().end(), "net contains a missing pin");
      require(pin->second.get_net_name() == net_name,
              "net/pin membership is not bidirectional");
    }
  }
}

void installInstanceSink(Database& database, bool sequential, bool clock_port)
{
  Pin& sink = database.get_pin_map().at("out");
  sink.set_pin_name("D");
  sink.set_instance_name("u_sink");
  sink.set_direction(PinDirection::kInput);
  sink.set_is_port(false);

  Instance instance;
  instance.set_instance_id(0);
  instance.set_instance_name("u_sink");
  instance.set_cell_name("DFF_X1");
  instance.set_pin_name_list({"out"});
  instance.set_data_pin_name("out");
  instance.set_is_sequential(sequential);
  instance.set_is_clock_gating(false);
  require(database.get_instance_map().emplace("u_sink", std::move(instance)).second,
          "failed to install instance sink");

  TimingCellPort port;
  port.set_port_name("D");
  port.set_is_input(true);
  port.set_is_output(false);
  port.set_is_clock(clock_port);
  TimingCell cell;
  cell.set_cell_name("DFF_X1");
  cell.set_library_name("slow.lib");
  cell.set_is_sequential(sequential);
  cell.set_is_clock_gating(false);
  cell.get_port_map().emplace("D", std::move(port));
  require(database.get_timing_library().get_cell_map().emplace("DFF_X1", std::move(cell)).second,
          "failed to install sink library cell");
}

void testOnlyExactNonInvertingTwoPortBufferIsAccepted()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  Database& database = fixture.database();
  const LibraryCellRef buffer = nativeLibraryCellRef(database, "BUF_X1");
  const TimingCell& accepted = requireLegalNativeBufferCell(database, buffer);
  require(accepted.get_cell_name() == "BUF_X1", "legal BUF_X1 was not accepted");

  requireInvalid(
      [&] { static_cast<void>(requireLegalNativeBufferCell(database, nativeLibraryCellRef(database, "INV_X1"))); },
      "inverting cell was accepted as a buffer");
  requireInvalid(
      [&] { static_cast<void>(requireLegalNativeBufferCell(database, nativeLibraryCellRef(database, "OR2_X1"))); },
      "two-input OR cell was accepted as a buffer");

  LibraryCellRef forged = buffer;
  forged.ordered_arc_signatures.front().arc_index += 1;
  requireInvalid([&] { static_cast<void>(requireLegalNativeBufferCell(database, forged)); },
                 "buffer with nonmatching canonical arc signatures was accepted");

  auto& native_arcs
      = database.get_timing_library().get_cell_map().at("BUF_X1").get_cell_arc_list();
  auto duplicate = native_arcs.front();
  duplicate.get_timing_arc_list().front().set_arc_idx(1);
  native_arcs.push_back(std::move(duplicate));
  const std::string duplicate_error = requireInvalid(
      [&] { static_cast<void>(requireLegalNativeBufferCell(database, buffer)); },
      "buffer with two enabled A-to-Y graph arcs was accepted");
  require(duplicate_error.find("exactly one matching enabled graph arc")
              != std::string::npos,
          "duplicate enabled graph-arc rejection reported the wrong gate: " + duplicate_error);
}

void testNativeDatabaseRejectsIncompleteTimingOrder()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  require(!validateNativeDatabase(fixture.database()).has_value(),
          "baseline native validation fixture is not legal");
  fixture.database().get_timing_order_list().clear();
  const NativeDatabaseValidationResult rejected
      = validateNativeDatabase(fixture.database());
  require(rejected.has_value(), "incomplete timing order was accepted");
  require(rejected->code == NativeDatabaseValidationCode::kInvalidTimingOrder
              && rejected->object == "timing_order_list"
              && rejected->detail
                     == "timing order must exactly cover all timing points once",
          "incomplete timing order did not return the exact typed failure fields");
}

void testTranslatorUpdatesAllRawMembershipsWithoutConstructingDerivedArcs()
{
  constexpr TimingEditSessionToken kToken{23};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  const CommittedEditIdentity identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  const BaselineSemanticIdentity baseline
      = BaselineSemanticIdentity{fixture.database().get_design_name(), identity.current_fingerprints,
                                 identity.canonical_pre_state};
  DesignEditPlan plan = bufferInsertionPlan(fixture.database(), baseline, kGeneration,
                                            editTestCreationToken());
  PreparedBatch batch = NativeTimingEditPreparer::prepare(
      fixture.database(), plan, prepareContext(baseline, identity, kToken), resolver);
  TimingEditResolverState staged_resolver = resolver.stage(batch);
  const auto original_members = fixture.database().get_net_map().at("n0").get_pin_name_list();
  const auto original_sink_net = fixture.database().get_pin_map().at("out").get_net_name();
  const auto original_arcs = fixture.database().get_arc_list().size();
  const auto* library = &fixture.database().get_timing_library();
  const auto* cell = &fixture.database().get_timing_library().get_cell_map().at("BUF_X1");
  {
    auto edits = detail::prepareNativeBufferInsertions(
        fixture.database(), batch.normalizedEdits(), staged_resolver,
        detail::nextNativeInstanceId(fixture.database()));
    require(fixture.database().get_net_map().at("n0").get_pin_name_list() == original_members,
            "raw preparation changed canonical membership");
    edits.apply();
    requireBidirectionalRawMembership(fixture.database());
    require(&fixture.database().get_timing_library() == library
                && &fixture.database().get_timing_library().get_cell_map().at("BUF_X1") == cell,
            "raw apply replaced immutable library storage");
    edits.undo();
    require(fixture.database().get_net_map().at("n0").get_pin_name_list() == original_members
                && fixture.database().get_pin_map().at("out").get_net_name() == original_sink_net
                && fixture.database().get_arc_list().size() == original_arcs
                && fixture.database().get_instance_map().empty(),
            "local raw undo failed to restore actual old state");
    requireBidirectionalRawMembership(fixture.database());
  }

  const NativeTranslationResult result
      = NativeDesignEditTranslator::applyNormalizedBatch(fixture.database(), batch, staged_resolver);
  require(result.applied() && result.reasons.empty(), "supported translator path returned fallback");
  const Database& translated = fixture.database();
  const TokenMaterialization& names = staged_resolver.materialization(editTestCreationToken());

  require(translated.get_instance_map().contains(names.instance_name), "translator did not add instance");
  const auto& instance = translated.get_instance_map().at(names.instance_name);
  require(instance.get_cell_name() == "BUF_X1"
              && instance.get_pin_name_list()
                     == std::vector<std::string>{names.input_pin_name, names.output_pin_name}
              && instance.get_output_pin_name() == names.output_pin_name,
          "inserted instance fields are incomplete");
  require(translated.get_pin_map().at(names.input_pin_name).get_net_name() == "n0"
              && translated.get_pin_map().at(names.output_pin_name).get_net_name()
                     == names.output_net_name,
          "inserted pins do not point at their exact nets");
  require(translated.get_pin_map().at("out").get_net_name() == names.output_net_name,
          "moved sink did not point at the new output net");
  require(translated.get_net_map().at("n0").get_pin_name_list()
              == std::vector<std::string>{"in", names.input_pin_name},
          "original net membership was not rewritten exactly");
  require(translated.get_net_map().at(names.output_net_name).get_pin_name_list()
              == std::vector<std::string>{names.output_pin_name, "out"},
          "new output net membership was not materialized exactly");
  requireBidirectionalRawMembership(translated);
  require(translated.get_arc_list().empty() && translated.get_timing_point_map().empty()
              && translated.get_outgoing_arc_list_map().empty()
              && translated.get_incoming_arc_list_map().empty(),
          "translator constructed or retained derived timing graph state");

}

void testTranslatorChecksSequentialDataAndControlSinkRoles()
{
  constexpr TimingEditSessionToken kToken{31};
  constexpr TransactionGeneration kGeneration{0};
  NativeEditFixture fixture = makeNativeEditFixture();
  TimingEditResolverState resolver = TimingEditResolverState::bindBaseline(fixture.database());
  const CommittedEditIdentity identity = NativeTimingEditPreparer::captureCurrentIdentity(
      fixture.database(), kGeneration, editTestAnalysisContext(), kToken);
  const BaselineSemanticIdentity baseline
      = BaselineSemanticIdentity{fixture.database().get_design_name(), identity.current_fingerprints,
                                 identity.canonical_pre_state};
  const DesignEditPlan plan = bufferInsertionPlan(fixture.database(), baseline, kGeneration,
                                                  editTestCreationToken());
  const PreparedBatch batch = NativeTimingEditPreparer::prepare(
      fixture.database(), plan, prepareContext(baseline, identity, kToken), resolver);
  const TimingEditResolverState staged_resolver = resolver.stage(batch);

  auto sequential = makeNativeEditFixture();
  installInstanceSink(sequential.database(), true, false);
  require(sequential.database().get_instance_map().at("u_sink").get_is_sequential()
              && sequential.database().get_timing_library().get_cell_map().at("DFF_X1").get_is_sequential(),
          "sequential rejection fixture does not carry matching instance/library roles");
  requireInvalid(
      [&] {
        static_cast<void>(NativeDesignEditTranslator::applyNormalizedBatch(
            sequential.database(), batch, staged_resolver));
      },
      "sequential sink without a setup/hold data role was accepted");

  auto data = makeNativeEditFixture();
  installInstanceSink(data.database(), true, false);
  auto& data_cell = data.database().get_timing_library().get_cell_map().at("DFF_X1");
  TimingCheckArc setup;
  setup.set_data_port("D");
  setup.set_clock_port("CK");
  setup.set_check_type(TimingCheckType::kSetup);
  data_cell.get_check_arc_list().push_back(setup);
  // A register may have reset circuitry while its ordinary D remains movable.
  data_cell.set_has_clear_arc(true);
  TimingCellArc clear;
  clear.set_source_port("RN");
  clear.set_sink_port("Q");
  clear.set_is_clear_preset_arc(true);
  data_cell.get_cell_arc_list().push_back(clear);
  const auto moved = NativeDesignEditTranslator::applyNormalizedBatch(data.database(), batch, staged_resolver);
  require(moved.applied() && moved.reasons.empty(), "ordinary sequential D insertion fell back");
  require(data.database().get_pin_map().at("out").get_net_name()
              == staged_resolver.materialization(editTestCreationToken()).output_net_name,
          "ordinary sequential D did not move onto the inserted buffer output net");
  requireBidirectionalRawMembership(data.database());

  for (const bool gating : {false, true}) {
    auto control = makeNativeEditFixture();
    installInstanceSink(control.database(), true, false);
    auto& control_cell = control.database().get_timing_library().get_cell_map().at("DFF_X1");
    control_cell.get_check_arc_list().push_back(setup);
    control_cell.set_is_clock_gating(gating);
    clear.set_source_port(gating ? "RN" : "D");
    control_cell.get_cell_arc_list().push_back(clear);
    requireInvalid([&] {
      static_cast<void>(NativeDesignEditTranslator::applyNormalizedBatch(control.database(), batch, staged_resolver));
    }, "clock-gating or clear/preset sink was accepted as ordinary D");
    require(control.database().get_pin_map().at("out").get_net_name() == "n0"
                && control.database().get_net_map().size() == fixture.database().get_net_map().size()
                && control.database().get_instance_map().size() == 1,
            "rejected control sink changed native memberships");
  }

  auto clock_port = makeNativeEditFixture();
  installInstanceSink(clock_port.database(), false, true);
  require(clock_port.database().get_timing_library().get_cell_map().at("DFF_X1")
              .get_port_map().at("D").get_is_clock(),
          "clock-port rejection fixture does not carry a library clock role");
  const std::string clock_error = requireInvalid(
      [&] {
        static_cast<void>(NativeDesignEditTranslator::applyNormalizedBatch(
            clock_port.database(), batch, staged_resolver));
      },
      "clock library-port sink was accepted by the Stage 3 data-only boundary");
  require(clock_error.find("clock or unresolved library sink") != std::string::npos,
          "clock-port rejection reported the wrong gate: " + clock_error);
}

}  // namespace

int main()
{
  testOnlyExactNonInvertingTwoPortBufferIsAccepted();
  testNativeDatabaseRejectsIncompleteTimingOrder();
  testTranslatorUpdatesAllRawMembershipsWithoutConstructingDerivedArcs();
  testTranslatorChecksSequentialDataAndControlSinkRoles();
  return 0;
}
