// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalTimingEdit.hpp"
#include "PhysicalSessionTestFixture.hpp"
#include "PhysicalGreedySelection.hpp"
#include "StaCsrSessionContext.hpp"
#include "PhysicalStaBatchJson.hpp"
#include "BenchmarkTimingConsumer.hpp"
#include "json.hpp"
#include "PhysicalStaSessionState.hpp"
#include "PhysicalStaPlacement.hpp"
#include "idm.h"
#include "PreRouteRcTestSupport.hpp"
#include "DataManager.hpp"
#include "NativeFullEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditValidation.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include <array>
#include <cmath>
#include "IncreGpuStaBuildConfig.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <limits>
#include <stdexcept>

namespace ista {
void physicalGreedyCandidateContract(bool with_cuda);
void physicalFirstReportFailure();
void physicalOutputFailureRetry(idb::IdbDesign& design);
void physicalQueryAllocationRetry(IncrementalStaSession& session);
void physicalColdViewRetry(IncrementalStaSession& session);
#if ECC_INCREGPUSTA_DEBUG_CHECKS
void physicalDiagnosticCanary(IncrementalStaSession& session, idb::IdbDesign& design);
#endif
PhysicalStaSessionState& StaTimingEditSessionProbeAccess::physicalState(StaCsrSessionContext& context)
{
  if (!context._physical) throw std::logic_error("physical test session has no committed owner");
  return *context._physical;
}
}
namespace {
using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using physical_test::PhysicalDesign;
using physical_test::RecoveryDirectory;
void recoveryRecordContract(const EditBatch& batch, std::span<const PhysicalBufferLocation> locations)
{
  const std::string baseline_id(64, 'a');
  const StaExecutionRequest request{StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError};
  const StaExecutionRecord execution{request.backend, request.update_strategy, request.fallback_policy, request.backend,
      request.update_strategy, StaExecutionKind::kFullUpdate, StaPropagationScope::kFullGraph, false, false, std::nullopt};
  const auto bytes = encodePhysicalStaRecoveryBatch(baseline_id, batch, locations, execution, "native_full", 1000);
  const auto parsed = decodePhysicalStaRecoveryBatch(bytes, baseline_id, batch.expected_generation.value(), request, 1000);
  require(parsed.batch == batch && parsed.locations == std::vector<PhysicalBufferLocation>(locations.begin(), locations.end())
      && parsed.execution == execution && parsed.levelization == "native_full", "physical recovery round trip differs");
  const auto rejected = [&](const std::string& altered) {
    try { static_cast<void>(decodePhysicalStaRecoveryBatch(altered, baseline_id, batch.expected_generation.value(), request, 1000)); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("malformed physical recovery record was accepted");
  };
  auto value = nlohmann::json::parse(bytes);
  const nlohmann::json request_value{{"schema_version", "physical-sta-batch/1.0.0"},
      {"expected_generation", batch.expected_generation.value()}, {"edits", value.at("edits")}};
  const auto [requested_batch, requested_locations] = decodePhysicalStaBatchRequest(request_value.dump());
  require(requested_batch == batch && requested_locations == parsed.locations, "physical production request changed semantic edits");
  const auto request_rejected = [&](const std::string& altered) {
    try { static_cast<void>(decodePhysicalStaBatchRequest(altered)); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("malformed physical production request was accepted");
  };
  request_rejected("{\"expected_generation\":1," + request_value.dump().substr(1));
  for (int scenario = 0; scenario != 5; ++scenario) {
    auto changed = request_value;
    switch (scenario) {
      case 0: changed["expected_generation"] = true; break;
      case 1: changed["edits"] = nlohmann::json::array(); break;
      case 2: changed["edits"][0]["physical"]["orientation"] = "NONE"; break;
      case 3: changed["edits"][0]["physical"]["x_dbu"] = 0.5; break;
      case 4: changed["schema_version"] = "physical-sta-batch/0.0.0"; break;
    }
    request_rejected(changed.dump());
  }
  for (int scenario = 0; scenario != 8; ++scenario) {
    auto changed = value;
    switch (scenario) {
      case 0: changed["baseline_id"] = std::string(64, 'b'); break;
      case 1: changed["generation"] = batch.expected_generation.value(); break;
      case 2: changed["edits"][0]["physical"]["x_dbu"] = std::numeric_limits<std::uint64_t>::max(); break;
      case 3: changed["edits"][0]["physical"]["x_dbu"] = 0.5; break;
      case 4: changed["edits"][0]["physical"]["orientation"] = "NONE"; break;
      case 5: changed["levelization"] = "mixed"; break;
      case 6: changed["execution"]["executed_backend"] = "cuda"; break;
      case 7: changed["edits"][0]["physical"]["x_dbu"] = locations.front().x_dbu + 1; break;
    }
    rejected(changed.dump());
  }
  rejected("{\"schema_version\":\"duplicate\"," + bytes.substr(1));
}
void cpuBaseline()
{
  NativeTransactionRuntime runtime;
  installNativeTransactionBaseline();
  PhysicalDesign physical;
  RecoveryDirectory recovery;
  auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
  GuardedTestSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError}, analysis);
  const auto baseline = session.session().runPhysicalBaseline(recovery.config());
  if (!baseline.ok()) throw std::runtime_error("CPU physical baseline: " + baseline.status.message);
  const auto result = session.session().readPhysicalResult();
  require(result.generation == 0 && !baseline.record->incremental_batch_executed,
          "CPU baseline counted as an incremental edit");
  const auto placement = session.session().readPhysicalPlacement(0, {"BUF_X1"});
  require(placement.generation == 0 && placement.dbu_per_um == 1000
      && placement.cells.at("BUF_X1") == std::array<std::int32_t, 2>{1000, 2000}
      && placement.geometry.obstacles.empty(), "physical placement query changed baseline geometry");
  bool stale = false;
  try { static_cast<void>(session.session().readPhysicalPlacement(1, {"BUF_X1"})); }
  catch (const StaSessionException&) { stale = true; }
  require(stale, "physical placement query accepted a stale generation");
  physicalOutputFailureRetry(physical.design);
  physicalQueryAllocationRetry(session.session());
#if ECC_INCREGPUSTA_DEBUG_CHECKS
  physicalDiagnosticCanary(session.session(), physical.design);
#endif
  require(session.closeAndRelease().ok(), "CPU physical baseline close failed");
}
void recoveryModelIdentity()
{
  for (int scenario : {0, 1, 2}) {
    NativeTransactionRuntime runtime;
    installNativeTransactionBaseline();
    PhysicalDesign physical;
    RecoveryDirectory initial, checkpoint;
    auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
    const StaExecutionRequest request{StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError};
    {
      GuardedTestSession session(request, analysis);
      require(session.session().runPhysicalBaseline(initial.config()).ok(), "model fixture baseline failed");
      BufferInsertionMutation edit{ExistingNetRef{"n0"},
          {ExistingNodeRef{{csr::NodeKind::kPort, "out", {}, {}}}},
          nativeLibraryCellRef(STADM.getDatabase(), "BUF_X1"), PlacedInputs{5.0, 7.0},
          ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree}, editTestCreationToken()};
      const std::array locations{PhysicalBufferLocation{edit.creation_token, 5000, 7000, idb::IdbOrient::kN_R0}};
      require(session.session().applyPhysicalBatch({TransactionGeneration{0}, {edit}}, locations).generation == 1,
              "model fixture did not install a used cell");
      require(session.closeAndRelease().ok(), "model fixture close failed");
    }
    {
      GuardedTestSession session(request, analysis);
      require(session.session().runPhysicalBaseline(checkpoint.config()).ok(), "used-cell checkpoint failed");
      require(session.closeAndRelease().ok(), "used-cell checkpoint close failed");
    }
    auto& library = STADM.getDatabase().get_timing_library();
    if (scenario == 1) {
      auto& child = library.get_cell_map().at("BUF_X1").get_cell_arc_list().front().get_timing_arc_list().front();
      TimingTable delay; delay.set_value_list({0.9});
      child.get_delay_table_map()[TransType::kRise] = std::move(delay);
    } else if (scenario == 2) {
      library.set_slew_derate_from_library(0.6);
    }
    GuardedTestSession session(request, analysis);
    bool rejected = false;
    try { static_cast<void>(session.session().recoverPhysicalBaseline(checkpoint.config())); }
    catch (const StaSessionException& error) {
      require(std::string(error.what()).find("physical recovery checkpoint/configuration differs") != std::string::npos,
              "model recovery failed before checking checkpoint identity");
      rejected = true;
    }
    require(rejected == (scenario != 0), "recovery did not bind the actual loaded models and port thresholds");
    require(physical.design.get_instance_list()->get_instance_list().size() == 1,
            "model identity check changed the checkpoint design");
    require(session.closeAndRelease().ok(), "model identity recovery close failed");
  }
}
void baselineRejections(bool cuda)
{
  NativeTransactionRuntime runtime;
  for (int scenario = cuda ? 3 : 0; scenario != (cuda ? 4 : 3); ++scenario) {
    installNativeTransactionBaseline();
    PhysicalDesign physical;
    auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
    const bool gated = scenario == 3;
    if (gated) {
      TimingClock clock;
      clock.set_clock_name("test_clock");
      clock.set_source_list({"in"});
      clock.set_period(10.0);
      STADM.getDatabase().get_timing_constraint().get_case_analysis_map().emplace("in", false);
      STADM.getDatabase().get_timing_constraint().get_clock_map().emplace("test_clock", std::move(clock));
    }
    GuardedTestSession session({cuda ? StaBackend::kCuda : StaBackend::kCpu, cuda ? StaUpdateStrategy::kIncremental : StaUpdateStrategy::kFull,
                               StaFallbackPolicy::kError}, analysis);
    if (scenario == 2) {
      auto& pins = STADM.getDatabase().get_net_map().at("n0").get_pin_name_list();
      pins.back() = pins.front();
    }
    if (scenario == 1) STADM.getDatabase().get_net_map().at("n0").get_pin_name_list().push_back("absent");
    const auto result = session.session().runPhysicalBaseline({scenario == 0 ? "missing" : "M1", "M2", {}, {}});
    require(!result.ok() && session.session().state() == StaSessionState::kPoisoned,
            "invalid physical baseline remained usable");
    if (gated)
      require(result.status.code == StaSessionStatusCode::kUnsupported
                  && result.status.capability_reason == StaCapabilityReason::kUnsupportedAnalysisFeatures,
              "scope gate did not retain its typed unsupported failure");
    require(STADM.getDatabase().get_parasitic_library().get_net_map().empty(), "failed baseline retained RC");
    for (const auto& [name, clock] : STADM.getDatabase().get_timing_constraint().get_clock_map()) {
      static_cast<void>(name);
      require(!clock.get_is_propagated(), "failed baseline retained propagated clock state");
    }
    const auto query = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(),
        [](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority&) -> StaStatus {
          throw std::runtime_error("poisoned baseline entered a query");
        });
    require(query.code == StaSessionStatusCode::kInvalidState && physical.design.validateConnectivity().ok
                && physical.design.get_instance_list()->get_instance_list().empty(),
            "failed physical baseline changed IDB or allowed a query");
    require(session.closeAndRelease().ok(), "failed physical baseline close failed");
  }
}
void run()
{
  RecoveryDirectory recovery;
  ista::test::BenchmarkTimingConsumer committed_consumer;
  EditBatch next_batch;
  std::vector<PhysicalBufferLocation> next_locations;
  {
  NativeTransactionRuntime runtime;
  installNativeTransactionBaseline();
  PhysicalDesign physical;
  Net empty;
  empty.set_net_name("orphan");
  STADM.getDatabase().get_net_map().emplace("orphan", std::move(empty));
  physical.design.createOrFindNet("orphan", idb::IdbConnectType::kSignal);
  auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
  GuardedTestSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError}, analysis);
  const auto baseline_result = session.session().runPhysicalBaseline(recovery.config());
  if (!baseline_result.ok()) throw std::runtime_error("production physical baseline failed: " + baseline_result.status.message);
  require(baseline_result.record->execution_kind == StaExecutionKind::kFullUpdate
              && !baseline_result.record->incremental_batch_executed, "physical baseline mislabeled an incremental batch");
  require(!STADM.getDatabase().get_parasitic_library().get_net_map().contains("orphan"),
          "empty DEF/native net incorrectly received a timing RC tree");
  require(!session.session().runPhysicalBaseline(recovery.config()).ok(), "duplicate physical baseline was accepted");
  DesignEditPlan plan;
  const auto guarded = [&](const auto& action) {
    const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(), action);
    if (!status.ok()) throw std::runtime_error(status.message);
  };
  guarded([&](std::uint64_t token, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
    auto& database = STADM.getDatabase();
    auto& owner = StaTimingEditSessionProbeAccess::physicalState(context);
    require(owner.generation(context).value() == 0 && owner.full != nullptr,
            "CPU/full baseline did not bind native state");
    owner.prepareReport(database, context, authority);
    auto exported = csr::exportCsrReferenceInputCandidate(database, analysis, token);
    require(std::holds_alternative<csr::CsrReferenceInput>(exported), "physical native baseline export failed");
    const auto identity = NativeTimingEditPreparer::identityFromInput(database, std::get<csr::CsrReferenceInput>(exported), TransactionGeneration{0});
    const BaselineSemanticIdentity baseline{database.get_design_name(), identity.current_fingerprints, identity.canonical_pre_state};
    plan = bufferInsertionPlan(database, baseline, TransactionGeneration{0}, editTestCreationToken());
    return StaStatus::success(session.session().state());
  });
  for (std::uint64_t generation = 0; generation != 2; ++generation) {
    plan.expected_generation = TransactionGeneration{generation};
    auto& edit = plan.edits.front();
    edit.creation_token = editTestCreationToken(generation + 1);
    edit.rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree};
    edit.placement_inputs = PlacedInputs{5.0 + generation * 2.0, 7.0};
    const std::array locations{ista::PhysicalBufferLocation{edit.creation_token,
        5000 + static_cast<int>(generation) * 2000, 7000, idb::IdbOrient::kN_R0}};
    const EditBatch batch{plan.expected_generation, plan.edits};
    recoveryRecordContract(batch, locations);
    const auto names = defaultTokenMaterialization(edit.creation_token, edit.library_cell);
    std::string source;
    guarded([&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
      auto& database = STADM.getDatabase();
      auto& rc = database.get_parasitic_library();
      auto& state = *StaTimingEditSessionProbeAccess::physicalState(context).full;
      source = state.resolver().resolveNet(edit.original_net);
      auto* old_net = physical.design.get_net_list()->find_net(source);
      const auto old_rc = izh::test::rcValues(rc.get_net_map().at(source));
      const auto* old_rc_owner = &rc.get_net_map().at(source);
      for (int attempt = 0; attempt != 3; ++attempt) {
        {
          auto idb_batch = ista::preparePhysicalTimingEdit(physical.design, batch, state.resolver(), locations, physical.rc.dbu_per_um);
          auto trees = idb_batch.prepareRc(physical.rc);
          if (attempt == 0) trees.at(source).get_resistor_list().front().set_resistance(-1.0);
          require(idb_batch.apply(), "physical apply rejected");
          bool rejected = false;
          try {
            auto native = NativeFullEditPreparer::prepare(database, authority, batch, state, std::move(trees));
            ista::copyCreatedPhysicalPinCoordinates(idb_batch, database);
            if (attempt != 1) NativeCandidateExecutor::executeFull(native.transaction());
            require(attempt != 0, "invalid physical RC was accepted");
          } catch (const std::invalid_argument&) {
            if (attempt != 0) throw;
            rejected = true;
          }
          require(attempt != 0 || rejected, "malformed tree did not reject after physical apply");
        }
        require(authority.readyForQuery() && physical.design.validateConnectivity().ok,
                "physical/native unwind left pending or invalid connectivity");
        require(state.generation().value() == generation && &rc.get_net_map().at(source) == old_rc_owner
            && izh::test::rcValues(rc.get_net_map().at(source)) == old_rc
            && !database.get_net_map().contains(names.output_net_name)
            && !physical.design.get_net_list()->find_net(names.output_net_name)
            && !physical.design.get_instance_list()->find_instance(names.instance_name)
            && physical.design.get_io_pin_list()->find_pin("out")->get_net() == old_net,
            "joint abandon/failure did not restore physical/native/RC state");
      }
      return StaStatus::success(session.session().state());
    });
    ista::test::BenchmarkTimingConsumer before_failure, after_failure;
    guarded([&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
      StaTimingEditSessionProbeAccess::physicalState(context).prepareReport(STADM.getDatabase(), context, authority);
      before_failure();
      return StaStatus::success(session.session().state());
    });
    // Real output failure after native calculation must undo every layer.
    const auto collision = recovery.path / "journal" / ("committed-" + std::to_string(generation + 1) + ".json");
    std::ofstream(collision) << "existing record";
    bool refused = false;
    try { static_cast<void>(session.session().applyPhysicalBatch(batch, locations)); }
    catch (const std::exception&) { refused = true; }
    require(refused && !physical.design.get_instance_list()->find_instance(names.instance_name)
        && !STADM.getDatabase().get_net_map().contains(names.output_net_name)
        && session.session().state() == StaSessionState::kBaselineExecuted, "marker failure did not perform a retryable rollback");
    guarded([&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
      auto& owner = StaTimingEditSessionProbeAccess::physicalState(context);
      owner.prepareReport(STADM.getDatabase(), context, authority);
      after_failure();
      require(owner.generation(context).value() == generation && before_failure.values == after_failure.values
          && before_failure.reports == after_failure.reports, "marker rollback changed committed timing, QOR or paths");
      return StaStatus::success(session.session().state());
    });
    std::filesystem::remove(collision);
    const auto result = session.session().applyPhysicalBatch(batch, locations);
    require(result.generation == generation + 1 && result.execution.execution_kind == StaExecutionKind::kFullUpdate,
            "production physical commit returned an incorrect generation/mode");
    require(std::filesystem::is_regular_file(collision) && !std::filesystem::exists(recovery.path / "journal" / "pending.json"),
            "production physical commit lost its recovery record");
    bool stale = false;
    try { static_cast<void>(session.session().applyPhysicalBatch(batch, locations)); }
    catch (const std::exception&) { stale = true; }
    require(stale, "production physical batch accepted a stale generation");
    guarded([&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
      auto& database = STADM.getDatabase();
      auto& owner = StaTimingEditSessionProbeAccess::physicalState(context);
      require(authority.readyForQuery() && owner.generation(context).value() == generation + 1
          && database.get_pin_map().at("out").get_net_name() == names.output_net_name
          && physical.design.validateConnectivity().ok, "physical/native common generation or sink diverged");
      owner.prepareReport(database, context, authority);
      owner.prepareReport(database, context, authority);
      auto* inserted = physical.design.get_instance_list()->find_instance(names.instance_name);
      for (const auto& port : {edit.library_cell.input_port, edit.library_cell.output_port}) {
        const auto& pin = database.get_pin_map().at(names.instance_name + ":" + port);
        auto* coordinate = inserted->get_pin(port)->get_average_coordinate();
        require(pin.get_x() == coordinate->get_x() && pin.get_y() == coordinate->get_y(),
                "native pin geometry differs from the oriented physical pin");
      }
      for (const auto& name : {source, names.output_net_name})
        require(izh::test::rcValues(database.get_parasitic_library().get_net_map().at(name))
                    == izh::test::rcValues(izh::buildPreRouteRcNet(*physical.design.get_net_list()->find_net(name), physical.rc)),
                "committed native RC differs from actual physical geometry");
      return StaStatus::success(session.session().state());
    });
    edit.original_net = CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet};
  }
  guarded([&](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority&) {
    committed_consumer();
    return StaStatus::success(session.session().state());
  });
  const auto current = session.session().readPhysicalResult();
  const auto again = session.session().readPhysicalResult();
  require(current.generation == 2 && current.timing == again.timing
      && current.qor_json == again.qor_json && current.paths_json == again.paths_json
      && current.timing.size() == committed_consumer.values.size()
      && nlohmann::json::parse(current.qor_json) == committed_consumer.reports.at("qor")
      && nlohmann::json::parse(current.paths_json) == committed_consumer.reports.at("paths"),
      "public physical result did not return current timing, QOR and paths");
  next_batch = {TransactionGeneration{2}, plan.edits};
  auto& next = next_batch.edits.front();
  next.creation_token = editTestCreationToken(3);
  next.placement_inputs = PlacedInputs{9.0, 7.0};
  next_locations.push_back({next.creation_token, 9000, 7000, idb::IdbOrient::kN_R0});
  require(session.closeAndRelease().ok(), "physical native session close failed");
  }
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("committed record could not be read");
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  const auto first_path = recovery.path / "journal" / "committed-1.json";
  const auto second_path = recovery.path / "journal" / "committed-2.json";
  const auto first_record = read(first_path), second_record = read(second_path);
  for (int scenario : {0, 1, 2, 4, 5, 3}) {
    NativeTransactionRuntime runtime;
    installNativeTransactionBaseline();
    PhysicalDesign physical;
    auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
    GuardedTestSession session({StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError}, analysis);
    if (scenario != 3) {
      if (scenario == 0) {
        require(!session.session().runPhysicalBaseline(recovery.config()).ok(), "fresh baseline accepted committed history");
      } else if (scenario == 4 || scenario == 5) {
        auto& database = STADM.getDatabase();
        if (scenario == 4) {
          auto& output = database.get_timing_constraint().get_port_constraint_map()["out"];
          output.set_port_name("out");
          output.set_has_output_delay_max(true);
          output.set_output_delay_max(0.25);
        } else {
          auto& pins = database.get_net_map().at("n0").get_pin_name_list();
          std::reverse(pins.begin(), pins.end());
        }
        bool failed = false;
        try { static_cast<void>(session.session().recoverPhysicalBaseline(recovery.config())); }
        catch (const StaSessionException&) { failed = true; }
        require(failed && physical.design.get_instance_list()->get_instance_list().empty(),
                "changed native checkpoint input accepted recovery or replayed before rejection");
      } else if (scenario == 2) {
        for (auto* pin : physical.design.get_io_pin_list()->get_pin_list()) {
          auto* coordinate = pin->get_average_coordinate();
          pin->set_average_coordinate(coordinate->get_x() + 100, coordinate->get_y() + 100);
        }
        bool failed = false;
        try { static_cast<void>(session.session().recoverPhysicalBaseline(recovery.config())); }
        catch (const StaSessionException&) { failed = true; }
        require(failed && physical.design.get_instance_list()->get_instance_list().empty(),
                "changed physical geometry accepted recovery or replayed before rejection");
      } else {
        std::ofstream(second_path) << "{}";
        bool failed = false;
        try { static_cast<void>(session.session().recoverPhysicalBaseline(recovery.config())); }
        catch (const StaSessionException&) { failed = true; }
        require(failed && physical.design.get_instance_list()->get_instance_list().size() == 1,
                "damaged second record did not stop after the first committed replay");
      }
      require(session.session().state() == StaSessionState::kPoisoned, "failed recovery exposed a usable prefix");
      const auto query = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(),
          [](std::uint64_t, StaCsrSessionContext&, const NativeExecutionAuthority&) -> StaStatus {
            throw std::runtime_error("query observed an incomplete recovery");
          });
      require(query.code == StaSessionStatusCode::kInvalidState, "incomplete physical recovery accepted a query");
      require(session.closeAndRelease().ok(), "failed physical recovery close failed");
      if (scenario == 1) std::ofstream(second_path) << second_record;
      continue;  // Destroy this physical prefix and reload the original checkpoint.
    }
    const auto recovered = session.session().recoverPhysicalBaseline(recovery.config());
    require(recovered.generation == 2 && recovered.execution.execution_kind == StaExecutionKind::kFullUpdate,
            "physical recovery did not reach the committed generation");
    const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(),
        [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
      auto& owner = StaTimingEditSessionProbeAccess::physicalState(context);
      owner.prepareReport(STADM.getDatabase(), context, authority);
      ista::test::BenchmarkTimingConsumer restored; restored();
      require(restored.values == committed_consumer.values && restored.reports == committed_consumer.reports
          && physical.design.validateConnectivity().ok && physical.design.get_instance_list()->get_instance_list().size() == 2,
          "physical recovery changed committed timing/QOR/paths or replayed an edit twice");
      for (const auto& [name, net] : STADM.getDatabase().get_parasitic_library().get_net_map())
        require(izh::test::rcValues(net) == izh::test::rcValues(
                    izh::buildPreRouteRcNet(*physical.design.get_net_list()->find_net(name), owner.technology)),
                "recovered RC differs from physical connectivity/coordinates");
      return StaStatus::success(session.session().state());
    });
    if (!status.ok()) throw std::runtime_error(status.message);
    require(read(first_path) == first_record && read(second_path) == second_record,
            "physical recovery rewrote committed history");
    const auto continued = session.session().applyPhysicalBatch(next_batch, next_locations);
    require(continued.generation == 3 && physical.design.get_instance_list()->get_instance_list().size() == 3,
            "recovered physical session could not continue a new commit");
    require(session.closeAndRelease().ok(), "recovered physical session close failed");
  }
}


void physicalBatches(bool with_cuda)
{
  std::vector<PhysicalStaCurrentResult> oracle;
  std::vector<StaExecutionRequest> requests{
      {StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError}};
  if (with_cuda) {
    requests.push_back({StaBackend::kCuda, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError});
  }
  for (const auto request : requests) {
    RecoveryDirectory recovery;
    EditBatch next;
    std::vector<PhysicalBufferLocation> next_locations;
    const auto check = [&](const PhysicalStaCurrentResult& result) {
      if (request.backend == StaBackend::kCpu && request.update_strategy == StaUpdateStrategy::kFull
          && oracle.size() < result.generation) {
        oracle.push_back(result);
        return;
      }
      const auto& expected = oracle.at(result.generation - 1);
      require(result.generation == expected.generation && result.timing.size() == expected.timing.size()
                  && result.qor_json == expected.qor_json && result.paths_json == expected.paths_json,
              "physical CUDA public results differ from independent native full");
      for (std::size_t index = 0; index < result.timing.size(); ++index) {
        require(result.timing[index].first == expected.timing[index].first, "physical timing point identity differs");
        for (std::size_t lane = 0; lane < 3; ++lane) {
          const auto actual = result.timing[index].second[lane], reference = expected.timing[index].second[lane];
          require(actual == reference || (std::isfinite(actual) && std::isfinite(reference)
                                          && std::abs(actual - reference) < 1e-10),
                  "physical CUDA timing differs from native full");
        }
      }
    };
    const auto mode_check = [&](const PhysicalStaBatchResult& result, std::uint64_t generation) {
      require(result.generation == generation && result.execution.executed_backend == request.backend
                  && result.execution.executed_update_strategy == request.update_strategy
                  && result.execution.incremental_batch_executed == (request.update_strategy == StaUpdateStrategy::kIncremental)
                  && !result.execution.fallback_to_full, "physical batch did not execute its requested mode");
    };
    {
      NativeTransactionRuntime runtime;
      installNativeTransactionBaseline();
      PhysicalDesign physical;
      auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
      GuardedTestSession session(request, analysis);
      const auto baseline = session.session().runPhysicalBaseline(recovery.config());
      if (!baseline.ok()) throw std::runtime_error("physical multi-mode baseline: " + baseline.status.message);
      DesignEditPlan plan;
      const auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(),
          [&](std::uint64_t token, StaCsrSessionContext&, const NativeExecutionAuthority&) {
        auto& database = STADM.getDatabase();
        auto exported = csr::exportCsrReferenceInputCandidate(database, analysis, token);
        require(std::holds_alternative<csr::CsrReferenceInput>(exported), "physical trace baseline export failed");
        const auto identity = NativeTimingEditPreparer::identityFromInput(database,
            std::get<csr::CsrReferenceInput>(exported), TransactionGeneration{0});
        plan = bufferInsertionPlan(database,
            {database.get_design_name(), identity.current_fingerprints, identity.canonical_pre_state},
            TransactionGeneration{0}, editTestCreationToken());
        return StaStatus::success(session.session().state());
      });
      if (!status.ok()) throw std::runtime_error(status.message);
      for (std::uint64_t before = 0; before != 2; ++before) {
        auto& edit = plan.edits.front();
        edit.creation_token = editTestCreationToken(before + 1);
        edit.rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree};
        edit.placement_inputs = PlacedInputs{5.0 + before * 2.0, 7.0};
        const std::array locations{PhysicalBufferLocation{edit.creation_token,
            5000 + static_cast<int>(before) * 2000, 7000, idb::IdbOrient::kN_R0}};
        const EditBatch batch{TransactionGeneration{before}, plan.edits};
        const auto previous = session.session().readPhysicalResult();
        const auto collision = recovery.path / "journal" / ("committed-" + std::to_string(before + 1) + ".json");
        std::ofstream(collision) << "another publisher";
        bool rejected = false;
        try { static_cast<void>(session.session().applyPhysicalBatch(batch, locations)); }
        catch (const std::exception&) { rejected = true; }
        const auto unchanged = session.session().readPhysicalResult();
        require(rejected && unchanged.generation == before && unchanged.timing == previous.timing
                    && unchanged.qor_json == previous.qor_json && unchanged.paths_json == previous.paths_json
                    && physical.design.validateConnectivity().ok
                    && physical.design.get_instance_list()->get_instance_list().size() == before,
                "physical CUDA marker failure did not restore all participants");
        std::filesystem::remove(collision);
        const auto native_before = csr::exportCsrGraphSnapshot(STADM.getDatabase());
        mode_check(session.session().applyPhysicalBatch(batch, locations), before + 1);
        if (request.backend == StaBackend::kCuda)
          require(csr::exportCsrGraphSnapshot(STADM.getDatabase()) == native_before,
                  "CSR mode edited native timing before a query");
        const auto selection = session.session().selectPhysicalBuffers(before + 1, {"BUF_X1"}, 100, 100);
        require(selection == session.session().selectPhysicalBuffers(before + 1, {"BUF_X1"}, 100, 100),
                "repeated candidate selection changed the committed result");
        if (request.backend == StaBackend::kCuda) {
          require(csr::exportCsrGraphSnapshot(STADM.getDatabase()) == native_before,
                  "candidate selection materialized the native graph");
          physicalColdViewRetry(session.session());
        }
        const auto result = session.session().readPhysicalResult();
        const auto candidate_status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(session.session(),
            [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority&) {
          const auto& owner = StaTimingEditSessionProbeAccess::physicalState(context);
          const auto& resolver = owner.full ? owner.full->resolver()
#if ISTA_ENABLE_CUDA
              : owner.cuda_graph->resolver
#else
              : throw std::logic_error("candidate test owner missing")
#endif
              ;
          const std::array<std::string, 1> cells{"BUF_X1"};
          require(selection == ista::selectPhysicalBuffers(STADM.getDatabase(), context.libraryModels(STADM.getDatabase()),
                      resolver, before + 1, cells, 100, 100, physical.rc.dbu_per_um),
                  "CSR candidate choice differs from the current native view");
          return StaStatus::success(session.session().state());
        });
        require(candidate_status.ok(), "native candidate comparison guard failed");
        check(result);
        const auto cached = session.session().readPhysicalResult();
        require(cached.timing == result.timing && cached.qor_json == result.qor_json && cached.paths_json == result.paths_json,
                "same-generation physical query changed the result");
        edit.original_net = CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet};
      }
      next = {TransactionGeneration{2}, plan.edits};
      next.edits.front().creation_token = editTestCreationToken(3);
      next.edits.front().placement_inputs = PlacedInputs{9.0, 7.0};
      next_locations.push_back({next.edits.front().creation_token, 9000, 7000, idb::IdbOrient::kN_R0});
      physicalQueryAllocationRetry(session.session());
#if ECC_INCREGPUSTA_DEBUG_CHECKS
      physicalDiagnosticCanary(session.session(), physical.design);
#endif
      require(session.closeAndRelease().ok(), "physical multi-mode close failed");
    }
    {
      NativeTransactionRuntime runtime;
      installNativeTransactionBaseline();
      PhysicalDesign physical;
      auto analysis = editTestAnalysisContext(); analysis.rc_corner = "pre_route";
      GuardedTestSession session(request, analysis);
      mode_check(session.session().recoverPhysicalBaseline(recovery.config()), 2);
      if (request.backend == StaBackend::kCuda)
        require(STADM.getDatabase().get_instance_map().empty() && STADM.getDatabase().get_pin_map().size() == 2,
                "CSR recovery materialized unrequested native timing");
      if (request.backend == StaBackend::kCuda)
        physicalColdViewRetry(session.session());
      check(session.session().readPhysicalResult());
      mode_check(session.session().applyPhysicalBatch(next, next_locations), 3);
      check(session.session().readPhysicalResult());
      require(physical.design.validateConnectivity().ok
                  && physical.design.get_instance_list()->get_instance_list().size() == 3,
              "recovery did not continue the physical design");
      require(session.closeAndRelease().ok(), "physical multi-mode recovery close failed");
    }
  }
}

}
int main(int argc, char** argv)
{
  try {
    require(argc == 1 || (argc == 2 && std::string(argv[1]) == "--cuda"), "usage: physical-test [--cuda]");
    if (argc == 2) {
      require(incregpusta_cuda_enabled, "CUDA physical test needs a CUDA build");
      baselineRejections(true);
      physicalBatches(true);
      physicalGreedyCandidateContract(true);
    } else { recoveryModelIdentity(); baselineRejections(false); cpuBaseline(); run(); run(); physicalBatches(false); physicalGreedyCandidateContract(false); physicalFirstReportFailure(); }
    std::cout << "physical/native RC multi-batch apply, rollback and commit passed\n";
  }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
