// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "IncrementalStaSession.hpp"
#include "PhysicalStaSessionState.hpp"
#include "PhysicalStaPlacement.hpp"
#include "PhysicalGreedySelection.hpp"
#if ISTA_ENABLE_CUDA
#include "StaCudaSessionState.hpp"
#endif
#include "StaSessionApiGuard.hpp"
#include "StaCsrSessionContext.hpp"
#include "STAInterface.hpp"
#include "StaEditOperation.hpp"
#include "DataManager.hpp"

namespace ista {
StaExecutionResult IncrementalStaSession::runPhysicalBaseline(const PhysicalStaConfig& config)
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard) return StaExecutionResult::failure(invalidState("physical baseline cannot reenter the session"));
  return runPhysicalBaselineImpl(config, false);
}
PhysicalStaBatchResult IncrementalStaSession::recoverPhysicalBaseline(const PhysicalStaConfig& config)
{
  SessionApiGuard guard(this, _api_mutex);
  if (!guard) throw StaSessionException(invalidState("physical recovery cannot reenter the session"), request());
  auto result = runPhysicalBaselineImpl(config, true);
  if (!result.ok()) throw StaSessionException(std::move(result.status), request());
  return {_context->_physical->generation(*_context).value(), *result.record};
}
StaExecutionResult IncrementalStaSession::runPhysicalBaselineImpl(const PhysicalStaConfig& config, bool recovery)
{
  if (state() != StaSessionState::kInitialized || !_context || !_token.load() || !_resolved_record)
    return StaExecutionResult::failure(invalidState("physical baseline requires an initialized session"));
  try {
    const timing_edit::NativeExecutionScopeViolationSignal violation{this, [](void* opaque) noexcept {
      static_cast<IncrementalStaSession*>(opaque)->_state.store(StaSessionState::kPoisoned, std::memory_order_release);
    }};
    auto execution = *_resolved_record;
    auto status = STAInterface::getInst().runSessionPhysicalBaseline(_token.load(), _context.get(), _request, config, recovery, execution, violation);
    if (!status.ok()) {
      _state.store(StaSessionState::kPoisoned, std::memory_order_release);
      return StaExecutionResult::failure(withCurrentState(std::move(status)));
    }
    _baseline_executed = true;
    _state.store(StaSessionState::kBaselineExecuted, std::memory_order_release);
    return StaExecutionResult::success(execution, state());
  } catch (...) {
    _state.store(StaSessionState::kPoisoned, std::memory_order_release);
    throw;
  }
}
PhysicalStaBatchResult IncrementalStaSession::applyPhysicalBatch(const timing_edit::EditBatch& edits,
    std::span<const PhysicalBufferLocation> locations)
{
  StaEditOperation operation(*this);
  auto& context = operation.context();
  if (!context._physical)
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kInvalidState, state(),
        "physical batch requires its committed physical baseline"), request());
  try {
    return context._physical->applyBatch(STADM.getDatabase(), context, operation.authority(), edits, locations);
  } catch (const std::exception& error) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), error.what()), request());
  }
}
PhysicalStaPlacement IncrementalStaSession::readPhysicalPlacement(
    std::uint64_t expected_generation, const std::vector<std::string>& cell_names)
{
  StaEditOperation operation(*this);
  auto& context = operation.context();
  if (!context._physical)
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kInvalidState, state(),
        "physical placement requires its committed physical baseline"), request());
  try {
    auto& physical = *context._physical;
    if (physical.generation(context).value() != expected_generation)
      throw std::invalid_argument("physical placement generation is stale");
    PhysicalStaPlacement result;
    result.generation = expected_generation;
    result.dbu_per_um = physical.technology.dbu_per_um;
    result.geometry = physical.placementRows().geometry();
    for (const auto& name : cell_names) {
      auto* master = physical.design.get_layout()->get_cell_master_list()->find_cell_master(name);
      if (!master) throw std::invalid_argument("physical buffer master is absent");
      physical.placementRows().requireBufferMaster(*master);
      if (!result.cells.emplace(name, std::array{static_cast<std::int32_t>(master->get_width()), static_cast<std::int32_t>(master->get_height())}).second)
        throw std::invalid_argument("physical buffer master names must be unique");
    }
    return result;
  } catch (const std::exception& error) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), error.what()), request());
  }
}
PhysicalGreedySelection IncrementalStaSession::selectPhysicalBuffers(
    std::uint64_t expected_generation, const std::vector<std::string>& cell_names,
    std::uint64_t top_k, std::uint64_t max_buffers)
{
  StaEditOperation operation(*this);
  auto& context = operation.context();
  if (!context._physical)
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kInvalidState, state(),
        "greedy selection requires its committed physical baseline"), request());
  try {
    auto& physical = *context._physical;
    if (physical.generation(context).value() != expected_generation)
      throw std::invalid_argument("greedy selection generation is stale");
    auto& database = STADM.getDatabase();
    physical.prepareReport(database, context, operation.authority());
    const auto& resolver = [&]() -> const timing_edit::TimingEditResolverState& {
      if (physical.full) return physical.full->resolver();
      if (physical.cpu_native) return physical.cpu_native->resolver;
#if ISTA_ENABLE_CUDA
      auto& cuda = context.cudaState(operation.authority());
      if (cuda.native) return cuda.native->resolver;
#endif
      throw std::logic_error("greedy selection has no committed resolver");
    }();
    return ista::selectPhysicalBuffers(database, context.libraryModels(database), resolver,
        expected_generation, cell_names, top_k, max_buffers, physical.technology.dbu_per_um);
  } catch (const std::exception& error) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), error.what()), request());
  }
}
PhysicalStaCurrentResult IncrementalStaSession::readPhysicalResult(bool summary_only)
{
  StaEditOperation operation(*this);
  auto& context = operation.context();
  if (!context._physical)
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kInvalidState, state(),
        "physical result requires its committed physical baseline"), request());
  try {
    return context._physical->read(STADM.getDatabase(), context, operation.authority(), summary_only);
  } catch (const std::exception& error) {
    throw StaSessionException(StaStatus::failure(StaSessionStatusCode::kRunFailed, state(), error.what()), request());
  }
}
}  // namespace ista
