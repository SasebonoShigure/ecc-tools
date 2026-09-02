#include "NativeDualCsrAffectedArcIndex.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string>

#include "Database.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

struct ArcBaseLess
{
  bool operator()(const csr::ArcKeyBase& lhs,
                  const csr::ArcKeyBase& rhs) const
  {
    return csr::arcKeyLess(csr::ArcKey{lhs, 0},
                           csr::ArcKey{rhs, 0});
  }
};

}  // namespace

NativeAffectedArcAdjacencyResult collectNativeAffectedArcAdjacency(
    const ista::Database& database,
    const DualCsrArcGroupIndex& committed_index,
    const AffectedCapturePlan& plan)
{
  if (committed_index.session_token != plan.session_token
      || committed_index.generation != plan.expected_generation
      || committed_index.arc_high_water != plan.before.arc_slots)
    return NarrowCaptureError::kBatchBindingMismatch;

  NativeAffectedArcAdjacency adjacency_result;
  std::set<std::string> affected_names;
  for (const auto& request : plan.nodes)
    affected_names.insert(request.expected_key.full_name);
  for (const auto& request : plan.arcs) {
    affected_names.insert(request.expected_post_base.source.full_name);
    affected_names.insert(request.expected_post_base.sink.full_name);
    if (!request.expected_current_key)
      continue;
    affected_names.insert(
        request.expected_current_key->base.source.full_name);
    affected_names.insert(
        request.expected_current_key->base.sink.full_name);
    const auto* group = committed_index.activeGroup(
        request.expected_current_key->base);
    ++adjacency_result.index_group_lookups;
    if (group == nullptr)
      return NarrowCaptureError::kBatchBindingMismatch;
    bool exact = false;
    for (const auto id : group->active_arc_ids) {
      const auto* slot = committed_index.slot(id);
      if (slot != nullptr && slot->ref == request.ref
          && slot->current_active_key
                 == request.expected_current_key) {
        if (exact)
          return NarrowCaptureError::kDuplicateRequest;
        exact = true;
      }
    }
    if (!exact)
      return NarrowCaptureError::kBatchBindingMismatch;
  }
  for (const auto& request : plan.load_nets) {
    const auto found = database.get_net_map().find(request.expected_name);
    if (found == database.get_net_map().end())
      return NarrowCaptureError::kNativeNetNotFound;
    const auto& net = found->second;
    affected_names.insert(net.get_driver_pin());
    affected_names.insert(net.get_driver_pin_list().begin(),
                          net.get_driver_pin_list().end());
    affected_names.insert(net.get_load_pin_list().begin(),
                          net.get_load_pin_list().end());
    affected_names.insert(net.get_pin_name_list().begin(),
                          net.get_pin_name_list().end());
  }
  affected_names.erase(std::string{});

  std::set<std::size_t> result;
  const auto& native_arcs = database.get_arc_list();
  for (const auto& name : affected_names) {
    const auto collect = [&](const auto& adjacency, bool outgoing)
        -> std::optional<NarrowCaptureError> {
      const auto found = adjacency.find(name);
      if (found == adjacency.end())
        return std::nullopt;
      for (std::size_t index : found->second) {
        ++adjacency_result.candidate_neighbor_upper_bound;
        if (index >= native_arcs.size())
          return NarrowCaptureError::kMalformedNativeRecord;
        const auto& arc = native_arcs[index];
        const auto& endpoint = outgoing ? arc.get_source_pin()
                                        : arc.get_sink_pin();
        if (endpoint != name)
          return NarrowCaptureError::kMalformedNativeRecord;
        result.insert(index);
      }
      return std::nullopt;
    };
    if (const auto error = collect(
            database.get_outgoing_arc_list_map(), true))
      return *error;
    if (const auto error = collect(
            database.get_incoming_arc_list_map(), false))
      return *error;
  }

  for (std::size_t index : result) {
    const auto& arc = native_arcs[index];
    const auto outgoing = database.get_outgoing_arc_list_map().find(
        arc.get_source_pin());
    const auto incoming = database.get_incoming_arc_list_map().find(
        arc.get_sink_pin());
    if (outgoing == database.get_outgoing_arc_list_map().end()
        || incoming == database.get_incoming_arc_list_map().end()
        || std::ranges::count(outgoing->second, index) != 1
        || std::ranges::count(incoming->second, index) != 1)
      return NarrowCaptureError::kMalformedNativeRecord;
  }
  adjacency_result.indices_in_native_order.assign(result.begin(),
                                                   result.end());
  return adjacency_result;
}

}  // namespace ista::timing_dual_csr::detail
