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
  auto instances = committed.referencePayload().instances;
  for (const auto& captured : records.instances) {
    if (std::ranges::any_of(instances, [&](const auto& existing) {
          return existing.instance_name == captured.record.instance_name;
        }))
      return NarrowCaptureError::kDuplicateRequest;
    instances.push_back(captured.record);
  }
  std::ranges::sort(instances, [](const auto& lhs, const auto& rhs) {
    return byteLess(lhs.instance_name, rhs.instance_name);
  });
  if (instances != committed.referencePayload().instances)
    records.reference_payload.instances
        = ValueReplacement<std::vector<csr::InstanceRecord>>{
            committed.referencePayload().instances, std::move(instances)};

  auto cells = committed.referencePayload().referenced_library_cells;
  for (const auto& captured : records.library_cells) {
    const auto found = std::ranges::find_if(
        cells, [&](const auto& existing) {
          return existing.cell_name == captured.model.cell_name;
        });
    if (found != cells.end()) {
      if (*found != captured.model)
        return NarrowCaptureError::kUnplannedDependency;
    } else {
      cells.push_back(captured.model);
    }
  }
  std::ranges::sort(cells, [](const auto& lhs, const auto& rhs) {
    return byteLess(lhs.cell_name, rhs.cell_name);
  });
  if (cells != committed.referencePayload().referenced_library_cells)
    records.reference_payload.referenced_library_cells
        = ValueReplacement<
            std::vector<csr::ReferencedLibraryCellModel>>{
            committed.referencePayload().referenced_library_cells,
            std::move(cells)};

  const auto nodeKeyForId = [&](csr::NodeId id)
      -> std::optional<csr::NodeKey> {
    if (id.value() < plan.before.node_slots) {
      const auto node = committed.nodeById(id);
      if (!node)
        return std::nullopt;
      return node.value().key;
    }
    if (id.value() >= captured_node_keys.size())
      return std::nullopt;
    return captured_node_keys[id.value()];
  };

  auto caps = committed.referencePayload().cell_pin_capacitances;
  for (const auto& captured : records.cell_pin_capacitances) {
    const auto found = std::ranges::find_if(
        caps, [&](const auto& existing) {
          return existing.node == captured.model.node;
        });
    if (found != caps.end()) {
      if (*found != captured.model)
        return NarrowCaptureError::kUnplannedDependency;
    } else {
      caps.push_back(captured.model);
    }
  }
  for (const auto& cap : caps)
    if (!nodeKeyForId(cap.node))
      return NarrowCaptureError::kMalformedNativeRecord;
  std::ranges::sort(caps, [&](const auto& lhs, const auto& rhs) {
    return byteLess(nodeKeyForId(lhs.node)->full_name,
                    nodeKeyForId(rhs.node)->full_name);
  });
  if (caps != committed.referencePayload().cell_pin_capacitances)
    records.reference_payload.cell_pin_capacitances
        = ValueReplacement<
            std::vector<csr::CellPinCapacitanceModel>>{
            committed.referencePayload().cell_pin_capacitances,
            std::move(caps)};

  const auto& old_owners = committed.referencePayload().clock_owner_by_node;
  if (old_owners.size() != plan.before.node_slots)
    return NarrowCaptureError::kMalformedNativeRecord;
  auto owners = old_owners;
  owners.resize(plan.after.node_slots, std::nullopt);
  if (owners != old_owners)
    records.reference_payload.clock_owner_by_node
        = ValueReplacement<std::vector<std::optional<std::string>>>{
            old_owners, std::move(owners)};
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
  if (const auto error = buildReferenceDelta(
          committed, plan, captured_node_keys, records))
    return error;
  canonicalizeOutput(records);
  return std::nullopt;
}

}  // namespace ista::timing_dual_csr::detail
