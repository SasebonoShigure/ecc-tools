// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrStateValidation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "TimingSemanticKey.hpp"

#include "DualCsrReferenceValidationInternal.hpp"
#include "DualCsrValidationInternal.hpp"

namespace ista::timing_dual_csr {
namespace {

using detail::reject;

template <typename Enum>
bool knownEnum(Enum value) noexcept
{
  try {
    static_cast<void>(csr::token(value));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool isFinite(double value) noexcept
{
  return std::isfinite(value);
}

void validateTableModel(
    const csr::TimingTableModel& table,
    const csr::TimingTableSignature* signature, const std::string& field,
    DualCsrViolations& violations)
{
  if (signature
      && !(*signature
           == csr::TimingTableSignature{table.present, table.variable1,
                                        table.variable2}))
    reject(violations, DualCsrViolationCode::kExpectedValue, field,
           "table model presence/variables differ from ArcKey signature");

  const bool variables_known
      = knownEnum(table.variable1) && knownEnum(table.variable2);
  if (!variables_known)
    reject(violations, DualCsrViolationCode::kSchema,
           field + ".variables", "table variable enum is unknown");
  if (!table.present) {
    if (table.variable1 != csr::TableVariable::kNone
        || table.variable2 != csr::TableVariable::kNone
        || !table.axes.empty() || !table.values.empty())
      reject(violations, DualCsrViolationCode::kExpectedValue, field,
             "absent table must use none variables and own no data");
    return;
  }
  if (!variables_known)
    return;
  if (table.variable1 == csr::TableVariable::kNone
      && table.variable2 != csr::TableVariable::kNone)
    reject(violations, DualCsrViolationCode::kShape,
           field + ".variables", "variable2 requires variable1");

  const std::size_t dimensions
      = static_cast<std::size_t>(
            table.variable1 != csr::TableVariable::kNone)
        + static_cast<std::size_t>(
            table.variable2 != csr::TableVariable::kNone);
  const bool axes_match = table.axes.size() == dimensions;
  if (!axes_match)
    reject(violations, DualCsrViolationCode::kShape, field + ".axes",
           "axis count must match declared variables");
  for (const auto& axis : table.axes) {
    for (std::size_t index = 0; index < axis.size(); ++index) {
      if (!isFinite(axis[index]))
        reject(violations, DualCsrViolationCode::kExpectedValue,
               field + ".axes", "axis value must be finite");
      if (index != 0 && !(axis[index - 1] < axis[index]))
        reject(violations, DualCsrViolationCode::kShape,
               field + ".axes", "axis values must be strictly increasing");
    }
  }
  for (const double value : table.values)
    if (!isFinite(value))
      reject(violations, DualCsrViolationCode::kExpectedValue,
             field + ".values", "table value must be finite");
  if (table.values.empty())
    return;

  std::size_t expected = 1;
  bool populated_shape_valid = axes_match;
  for (const auto& axis : table.axes) {
    if (axis.empty()) {
      reject(violations, DualCsrViolationCode::kShape,
             field + ".axes", "populated table axis must not be empty");
      populated_shape_valid = false;
    } else if (populated_shape_valid
               && expected
                      > std::numeric_limits<std::size_t>::max()
                            / axis.size()) {
      reject(violations, DualCsrViolationCode::kShape,
             field + ".axes", "table value count overflows size_t");
      populated_shape_valid = false;
    } else if (populated_shape_valid) {
      expected *= axis.size();
    }
  }
  if (populated_shape_valid && table.values.size() != expected)
    reject(violations, DualCsrViolationCode::kShape,
           field + ".values", "value count does not match axes");
}

void validateChild(const csr::ChildTimingArcModel& child,
                   const std::string& field,
                   DualCsrViolations& violations)
{
  if (!knownEnum(child.signature.sense)
      || !knownEnum(child.signature.trigger_transition)
      || !knownEnum(child.signature.check_transition))
    reject(violations, DualCsrViolationCode::kSchema,
           field + ".signature",
           "child timing signature contains an unknown enum");
  if (!isFinite(child.time_unit_scale) || child.time_unit_scale <= 0
      || !isFinite(child.capacitance_unit_scale)
      || child.capacitance_unit_scale <= 0
      || !isFinite(child.slew_derate) || child.slew_derate <= 0)
    reject(violations, DualCsrViolationCode::kExpectedValue, field,
           "child scale or derate is invalid");

  for (std::size_t transition = 0;
       transition < csr::kTransitionCount; ++transition) {
    const bool thresholds_valid
        = isFinite(child.slew_lower_threshold_pct[transition])
          && isFinite(child.slew_upper_threshold_pct[transition])
          && isFinite(child.input_threshold_pct[transition])
          && isFinite(child.output_threshold_pct[transition])
          && child.slew_lower_threshold_pct[transition] >= 0
          && child.slew_upper_threshold_pct[transition] <= 1
          && child.slew_lower_threshold_pct[transition]
                 < child.slew_upper_threshold_pct[transition]
          && child.input_threshold_pct[transition] >= 0
          && child.input_threshold_pct[transition] <= 1
          && child.output_threshold_pct[transition] >= 0
          && child.output_threshold_pct[transition] <= 1;
    if (!thresholds_valid)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             field + ".thresholds",
             "threshold percentages are invalid");
    validateTableModel(child.delay_tables[transition],
                       &child.signature.delay_tables[transition],
                       field + ".delay_tables", violations);
    validateTableModel(child.slew_tables[transition],
                       &child.signature.slew_tables[transition],
                       field + ".slew_tables", violations);
    validateTableModel(child.check_tables[transition], nullptr,
                       field + ".check_tables", violations);
  }
}

void validateTransfer(const csr::CellTransferModel& transfer,
                      const std::string& field,
                      DualCsrViolations& violations)
{
  if (!isFinite(transfer.delay) || !isFinite(transfer.delay_min)
      || !isFinite(transfer.delay_max))
    reject(violations, DualCsrViolationCode::kExpectedValue, field,
           "fallback delay is non-finite");
  std::vector<std::int32_t> child_indices;
  child_indices.reserve(transfer.children.size());
  for (const auto& child : transfer.children) {
    validateChild(child, field + ".children", violations);
    child_indices.push_back(child.signature.arc_index);
  }
  std::ranges::sort(child_indices);
  if (std::ranges::adjacent_find(child_indices) != child_indices.end())
    reject(violations, DualCsrViolationCode::kKey,
           field + ".children.signature.arc_index",
           "arc indices must be unique");
}

void validateArcPayload(const DualCsrState& state,
                        const csr::ArcSemanticPayload& payload,
                        const std::string& field,
                        bool active,
                        DualCsrViolations& violations)
{
  if (!knownEnum(payload.kind))
    reject(violations, DualCsrViolationCode::kSchema, field + ".kind",
           "ArcKind enum is unknown");
  const bool any_eligible
      = payload.eligibility.levelization
        || payload.eligibility.data_arrival
        || payload.eligibility.data_slew || payload.eligibility.clock;
  if ((payload.is_disabled || payload.is_loop_disabled) && any_eligible)
    reject(violations, DualCsrViolationCode::kExpectedValue,
           field + ".eligibility",
           "disabled and loop-disabled arcs must be ineligible");
  if ((payload.eligibility.data_arrival
       || payload.eligibility.data_slew || payload.eligibility.clock)
      && !payload.eligibility.levelization)
    reject(violations, DualCsrViolationCode::kExpectedValue,
           field + ".eligibility",
           "propagation eligibility requires levelization eligibility");

  validateTransfer(payload.transfer, field + ".transfer", violations);
  const bool endpoints_valid
      = payload.source.value() < state.nodes.size()
        && payload.sink.value() < state.nodes.size();
  if (payload.kind == csr::ArcKind::kNet) {
    csr::CellTransferModel canonical;
    canonical.delay = 0.0;
    canonical.delay_min = 0.0;
    canonical.delay_max = 0.0;
    if (!(payload.transfer == canonical) || payload.is_clock_arc)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             field + ".transfer",
             "net arc transfer must use the canonical unbound model");
    if (active && endpoints_valid) {
      const auto& source = state.nodes[payload.source.value()];
      const auto& sink = state.nodes[payload.sink.value()];
      if (source.net_name.empty() || source.net_name != sink.net_name
          || payload.key.base.owner_name != source.net_name)
        reject(violations, DualCsrViolationCode::kKey,
               field + ".key",
               "net arc owner and endpoint net names must be the same nonempty net");
    }
  } else if (payload.kind == csr::ArcKind::kCell) {
    if (endpoints_valid) {
      const auto& source = state.nodes[payload.source.value()];
      const auto& sink = state.nodes[payload.sink.value()];
      if (source.is_port || sink.is_port
          || source.key.instance_name != sink.key.instance_name
          || source.instance_cell != sink.instance_cell
          || payload.key.base.owner_name != source.key.instance_name
          || payload.key.base.library_source_port != source.key.pin_name
          || payload.key.base.library_sink_port != sink.key.pin_name)
        reject(violations, DualCsrViolationCode::kKey,
               field + ".key",
               "cell arc ownership and library ports do not match its same-instance endpoints");
    }
    if (payload.transfer.is_clock_arc != payload.is_clock_arc
        || payload.transfer.is_disabled != payload.is_disabled)
      reject(violations, DualCsrViolationCode::kExpectedValue,
             field + ".transfer",
             "cell transfer and arc flags differ");
    if (payload.transfer.children.size()
        != payload.key.base.library_arcs.size())
      reject(violations, DualCsrViolationCode::kShape,
             field + ".transfer.children",
             "child count differs from ArcKey signature");
    const std::size_t child_count = std::min(
        payload.transfer.children.size(),
        payload.key.base.library_arcs.size());
    for (std::size_t child = 0; child < child_count; ++child)
      if (!(payload.transfer.children[child].signature
            == payload.key.base.library_arcs[child]))
        reject(violations, DualCsrViolationCode::kExpectedValue,
               field + ".transfer.children.signature",
               "child signature differs from ArcKey");
  }

  if (payload.output_load)
    for (const auto& analysis : *payload.output_load)
      for (const double value : analysis)
        if (!isFinite(value) || value < 0)
          reject(violations, DualCsrViolationCode::kExpectedValue,
                 field + ".output_load",
                 "load must be finite and nonnegative");
  const bool loads_present
      = state.reference_payload.parasitics.nets.empty();
  if (active && payload.output_load.has_value() != loads_present)
    reject(violations, DualCsrViolationCode::kExpectedValue,
           field + ".output_load",
           "load presence must follow the global parasitic-domain state");
}

void validateBaseShape(const BaseCsrState& base, std::size_t node_count,
                       const std::string& field,
                       DualCsrViolations& violations)
{
  detail::validateOffsets(base.row_offsets, node_count,
                          base.physical_slots.size(),
                          field + ".row_offsets", violations);
  if (!detail::validExtent(base.offsets_extent, base.row_offsets.size()))
    reject(violations, DualCsrViolationCode::kCapacity,
           field + ".offsets_extent", "extent does not cover offsets");
  if (!detail::validExtent(base.edge_extent, base.physical_slots.size()))
    reject(violations, DualCsrViolationCode::kCapacity,
           field + ".edge_extent", "extent does not cover physical slots");
}

bool payloadMatchesNodes(const DualCsrState& state,
                         const csr::ArcSemanticPayload& payload) noexcept
{
  const auto source = payload.source.value();
  const auto sink = payload.sink.value();
  return source < state.nodes.size() && sink < state.nodes.size()
         && payload.kind == payload.key.base.kind
         && payload.key.base.source == state.nodes[source].key
         && payload.key.base.sink == state.nodes[sink].key;
}

void validateReferenceNodeIds(const DualCsrState& state,
                              DualCsrViolations& violations)
{
  const auto check = [&](const std::vector<csr::NodeId>& ids,
                         const std::string& field) {
    for (const auto id : ids) {
      if (id.value() >= state.nodes.size())
        reject(violations, DualCsrViolationCode::kId, field,
               "NodeId is out of range");
    }
  };
  check(state.reference_payload.ordered_root_ids,
        "reference_payload.ordered_root_ids");
  check(state.reference_payload.ordered_start_ids,
        "reference_payload.ordered_start_ids");
  check(state.reference_payload.ordered_end_ids,
        "reference_payload.ordered_end_ids");
  if (state.reference_payload.clock_owner_by_node.size()
      != state.nodes.size()) {
    reject(violations, DualCsrViolationCode::kShape,
           "reference_payload.clock_owner_by_node",
           "node-indexed clock ownership must cover every NodeId");
  }
}

}  // namespace

DualCsrViolations validateDualCsrState(const DualCsrState& state)
{
  DualCsrViolations violations;
  const std::size_t node_count = state.nodes.size();
  const std::size_t arc_count = state.arc_slots.size();

  if (state.session_token.value() == 0)
    reject(violations, DualCsrViolationCode::kSession, "session_token",
           "session token must be nonzero");
  if (state.high_water.node_slots != node_count)
    reject(violations, DualCsrViolationCode::kHighWater,
           "high_water.node_slots", "high-water must equal node slot count");
  if (state.high_water.arc_slots != arc_count)
    reject(violations, DualCsrViolationCode::kHighWater,
           "high_water.arc_slots", "high-water must equal arc slot count");

  bool node_keys_valid = true;
  for (std::size_t index = 0; index < node_count; ++index) {
    if (state.nodes[index].id.value() != index)
      reject(violations, DualCsrViolationCode::kId, "nodes.id",
             "NodeId must equal stable slot index");
    if (const auto error = csr::validateNodeKey(state.nodes[index].key)) {
      node_keys_valid = false;
      reject(violations, DualCsrViolationCode::kKey, "nodes.key", *error);
    }
  }
  if (node_keys_valid) {
    std::vector<const csr::NodeKey*> ordered_nodes;
    ordered_nodes.reserve(node_count);
    for (const auto& record : state.nodes)
      ordered_nodes.push_back(&record.key);
    std::ranges::sort(ordered_nodes,
                      [](const csr::NodeKey* lhs, const csr::NodeKey* rhs) {
                        return csr::nodeKeyLess(*lhs, *rhs);
                      });
    for (std::size_t index = 1; index < ordered_nodes.size(); ++index)
      if (*ordered_nodes[index - 1] == *ordered_nodes[index])
        reject(violations, DualCsrViolationCode::kKey, "nodes.key",
               "NodeKey must be unique");
  }

  bool arc_keys_valid = true;
  for (std::size_t index = 0; index < state.arc_slots.size(); ++index) {
    const auto& slot = state.arc_slots[index];
    if (const auto error = csr::validateArcKeyBase(slot.payload.key.base)) {
      arc_keys_valid = false;
      reject(violations, DualCsrViolationCode::kKey,
             "arc_slots.payload.key", *error);
    }
    validateArcPayload(state, slot.payload,
                       "arc_slots[" + std::to_string(index) + "].payload",
                       slot.active, violations);
  }
  if (arc_keys_valid) {
    std::vector<const csr::ArcSemanticPayload*> ordered_arcs;
    ordered_arcs.reserve(arc_count);
    for (const auto& slot : state.arc_slots)
      if (slot.active)
        ordered_arcs.push_back(&slot.payload);
    std::ranges::sort(
        ordered_arcs, [](const csr::ArcSemanticPayload* lhs,
                         const csr::ArcSemanticPayload* rhs) {
          return csr::arcKeyLess(lhs->key, rhs->key);
        });
    std::uint64_t expected_ordinal = 0;
    for (std::size_t index = 0; index < ordered_arcs.size(); ++index) {
      if (index == 0
          || !(ordered_arcs[index - 1]->key.base
               == ordered_arcs[index]->key.base))
        expected_ordinal = 0;
      if (ordered_arcs[index]->key.parallel_ordinal != expected_ordinal)
        reject(violations, DualCsrViolationCode::kKey,
               "arc_slots.payload.key.parallel_ordinal",
               "parallel ordinals must be continuous from zero within each ArcKeyBase");
      ++expected_ordinal;
      if (index != 0
          && ordered_arcs[index - 1]->key == ordered_arcs[index]->key)
        reject(violations, DualCsrViolationCode::kKey,
               "arc_slots.active.payload.key",
               "ArcKey must be unique across active logical arcs");
    }
  }

  validateBaseShape(state.base_out, node_count, "base_out", violations);
  validateBaseShape(state.base_in, node_count, "base_in", violations);
  const bool aux_offsets_ok = detail::validateOffsets(
      state.aux_out.row_offsets, state.aux_out.row_sources.size(),
      state.aux_out.arc_ids.size(), "aux_out.row_offsets", violations);
  if (state.aux_out.source_to_aux_row.size() != node_count)
    reject(violations, DualCsrViolationCode::kShape,
           "aux_out.source_to_aux_row",
           "mapping must contain one entry per NodeId");
  if (!detail::validExtent(state.aux_out.offsets_extent,
                           state.aux_out.row_offsets.size())
      || !detail::validExtent(state.aux_out.source_extent,
                              state.aux_out.row_sources.size())
      || !detail::validExtent(state.aux_out.edge_extent,
                              state.aux_out.arc_ids.size())
      || !detail::validExtent(state.aux_out.mapping_extent,
                              state.aux_out.source_to_aux_row.size())) {
    reject(violations, DualCsrViolationCode::kCapacity, "aux_out.extents",
           "Aux extents must cover all logical elements");
  }

  std::vector<std::uint64_t> out_seen(arc_count, 0);
  std::vector<std::uint64_t> in_seen(arc_count, 0);
  const auto count_arc = [&](const std::optional<csr::ArcId>& maybe_arc,
                             csr::NodeId row, bool outgoing,
                             const std::string& field) {
    if (!maybe_arc)
      return;
    const auto id = maybe_arc->value();
    if (id >= arc_count) {
      reject(violations, DualCsrViolationCode::kId, field,
             "ArcId is out of range");
      return;
    }
    const auto& slot = state.arc_slots[id];
    if (!slot.active)
      reject(violations, DualCsrViolationCode::kAdjacency, field,
             "inactive ArcId appears in active adjacency");
    if (!payloadMatchesNodes(state, slot.payload))
      reject(violations, DualCsrViolationCode::kKey, field,
             "arc payload does not match node keys/endpoints");
    const bool row_matches = outgoing ? slot.payload.source == row
                                      : slot.payload.sink == row;
    if (!row_matches)
      reject(violations, DualCsrViolationCode::kAdjacency, field,
             "arc endpoint does not match CSR row");
    ++(outgoing ? out_seen[id] : in_seen[id]);
  };

  if (state.base_out.row_offsets.size() == node_count + 1) {
    for (std::size_t row = 0; row < node_count; ++row)
      for (std::uint64_t slot = state.base_out.row_offsets[row];
           slot < state.base_out.row_offsets[row + 1]
           && slot < state.base_out.physical_slots.size(); ++slot)
        count_arc(state.base_out.physical_slots[slot], csr::NodeId{row}, true,
                  "base_out.physical_slots");
  }
  if (state.base_in.row_offsets.size() == node_count + 1) {
    for (std::size_t row = 0; row < node_count; ++row)
      for (std::uint64_t slot = state.base_in.row_offsets[row];
           slot < state.base_in.row_offsets[row + 1]
           && slot < state.base_in.physical_slots.size(); ++slot)
        count_arc(state.base_in.physical_slots[slot], csr::NodeId{row}, false,
                  "base_in.physical_slots");
  }

  if (aux_offsets_ok) {
    std::vector<std::int64_t> expected_map(node_count, -1);
    for (std::size_t row = 0; row < state.aux_out.row_sources.size(); ++row) {
      const csr::NodeId source = state.aux_out.row_sources[row];
      if (source.value() >= node_count) {
        reject(violations, DualCsrViolationCode::kId,
               "aux_out.row_sources", "source NodeId is out of range");
        continue;
      }
      if (row > 0
          && state.aux_out.row_sources[row - 1].value() >= source.value())
        reject(violations, DualCsrViolationCode::kShape,
               "aux_out.row_sources",
               "Aux rows must be strictly ordered by source NodeId");
      expected_map[source.value()] = static_cast<std::int64_t>(row);
      for (std::uint64_t offset = state.aux_out.row_offsets[row];
           offset < state.aux_out.row_offsets[row + 1]; ++offset)
        count_arc(state.aux_out.arc_ids[offset], source, true,
                  "aux_out.arc_ids");
    }
    if (state.aux_out.source_to_aux_row != expected_map)
      reject(violations, DualCsrViolationCode::kAdjacency,
             "aux_out.source_to_aux_row",
             "mapping must be the exact inverse of row_sources");
  }

  for (std::size_t id = 0; id < arc_count; ++id) {
    const bool active = state.arc_slots[id].active;
    if (!payloadMatchesNodes(state, state.arc_slots[id].payload))
      reject(violations, DualCsrViolationCode::kKey, "arc_slots.payload",
             "arc payload does not match node keys/endpoints");
    if ((active && (out_seen[id] != 1 || in_seen[id] != 1))
        || (!active && (out_seen[id] != 0 || in_seen[id] != 0)))
      reject(violations, DualCsrViolationCode::kTranspose, "arc_slots",
             "active arc must occur once outgoing and once incoming");
  }

  const std::uint64_t active_arcs = static_cast<std::uint64_t>(
      std::ranges::count_if(state.arc_slots,
                            [](const DualCsrArcSlot& slot) {
                              return slot.active;
                            }));
  const std::uint64_t base_physical = static_cast<std::uint64_t>(
      state.base_out.physical_slots.size());
  const std::uint64_t base_active = static_cast<std::uint64_t>(
      std::ranges::count_if(
          state.base_out.physical_slots,
          [](const std::optional<csr::ArcId>& arc) {
            return arc.has_value();
          }));
  const std::uint64_t aux_rows = static_cast<std::uint64_t>(
      state.aux_out.row_sources.size());
  const std::uint64_t aux_edges = static_cast<std::uint64_t>(
      state.aux_out.arc_ids.size());
  const auto& telemetry = state.telemetry;
  const std::uint64_t expected_before_generation
      = state.generation.value() == 0 ? 0 : state.generation.value() - 1;
  if (telemetry.generation_before != expected_before_generation
      || telemetry.generation_after != state.generation.value())
    reject(violations, DualCsrViolationCode::kGeneration,
           "telemetry.generation",
           "telemetry generation must describe the committed update");
  if (telemetry.logical_edges_after != active_arcs
      || telemetry.base_physical_edges != base_physical
      || telemetry.base_active_edges != base_active
      || telemetry.base_tombstones != base_physical - base_active
      || telemetry.aux_rows != aux_rows
      || telemetry.aux_edges != aux_edges
      || base_active + aux_edges != active_arcs)
    reject(violations, DualCsrViolationCode::kExpectedValue,
           "telemetry.structure",
           "telemetry counts must exactly match active Base/Aux storage");
  if (telemetry.fallback_count != 0)
    reject(violations, DualCsrViolationCode::kConflict,
           "telemetry.fallback_count",
           "a committed targeted state cannot contain fallback");

  const auto& identity = state.reference_payload.source_identity;
  if (identity.session_token != state.session_token.value())
    reject(violations, DualCsrViolationCode::kSession,
           "reference_payload.source_identity.session_token",
           "source identity must match state session");
  if (identity.graph_generation != state.generation.value())
    reject(violations, DualCsrViolationCode::kGeneration,
           "reference_payload.source_identity.graph_generation",
           "source identity must match state generation");
  validateReferenceNodeIds(state, violations);
  detail::validateReferencePayload(state, violations);
  return violations;
}

}  // namespace ista::timing_dual_csr
