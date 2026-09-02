// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <utility>

#include "DualCsrState.hpp"
#include "TimingEditResolver.hpp"

namespace ista::timing_dual_csr {

// One rank-free identity projection is shared by the pre-candidate exact
// binding and the post-update transaction. Fingerprints are auxiliary source
// evidence; the canonical instance/net/node/active-arc values are the
// functional identity.
[[nodiscard]] inline timing_edit::CommittedEditIdentity
deriveDualCsrCommittedIdentity(
    const DualCsrState& graph,
    const timing_edit::TimingEditResolverState& resolver)
{
  timing_edit::CanonicalPreStateEvidence semantic;
  semantic.instances.reserve(graph.reference_payload.instances.size());
  for (const auto& instance : graph.reference_payload.instances)
    semantic.instances.push_back(instance.instance_name);
  for (const auto& [ref, binding] : resolver.netBindings()) {
    static_cast<void>(ref);
    if (!binding.tombstone)
      semantic.nets.push_back(binding.native_name);
  }
  semantic.nodes.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes)
    semantic.nodes.push_back(node.key);
  semantic.arcs.reserve(graph.arc_slots.size());
  for (const auto& slot : graph.arc_slots)
    if (slot.active)
      semantic.arcs.push_back(slot.payload.key);

  std::ranges::sort(semantic.instances);
  std::ranges::sort(semantic.nets);
  std::ranges::sort(semantic.nodes, csr::nodeKeyLess);
  std::ranges::sort(semantic.arcs, csr::arcKeyLess);

  const auto& source = graph.reference_payload.source_identity;
  timing_edit::ComponentFingerprints fingerprints;
  fingerprints.schema_version = source.fingerprint_schema_version;
  fingerprints.topology = source.topology_fingerprint;
  fingerprints.clock_constraint
      = source.clock_constraint_fingerprint;
  fingerprints.library = source.library_fingerprint;
  fingerprints.corner = source.corner_fingerprint;
  fingerprints.parasitic = source.parasitic_fingerprint;
  fingerprints.timing_input = source.timing_input_fingerprint;

  return {graph.generation, std::move(fingerprints),
          std::move(semantic)};
}

}  // namespace ista::timing_dual_csr
