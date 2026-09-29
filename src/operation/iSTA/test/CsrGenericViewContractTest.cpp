#include "IncreGpuStaBuildConfig.hpp"
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <exception>
#include <iostream>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr::test {
namespace {

class InternalHoleView final : public ActiveCsrEdgeView
{
 public:
  explicit InternalHoleView(const CsrReferenceInput& input) : _flat(input) {}

  [[nodiscard]] std::size_t nodeCount() const noexcept override { return _flat.nodeCount(); }
  [[nodiscard]] std::size_t arcSlotCount() const noexcept override { return _flat.arcSlotCount() + 1; }
  [[nodiscard]] std::size_t activeArcCount() const noexcept override { return _flat.activeArcCount(); }
  [[nodiscard]] bool isActive(ArcId arc_id) const noexcept override { return arc_id.value() < arcSlotCount() && arc_id.value() != _hole; }
  [[nodiscard]] ExpectedRef<const ArcSemanticPayload, CsrViewError> arc(ArcId arc_id) const noexcept override
  {
    if (arc_id.value() == _hole)
      return ExpectedRef<const ArcSemanticPayload, CsrViewError>::failure(CsrViewError::kInactiveArc);
    return _flat.arc(unmap(arc_id));
  }
  [[nodiscard]] EdgeVisitResult forEachActiveArc(const EdgeVisitor& visitor) const override { return remapVisit([&](const EdgeVisitor& wrapped) { return _flat.forEachActiveArc(wrapped); }, visitor); }
  [[nodiscard]] EdgeVisitResult forEachOutgoing(NodeId node, const EdgeVisitor& visitor) const override
  {
    return remapVisit([&](const EdgeVisitor& wrapped) { return _flat.forEachOutgoing(node, wrapped); }, visitor);
  }
  [[nodiscard]] EdgeVisitResult forEachIncoming(NodeId node, const EdgeVisitor& visitor) const override
  {
    return remapVisit([&](const EdgeVisitor& wrapped) { return _flat.forEachIncoming(node, wrapped); }, visitor);
  }

 private:
  [[nodiscard]] ArcId map(ArcId id) const noexcept { return ArcId{id.value() < _hole ? id.value() : id.value() + 1}; }
  [[nodiscard]] ArcId unmap(ArcId id) const noexcept { return ArcId{id.value() < _hole ? id.value() : id.value() - 1}; }
  template <typename Visit>
  [[nodiscard]] EdgeVisitResult remapVisit(Visit&& visit, const EdgeVisitor& visitor) const
  {
    if (!visitor)
      return EdgeVisitResult::fail(CsrViewError::kCallbackError);
    return visit([&](const EdgeEntry& entry) { return visitor(EdgeEntry{map(entry.arc_id), entry.storage_kind}); });
  }

  FlatCsrEdgeView _flat;
  static constexpr std::size_t _hole{1};
};

void checkVisitorContract(const FlatCsrEdgeView& view)
{
  std::size_t visits = 0;
  const EdgeVisitResult stopped = view.forEachActiveArc([&](const EdgeEntry&) {
    ++visits;
    return EdgeVisitResult::stopVisit();
  });
  requireCpuReference(stopped.control == EdgeVisitControl::kStop && visits == 1, "active visitor did not propagate Stop");

  const EdgeVisitResult failed = view.forEachActiveArc([](const EdgeEntry&) { return EdgeVisitResult::fail(CsrViewError::kCallbackError); });
  requireCpuReference(failed.control == EdgeVisitControl::kError && failed.error == CsrViewError::kCallbackError,
                      "active visitor did not propagate Error");
  const EdgeVisitResult contradictory = view.forEachActiveArc(
      [](const EdgeEntry&) {
        return EdgeVisitResult{EdgeVisitControl::kContinue,
                               CsrViewError::kArcOutOfRange};
      });
  requireCpuReference(contradictory.control == EdgeVisitControl::kError && contradictory.error == CsrViewError::kCallbackError,
                      "active visitor accepted Continue with an error payload");
  requireCpuReference(view.forEachOutgoing(NodeId{view.nodeCount()}, [](const EdgeEntry&) { return EdgeVisitResult::continueVisit(); }).error
                          == CsrViewError::kNodeOutOfRange,
                      "out-of-range node did not produce typed failure");
}

void checkHoleLanes(const CsrTimingResult& timing, std::size_t hole, std::size_t slots)
{
  const auto check_scalar = [&](const auto& lane, const char* name) {
    requireCpuReference(lane.presence.size() == slots && lane.values.size() == slots, std::string(name) + " does not use arcSlotCount");
    requireCpuReference(lane.presence[hole] == 0, std::string(name) + " inactive presence is not zero");
    requireCpuReference(lane.values[hole] == typename std::decay_t<decltype(lane.values)>::value_type{},
                        std::string(name) + " inactive value is not canonical default");
    for (std::size_t slot = 0; slot < slots; ++slot)
      if (slot != hole)
        requireCpuReference(lane.presence[slot] == 1, std::string(name) + " active scalar presence is not one");
  };
  check_scalar(timing.arcs.delays, "delays");
  check_scalar(timing.arcs.min_delays, "min_delays");
  check_scalar(timing.arcs.max_delays, "max_delays");
  const auto check_lanes = [&](const auto& lane, std::size_t width, const auto& absent, const char* name) {
    requireCpuReference(lane.presence.size() == slots * width, std::string(name) + " does not use arcSlotCount");
    for (std::size_t index = hole * width; index < (hole + 1) * width; ++index) {
      requireCpuReference(lane.presence[index] == 0, std::string(name) + " inactive presence is not zero");
      requireCpuReference(lane.values[index] == absent, std::string(name) + " inactive value is not canonical default");
    }
  };
  check_lanes(timing.arcs.transition_delays, kAnalysisCount * kTransitionCount, 0.0, "transition_delays");
  check_lanes(timing.arcs.input_output_delays, kAnalysisCount * kTransitionCount * kTransitionCount, 0.0, "input_output_delays");
  check_lanes(timing.arcs.graph_delays, kAnalysisCount * kTransitionCount * kTransitionCount, 0.0, "graph_delays");
  check_lanes(timing.arcs.transition_types, kTransitionCount, Transition::kNone, "transition_types");
  requireCpuReference(timing.arcs.timing_arc_row_offsets.size() == slots + 1, "timing-arc rows do not use arcSlotCount");
  requireCpuReference(timing.arcs.timing_arc_row_offsets[hole] == timing.arcs.timing_arc_row_offsets[hole + 1],
                      "inactive timing-arc row is not empty");
}

void runContracts()
{
  CsrCpuReferenceBackend backend;
  for (const CsrReferenceInput& input : {makeCpuReferenceNetFixture(), makeCpuReferenceCellFixture()}) {
    const CsrComputationResult legacy = backend.compute(input);
    FlatCsrEdgeView edges(input);
    FlatTimingPayloadView payloads(input);
    requireExactComputationResult(backend.compute(edges, payloads), legacy, "flat generic view versus legacy wrapper");
  }

  const CsrReferenceInput input = makeCpuReferenceNetFixture();
  FlatCsrEdgeView flat_edges(input);
  FlatTimingPayloadView flat_payloads(input);
  const CsrComputationWithTelemetry flat = backend.computeWithTelemetry(flat_edges, flat_payloads);
  checkVisitorContract(flat_edges);

  CsrReferenceInput malformed_endpoint = input;
  std::size_t outgoing_row = 0;
  while (outgoing_row + 1 < malformed_endpoint.out_row_offsets.size()
         && malformed_endpoint.out_row_offsets[outgoing_row]
                == malformed_endpoint.out_row_offsets[outgoing_row + 1])
    ++outgoing_row;
  requireCpuReference(outgoing_row < malformed_endpoint.nodes.size(),
                      "endpoint canary fixture has no outgoing row");
  const ArcId misplaced
      = malformed_endpoint.out_arc_ids[
          malformed_endpoint.out_row_offsets[outgoing_row]];
  malformed_endpoint.arcs[misplaced.value()].source
      = NodeId{malformed_endpoint.nodes.size()};
  FlatCsrEdgeView malformed_edges(malformed_endpoint);
  const EdgeVisitResult endpoint_error = malformed_edges.forEachOutgoing(
      NodeId{outgoing_row},
      [](const EdgeEntry&) { return EdgeVisitResult::continueVisit(); });
  requireCpuReference(endpoint_error.control == EdgeVisitControl::kError
                          && endpoint_error.error
                                 == CsrViewError::kMalformedAdjacency,
                      "flat view accepted an arc in the wrong source row");

  if constexpr (incregpusta_debug_checks_enabled) {
    requireCpuReference(flat.telemetry.levelization.base_in > 0 && flat.telemetry.levelization.base_out > 0,
                        "levelization did not traverse incoming and outgoing storage");
    requireCpuReference(flat.telemetry.arc_initialization.base_out > 0, "arc initialization did not traverse active arcs");
    requireCpuReference(flat.telemetry.forward_propagation.base_in > 0, "forward propagation did not traverse incoming arcs");
    requireCpuReference(flat.telemetry.required_propagation.base_out > 0, "required propagation did not traverse outgoing arcs");
  }

  InternalHoleView holes(input);
  constexpr std::size_t hole = 1;
  const auto inactive = holes.arc(ArcId{hole});
  requireCpuReference(!inactive && inactive.error() == CsrViewError::kInactiveArc, "inactive arc slot did not produce typed failure");
  const CsrComputationResult hole_computation = backend.compute(holes, flat_payloads);
  const auto* hole_timing = std::get_if<CsrTimingResult>(&hole_computation.outcome);
  requireCpuReference(hole_timing != nullptr, "stable-hole view did not compute timing");
  checkHoleLanes(*hole_timing, hole, holes.arcSlotCount());
}

}  // namespace
}  // namespace ista::csr::test

int main()
{
  try {
    ista::csr::test::runContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
