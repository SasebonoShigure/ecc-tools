// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "STAInterface.hpp"
#include "IncrementalStaSession.hpp"
#include "IdbTimingPinName.hpp"
#include <set>
#include <memory>
#include <vector>
#include "PhysicalStaSessionState.hpp"
#include "StaCsrSessionContext.hpp"
#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingSemanticKey.hpp"
#include "idm.h"
#include <stdexcept>

namespace ista {
StaStatus STAInterface::runSessionPhysicalBaseline(std::uint64_t token, StaCsrSessionContext* context,
    const StaExecutionRequest& request, const PhysicalStaConfig& config, bool recovery, StaExecutionRecord& execution,
    timing_edit::NativeExecutionScopeViolationSignal violation_signal)
{
  StaStatus failure;
  auto guard = acquireUse(StaOwner::session(token), context, true, StaSessionState::kInitialized, &failure);
  if (!guard) return failure;
  if (!_is_initialized || context->_physical || context->analysisContext().rc_corner != "pre_route")
    return StaStatus::failure(StaSessionStatusCode::kInvalidState, StaSessionState::kInitialized,
                             "physical baseline requires an initialized pre_route session without a baseline");
  try {
    auto* design = dmInst->get_idb_design();
    if (!design || !design->get_layout() || !design->get_units())
      throw std::invalid_argument("physical baseline requires the EngineDB design and units");
    auto technology = izh::preparePreRouteRcTechnology(*design->get_layout(), design->get_units()->get_micron_dbu(),
        config.bottom_layer, config.top_layer, config.horizontal_layer, config.vertical_layer);
    auto prepared = std::make_unique<PhysicalStaSessionState>(*design, std::move(technology), request);
    auto& database = STADM.getDatabase();
    // Prepare every baseline tree before touching native state. Data edits only
    // prepare their affected nets; fixed clock trees retain this baseline RC.
    ParasiticLibrary rc;
    rc.set_capacitive_unit("PF");
    rc.set_resistance_unit("OHM");
    for (const auto& [name, net] : database.get_net_map()) {
      auto* physical_net = design->get_net_list()->find_net(name);
      if (!physical_net) throw std::invalid_argument("native baseline net is absent from EngineDB: " + name);
      std::set<std::string> terminals;
      for (auto* pins : {physical_net->get_io_pins(), physical_net->get_instance_pin_list()})
        for (auto* pin : pins->get_pin_list())
          if (!pin || !terminals.insert(idbTimingPinName(*pin)).second)
            throw std::invalid_argument("physical baseline has ambiguous terminals: " + name);
      if (terminals.size() != net.get_pin_name_list().size())
        throw std::invalid_argument("physical/native baseline terminal count differs: " + name);
      // DEF may retain a named empty net after fanout repair; it has no timing RC.
      if (terminals.empty()) continue;
      for (const auto& pin : net.get_pin_name_list())
        if (terminals.erase(pin) != 1 || !database.get_pin_map().contains(pin)
            || database.get_pin_map().at(pin).get_net_name() != name)
          throw std::invalid_argument("physical/native baseline terminal binding differs: " + name);
      rc.get_net_map().emplace(name, izh::buildPreRouteRcNet(*physical_net, prepared->technology));
    }
    auto& installed = database.get_parasitic_library();
    std::vector<std::pair<TimingClock*, bool>> previous_clocks;
    for (auto& [name, clock] : database.get_timing_constraint().get_clock_map()) {
      static_cast<void>(name);
      previous_clocks.emplace_back(&clock, clock.get_is_propagated());
    }
    installed.get_net_map().swap(rc.get_net_map());
    installed.get_capacitive_unit().swap(rc.get_capacitive_unit());
    installed.get_resistance_unit().swap(rc.get_resistance_unit());
    installed.get_spef_file_path().swap(rc.get_spef_file_path());
    const auto restore_inputs = [&](ParasiticLibrary*) noexcept {
      installed.get_net_map().swap(rc.get_net_map());
      installed.get_capacitive_unit().swap(rc.get_capacitive_unit());
      installed.get_resistance_unit().swap(rc.get_resistance_unit());
      installed.get_spef_file_path().swap(rc.get_spef_file_path());
      for (const auto& [clock, propagated] : previous_clocks)
        clock->set_is_propagated(propagated);
    };
    std::unique_ptr<ParasiticLibrary, decltype(restore_inputs)> input_rollback(&installed, restore_inputs);
    for (const auto& [clock, propagated] : previous_clocks) {
      static_cast<void>(propagated);
      clock->set_is_propagated(true);
    }
    bool unsupported = false;
    const auto bind = [&] {
      auto exported = csr::exportCsrReferenceInputCandidate(database, context->analysisContext(), token, context->libraryModels(database));
      auto* input = std::get_if<csr::CsrReferenceInput>(&exported);
      if (!input) throw std::runtime_error("physical baseline CSR export failed");
      if (!(request.backend == StaBackend::kCpu && request.update_strategy == StaUpdateStrategy::kFull)
          && !input->unsupported_reasons.empty()) {
        unsupported = true;
        std::string message = "physical STA does not support analysis features:";
        for (auto reason : input->unsupported_reasons) message += " " + std::string(csr::token(reason));
        throw std::runtime_error(message);
      }
      prepared->bindRecovery(config, *input, recovery);
      prepared->bindBaseline(database, *input, *context);
    };
    StaOperationResult result;
    if (request.backend == StaBackend::kCpu && request.update_strategy == StaUpdateStrategy::kFull) {
      result = runFullStaPipeline(false);
      if (result.succeeded) bind();
    } else {
      result = executeStaLifecyclePipeline(
          {{"GraphBuilder", [] { GraphBuilder::initInst(); }, [] { STAGB.buildStructure(); }, [] { GraphBuilder::destroyInst(); }},
           {"Physical baseline backend", [] {}, bind, [] {}}},
          StaLifecycleStage{"DelayCalculator runtime", [] { STADC.init(); }, [] {}, [] { STADC.destroy(); }});
    }
    if (!result.succeeded) {
      if (result.cleanup_failed) {
        std::scoped_lock lock(_lifecycle_mutex);
        _lifecycle.quarantineCurrent();
      }
      return StaStatus::failure(result.cleanup_failed ? StaSessionStatusCode::kCleanupFailed
          : unsupported ? StaSessionStatusCode::kUnsupported : StaSessionStatusCode::kRunFailed,
          StaSessionState::kPoisoned, result.message,
          unsupported ? std::optional{StaCapabilityReason::kUnsupportedAnalysisFeatures} : std::nullopt);
    }
    if (recovery) {
      timing_edit::NativeExecutionAuthority authority(timing_edit::TimingEditSessionToken{token}, violation_signal);
      execution = prepared->recover(database, *context, authority, execution);
    }
    context->_physical = std::move(prepared);
    input_rollback.release();
    context->_baseline_ready = true;
    _is_timing_updated = request.backend == StaBackend::kCpu && request.update_strategy == StaUpdateStrategy::kFull;
    return StaStatus::success(StaSessionState::kBaselineExecuted);
  } catch (const std::exception& error) {
    return StaStatus::failure(StaSessionStatusCode::kRunFailed, StaSessionState::kPoisoned, error.what());
  }
}
}  // namespace ista
