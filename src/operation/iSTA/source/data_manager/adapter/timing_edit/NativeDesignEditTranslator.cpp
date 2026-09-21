// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDesignEditTranslator.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include "Database.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_edit {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("native buffer insertion: " + std::move(message));
}

NativeTranslationResult preflightPolicy(const PreparedBatch& batch)
{
  if (batch.policyReceipt().disposition == EditDisposition::kFullWithRecord) {
    return {EditDisposition::kFullWithRecord, batch.policyReceipt().reasons};
  }
  for (const auto& edit : batch.normalizedEdits()) {
    if (const auto* parasitic = std::get_if<ParasiticRcInputs>(&edit.rc_inputs)) {
      if (parasitic->parasitic_policy == ParasiticEditPolicy::kFullWithRecord) {
        return {EditDisposition::kFullWithRecord, {"source net parasitics require a fresh full rebuild"}};
      }
      invalid("source net parasitics are unsupported by native buffer insertion");
    }
  }
  return {};
}

void resetDerivedState(Database& database) noexcept
{
  database.get_arc_list().clear();
  database.get_outgoing_arc_list_map().clear();
  database.get_incoming_arc_list_map().clear();
  database.get_start_point_list().clear();
  database.get_end_point_list().clear();
  database.get_timing_order_list().clear();
  database.get_timing_point_map().clear();
  database.get_timing_path_group_list().clear();

  for (auto& [name, net] : database.get_net_map()) {
    static_cast<void>(name);
    net.get_driver_pin().clear();
    net.get_driver_pin_list().clear();
    net.get_load_pin_list().clear();
  }

  database.get_power_activity_map().clear();
  database.get_instance_power_map().clear();
  database.get_power_summary() = PowerSummary{};
  database.get_summary() = Summary{};
}

}  // namespace

NativeTranslationResult NativeDesignEditTranslator::applyNormalizedBatch(Database& database, const PreparedBatch& batch,
                                                                          TimingEditResolverView staged_resolver)
{
  NativeTranslationResult policy = preflightPolicy(batch);
  if (!policy.applied()) {
    return policy;
  }

  auto prepared = detail::prepareNativeBufferInsertions(
      database, batch.normalizedEdits(), staged_resolver,
      detail::nextNativeInstanceId(database));
  // No writes precede preparation. This no-throw publication leaves full
  // graph rebuilding to the selected backend and does not clone Liberty.
  prepared.apply();
  resetDerivedState(database);
  return policy;
}

}  // namespace ista::timing_edit
