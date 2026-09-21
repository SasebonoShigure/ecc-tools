// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrBaselineFactory.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "CsrReferenceValidator.hpp"
#include "DualCsrBaselineFactoryInstrumentationInternal.hpp"
#include "DualCsrValidationInternal.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::timing_dual_csr {
namespace {

thread_local std::uint64_t baseline_factory_calls = 0;

csr::ArcSemanticPayload semanticPayload(const csr::ArcRecord& arc)
{
  return {arc.key,
          arc.source,
          arc.sink,
          arc.kind,
          arc.is_clock_arc,
          arc.is_disabled,
          arc.is_loop_disabled,
          arc.eligibility,
          arc.transfer,
          arc.output_load};
}

template <typename T>
csr::CsrBufferExtent extentOf(const std::vector<T>& values)
{
  return {values.size(), values.capacity()};
}

DualCsrViolationCode contractCode(const std::string& field)
{
  if (field.find("schema") != std::string::npos)
    return DualCsrViolationCode::kSchema;
  if (field.find("session_token") != std::string::npos)
    return DualCsrViolationCode::kSession;
  if (field.find("generation") != std::string::npos)
    return DualCsrViolationCode::kGeneration;
  if (field.find("id") != std::string::npos
      || field.find("offset") != std::string::npos)
    return DualCsrViolationCode::kId;
  if (field.find("key") != std::string::npos)
    return DualCsrViolationCode::kKey;
  return DualCsrViolationCode::kShape;
}

void validateBaselineInput(const csr::CsrReferenceInput& input,
                           timing_edit::TimingEditSessionToken token,
                           timing_edit::AppendOnlyHighWater high_water,
                           DualCsrViolations& violations)
{
  using detail::reject;
  const auto contract_errors
      = csr::validateReferenceInputForSession(input, token.value());
  for (const auto& error : contract_errors)
    reject(violations, contractCode(error.field),
           "input." + error.field, error.message);

  if (token.value() == 0)
    reject(violations, DualCsrViolationCode::kSession, "session_token",
           "session token must be nonzero");
  if (input.source_identity.session_token != token.value())
    reject(violations, DualCsrViolationCode::kSession,
           "input.source_identity.session_token",
           "baseline input belongs to a different session");
  if (input.source_identity.graph_generation != 0)
    reject(violations, DualCsrViolationCode::kGeneration,
           "input.source_identity.graph_generation",
           "baseline graph generation must be zero");
  if (high_water.node_slots != input.nodes.size()
      || high_water.arc_slots != input.arcs.size())
    reject(violations, DualCsrViolationCode::kHighWater, "high_water",
           "baseline high-water must match dense node/arc counts");
  detail::validateOffsets(input.out_row_offsets, input.nodes.size(),
                          input.out_arc_ids.size(), "input.out_row_offsets",
                          violations);
  detail::validateOffsets(input.in_row_offsets, input.nodes.size(),
                          input.in_arc_ids.size(), "input.in_row_offsets",
                          violations);
  for (std::size_t index = 0; index < input.nodes.size(); ++index)
    if (input.nodes[index].id.value() != index)
      reject(violations, DualCsrViolationCode::kId, "input.nodes.id",
             "baseline NodeId must be dense");
  for (std::size_t index = 0; index < input.arcs.size(); ++index) {
    const auto& arc = input.arcs[index];
    if (arc.id.value() != index || arc.source.value() >= input.nodes.size()
        || arc.sink.value() >= input.nodes.size())
      reject(violations, DualCsrViolationCode::kId, "input.arcs",
             "baseline ArcId/endpoints must be dense and in range");
  }
}

}  // namespace

std::uint64_t detail::currentBaselineFactoryCallCount() noexcept
{
  return baseline_factory_calls;
}

DualCsrBaselineAttempt DualCsrBaselineFactory::create(
    const csr::CsrReferenceInput& input,
    timing_edit::TimingEditSessionToken session_token,
    timing_edit::AppendOnlyHighWater high_water)
{
  if (incregpusta_debug_checks_enabled && baseline_factory_calls != std::numeric_limits<std::uint64_t>::max())
    ++baseline_factory_calls;
  DualCsrBaselineAttempt attempt;
  validateBaselineInput(input, session_token, high_water, attempt.violations);
  if (!attempt.violations.empty())
    return attempt;

  DualCsrState state;
  state.session_token = session_token;
  state.generation = timing_edit::TransactionGeneration{0};
  state.high_water = high_water;
  state.nodes = input.nodes;
  state.arc_slots.reserve(input.arcs.size());
  for (const auto& arc : input.arcs)
    state.arc_slots.push_back({semanticPayload(arc), true});

  state.base_out.row_offsets = input.out_row_offsets;
  state.base_out.physical_slots.reserve(input.out_arc_ids.size());
  for (const auto arc : input.out_arc_ids)
    state.base_out.physical_slots.emplace_back(arc);
  state.base_out.offsets_extent = extentOf(state.base_out.row_offsets);
  state.base_out.edge_extent = extentOf(state.base_out.physical_slots);

  state.base_in.row_offsets = input.in_row_offsets;
  state.base_in.physical_slots.reserve(input.in_arc_ids.size());
  for (const auto arc : input.in_arc_ids)
    state.base_in.physical_slots.emplace_back(arc);
  state.base_in.offsets_extent = extentOf(state.base_in.row_offsets);
  state.base_in.edge_extent = extentOf(state.base_in.physical_slots);

  state.aux_out.source_to_aux_row.assign(input.nodes.size(), -1);
  state.aux_out.rows_extent = extentOf(state.aux_out.row_arcs);
  state.aux_out.source_extent = extentOf(state.aux_out.row_sources);
  state.aux_out.mapping_extent = extentOf(state.aux_out.source_to_aux_row);
  state.reference_payload = makeDualCsrReferencePayload(input);

  state.telemetry.generation_before = 0;
  state.telemetry.generation_after = 0;
  state.telemetry.logical_edges_before = input.arcs.size();
  state.telemetry.logical_edges_after = input.arcs.size();
  state.telemetry.base_physical_edges
      = input.out_arc_ids.size();
  state.telemetry.base_active_edges = input.out_arc_ids.size();
  state.telemetry.base_tombstones = 0;
  state.telemetry.aux_rows = 0;
  state.telemetry.aux_edges = 0;

  attempt.violations = validateDualCsrState(state);
  if (attempt.violations.empty())
    attempt.state = std::move(state);
  return attempt;
}

}  // namespace ista::timing_dual_csr
