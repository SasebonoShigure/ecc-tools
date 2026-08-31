// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "TimingEditValidationInternal.hpp"

#include <type_traits>

#include "TimingEditCanonicalEncoding.hpp"

namespace ista::timing_edit::detail {
namespace {

using Bytes = std::vector<std::uint8_t>;
using Violations = TimingEditContractViolations;

void reject(Violations& errors, std::string field, std::string message)
{
  errors.push_back({TimingEditContractCode::kConflict, std::move(field), std::move(message)});
}

bool arcMatches(const StableArcRef& arc, const StableNodeRef& sink, const StableNetRef& net)
{
  const Bytes sink_bytes = canonicalStableNodeRefBytes(sink);
  const Bytes net_bytes = canonicalStableNetRefBytes(net);
  return std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingArcRef>) {
          if (ref.baseline_key.base.kind != csr::ArcKind::kNet)
            return false;
          StableNodeRef arc_sink{ExistingNodeRef{ref.baseline_key.base.sink}};
          StableNetRef arc_net{ExistingNetRef{ref.baseline_key.base.owner_name}};
          return canonicalStableNodeRefBytes(arc_sink) == sink_bytes && canonicalStableNetRefBytes(arc_net) == net_bytes;
        } else if constexpr (std::is_same_v<Ref, CreatedOldNetArcRef>) {
          return canonicalStableNodeRefBytes(ref.sink) == sink_bytes && canonicalStableNetRefBytes(ref.net) == net_bytes;
        } else if constexpr (std::is_same_v<Ref, CreatedNewNetArcRef>) {
          StableNetRef arc_net{CreatedNetRef{ref.creation_token, CreatedNetRole::kOutputNet}};
          return canonicalStableNodeRefBytes(ref.sink) == sink_bytes && canonicalStableNetRefBytes(arc_net) == net_bytes;
        } else {
          return false;
        }
      },
      arc);
}

std::vector<StableArcRef> matchingArcs(const TimingGraphDelta& delta, bool additions, const StableNodeRef& sink, const StableNetRef& net)
{
  std::vector<StableArcRef> result;
  for (const auto& operation : delta.operations) {
    if (additions) {
      if (const auto* add = std::get_if<AddNetArc>(&operation); add && arcMatches(add->arc, sink, net))
        result.push_back(add->arc);
    } else if (const auto* remove = std::get_if<RemoveNetArc>(&operation); remove && arcMatches(remove->arc, sink, net)) {
      result.push_back(remove->arc);
    }
  }
  return result;
}

bool containsArc(const TimingGraphDelta& delta, bool additions, const StableArcRef& arc)
{
  const Bytes expected = canonicalStableArcRefBytes(arc);
  for (const auto& operation : delta.operations) {
    if (additions) {
      if (const auto* add = std::get_if<AddNetArc>(&operation); add && canonicalStableArcRefBytes(add->arc) == expected)
        return true;
    } else if (const auto* remove = std::get_if<RemoveNetArc>(&operation); remove && canonicalStableArcRefBytes(remove->arc) == expected) {
      return true;
    }
  }
  return false;
}

bool containsReverseReconnect(const TimingGraphDelta& reverse, const ReconnectSink& forward)
{
  for (const auto& operation : reverse.operations)
    if (const auto* reconnect = std::get_if<ReconnectSink>(&operation);
        reconnect && canonicalStableNodeRefBytes(reconnect->sink) == canonicalStableNodeRefBytes(forward.sink)
        && canonicalStableNetRefBytes(reconnect->old_net) == canonicalStableNetRefBytes(forward.new_net)
        && canonicalStableNetRefBytes(reconnect->new_net) == canonicalStableNetRefBytes(forward.old_net))
      return true;
  return false;
}

}  // namespace

TimingEditContractViolations validatePreparedBatchDeltaSemantics(const TimingGraphDelta& forward, const TimingGraphDelta& reverse)
{
  Violations errors;
  for (std::size_t index = 0; index < forward.operations.size(); ++index) {
    if (const auto* add = std::get_if<AddNetArc>(&forward.operations[index]); add && std::holds_alternative<ExistingArcRef>(add->arc))
      reject(errors, "prepared.normalized_forward.operations[" + std::to_string(index) + "].arc",
             "forward delta cannot add a baseline arc");

    const auto* reconnect = std::get_if<ReconnectSink>(&forward.operations[index]);
    if (!reconnect)
      continue;
    const std::string field = "prepared.normalized_forward.operations[" + std::to_string(index) + "]";
    auto removed_old = matchingArcs(forward, false, reconnect->sink, reconnect->old_net);
    auto added_new = matchingArcs(forward, true, reconnect->sink, reconnect->new_net);
    if (removed_old.size() != 1 || added_new.size() != 1) {
      reject(errors, field, "reconnect requires exactly one forward remove-old and one forward add-new net arc");
      continue;
    }
    if (!containsArc(reverse, true, removed_old.front()) || !containsArc(reverse, false, added_new.front()) || !containsReverseReconnect(reverse, *reconnect))
      reject(errors, field, "reverse delta must re-add old arc, remove new arc, and reconnect the sink back");
  }
  return errors;
}

}  // namespace ista::timing_edit::detail
