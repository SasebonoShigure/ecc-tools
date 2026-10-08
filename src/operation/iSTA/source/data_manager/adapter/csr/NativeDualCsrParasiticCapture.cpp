// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "NativeDualCsrParasiticCapture.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include <algorithm>
#include <stdexcept>

namespace ista::timing_dual_csr::detail {
std::optional<NarrowCaptureError> captureNativeDualCsrParasitics(
    const Database& database, const timing_edit::NativeTargetedEditBatch& native,
    const DualSemanticReadView& committed, const AffectedCapturePlan& plan, NarrowTimingRecords& records)
{
  const auto& before = committed.referencePayload();
  if (before.parasitics.nets.empty()) return std::nullopt;
  ParasiticNetDelta nets{before.parasitics.nets.size(), {}, {}};
  for (const auto& request : plan.load_nets) {
    const auto found = database.get_parasitic_library().get_net_map().find(request.expected_name);
    if (found == database.get_parasitic_library().get_net_map().end())
      return NarrowCaptureError::kParasiticModeMismatch;
    auto after = csr::detail::projectCanonicalParasiticNet(found->first, found->second);
    const auto slot = committed.parasiticNetSlot(found->first);
    if (slot) {
      const auto& expected = before.parasitics.nets.at(*slot);
      if (after != expected) nets.changes.push_back({*slot, expected, std::move(after)});
    } else nets.appends.push_back(std::move(after));
  }
  if (!nets.changes.empty() || !nets.appends.empty()) records.reference_payload.parasitics = std::move(nets);
  StartSeedDelta seeds{before.start_seeds.size(), {}, {}};
  for (const auto& binding : native.patch.rc_bindings) {
    if (!binding.replacement.tree || binding.replacement.terminal != 0
        || binding.node.value() >= plan.before.node_slots) continue;
    const auto node = committed.nodeById(binding.node);
    if (!node) return NarrowCaptureError::kMalformedNativeRecord;
    // Buffer insertion preserves existing starts and their native full-name order.
    const auto& name = node.value().key.full_name;
    const auto seed = std::lower_bound(before.start_seeds.begin(), before.start_seeds.end(), name,
        [&](const csr::StartSeedModel& seed, const std::string& key) {
          const auto node = committed.nodeById(seed.seed_node);
          if (!node) throw std::invalid_argument("committed RC seed node is absent");
          return csr::detail::referenceByteStringLess(node.value().key.full_name, key);
        });
    if (seed == before.start_seeds.end() || seed->seed_node != binding.node
        || seed->availability != csr::StartSeedAvailability::kDeferredSequential) continue;
    if (seed->parasitic != binding.expected) return NarrowCaptureError::kBatchBindingMismatch;
    auto updated = *seed;
    updated.parasitic = binding.replacement;
    seeds.changes.push_back({static_cast<std::uint64_t>(seed - before.start_seeds.begin()), *seed, std::move(updated)});
  }
  if (!seeds.changes.empty()) records.reference_payload.start_seeds = std::move(seeds);
  return std::nullopt;
}
}  // namespace ista::timing_dual_csr::detail
