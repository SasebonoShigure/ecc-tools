#include "NativeDualCsrReferenceCapture.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

bool byteLess(const std::string& lhs, const std::string& rhs)
{
  return csr::detail::referenceByteStringLess(lhs, rhs);
}

std::optional<NarrowCaptureError> buildReferenceDelta(
    const DualSemanticReadView& committed,
    const AffectedCapturePlan& plan,
    const std::vector<std::optional<csr::NodeKey>>& captured_node_keys,
    NarrowTimingRecords& records)
{
  const auto& before = committed.referencePayload();
  RecordAppend<csr::InstanceRecord> instances{before.instances.size(), {}};
  for (const auto& captured : records.instances) {
    const auto same_name = [&](const auto& existing) { return existing.instance_name == captured.record.instance_name; };
    if (std::ranges::any_of(before.instances, same_name) || std::ranges::any_of(instances.values, same_name))
      return NarrowCaptureError::kDuplicateRequest;
    instances.values.push_back(captured.record);
  }
  if (!instances.values.empty())
    records.reference_payload.instances = std::move(instances);

  RecordAppend<csr::ReferencedLibraryCellModel> cells{before.referenced_library_cells.size(), {}};
  for (const auto& captured : records.library_cells) {
    const auto found = std::ranges::find(before.referenced_library_cells, captured.model.cell_name, &csr::ReferencedLibraryCellModel::cell_name);
    if (found != before.referenced_library_cells.end()) {
      if (*found != captured.model)
        return NarrowCaptureError::kUnplannedDependency;
    } else {
      cells.values.push_back(captured.model);
    }
  }
  if (!cells.values.empty())
    records.reference_payload.referenced_library_cells = std::move(cells);

  RecordAppend<csr::CellPinCapacitanceModel> caps{before.cell_pin_capacitances.size(), {}};
  for (const auto& captured : records.cell_pin_capacitances) {
    const auto id = captured.model.node.value();
    if (id >= plan.after.node_slots || (id >= plan.before.node_slots && (id >= captured_node_keys.size() || !captured_node_keys[id])))
      return NarrowCaptureError::kMalformedNativeRecord;
    const auto found = std::ranges::find(before.cell_pin_capacitances, captured.model.node, &csr::CellPinCapacitanceModel::node);
    if (found != before.cell_pin_capacitances.end()) {
      if (*found != captured.model)
        return NarrowCaptureError::kUnplannedDependency;
    } else {
      caps.values.push_back(captured.model);
    }
  }
  if (!caps.values.empty())
    records.reference_payload.cell_pin_capacitances = std::move(caps);

  if (before.clock_owner_by_node.size() != plan.before.node_slots)
    return NarrowCaptureError::kMalformedNativeRecord;
  if (plan.after.node_slots > plan.before.node_slots)
    records.reference_payload.clock_owner_by_node = RecordAppend<std::optional<std::string>>{
        plan.before.node_slots, std::vector<std::optional<std::string>>(plan.after.node_slots - plan.before.node_slots)};
  return std::nullopt;
}

void canonicalizeOutput(NarrowTimingRecords& records)
{
  std::ranges::sort(records.nodes, [](const auto& lhs, const auto& rhs) {
    return lhs.id.value() < rhs.id.value();
  });
  std::ranges::sort(records.instances,
                    [](const auto& lhs, const auto& rhs) {
                      return byteLess(lhs.record.instance_name,
                                      rhs.record.instance_name);
                    });
  std::ranges::sort(records.arcs, [](const auto& lhs, const auto& rhs) {
    return lhs.id.value() < rhs.id.value();
  });
  std::ranges::sort(records.net_loads,
                    [](const auto& lhs, const auto& rhs) {
                      return byteLess(lhs.net_name, rhs.net_name);
                    });
  std::ranges::sort(records.library_cells,
                    [](const auto& lhs, const auto& rhs) {
                      return byteLess(lhs.model.cell_name,
                                      rhs.model.cell_name);
                    });
  std::ranges::sort(records.cell_pin_capacitances,
                    [](const auto& lhs, const auto& rhs) {
                      return csr::nodeKeyLess(lhs.requested.expected_key,
                                              rhs.requested.expected_key);
                    });
  std::ranges::sort(records.node_roles,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.id.value() < rhs.id.value();
                    });
}

}  // namespace

std::optional<NarrowCaptureError> finalizeNativeDualCsrReferenceCapture(
    const DualSemanticReadView& committed,
    const AffectedCapturePlan& plan,
    const std::vector<std::optional<csr::NodeKey>>& captured_node_keys,
    NarrowTimingRecords& records)
{
  if (captured_node_keys.size() != plan.after.node_slots)
    return NarrowCaptureError::kInvalidHighWater;
  canonicalizeOutput(records);
  if (const auto error = buildReferenceDelta(
          committed, plan, captured_node_keys, records))
    return error;
  return std::nullopt;
}

}  // namespace ista::timing_dual_csr::detail
