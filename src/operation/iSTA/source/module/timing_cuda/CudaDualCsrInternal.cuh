#pragma once
#include <limits>
#include <set>

#include "CsrCudaMemory.cuh"
#include "CsrCudaPacking.hpp"
#include "CudaDualCsrSession.hpp"

namespace ista::csr::cuda_detail {
inline Index checkedAdd(Index a, Index b)
{
  if (b > std::numeric_limits<Index>::max() - a)
    throw std::length_error("CUDA targeted size overflow");
  return a + b;
}
inline Index checkedProduct(Index a, Index b)
{
  if (a && b > std::numeric_limits<Index>::max() / a)
    throw std::length_error("CUDA targeted capacity overflow");
  return a * b;
}
inline void demand(bool value, const char* message)
{
  if (!value)
    throw std::invalid_argument(message);
}
struct ArcBinding
{
  Index source{}, sink{};
  bool active{true};
  bool levelization{false};
  EdgeStorageKind storage_kind{EdgeStorageKind::kBaseOut};
};
struct SlotPatch
{
  Index row{}, slot{}, expected{absent}, replacement{absent};
};
struct PayloadPatch
{
  Index id{};
  Arc expected, replacement;
};
struct DeactivatePatch
{
  Index id{}, source{}, sink{};
};
struct AuxExpected
{
  Index old_offset{}, payload_offset{}, count{};
};
struct PackedPatch
{
  std::vector<SlotPatch> removes, changes, reverse_changes;
  std::vector<PayloadPatch> payloads;
  std::vector<DeactivatePatch> deactivations;
  std::vector<Arc> appends;
  std::vector<Child> children, expected_children;
  std::vector<double> tables, expected_tables;
  std::vector<AuxExpected> aux_expected;
  std::vector<Index> expected_aux_arcs;
};
struct DevicePatch
{
  const SlotPatch *removes{}, *changes{}, *reverse_changes{};
  Index remove_count{}, change_count{}, reverse_count{};
  const PayloadPatch* payloads{};
  Index payload_count{};
  const DeactivatePatch* deactivations{};
  Index deactivate_count{};
  const Child* expected_children{};
  const double* expected_tables{};
  const AuxExpected* aux_expected{};
  Index aux_expected_count{};
  const Index *expected_aux_arcs{}, *old_aux_arcs{};
};
void launchTargetedPatch(DeviceView view, DevicePatch patch, cudaStream_t stream);
}  // namespace ista::csr::cuda_detail

namespace ista::csr {
struct CudaDualCsrSession::Impl
{
  CudaDualCsrTelemetry telemetry;
  cuda_detail::Runtime memory{telemetry.cuda};
  cuda_detail::DeviceView view;
  cuda_detail::NodeState* seed_nodes{};
  cuda_detail::PathState* seed_paths{};
  cuda_detail::Index table_count{};
  std::vector<cuda_detail::Index> out_offsets, in_offsets, aux_lengths, aux_offsets, aux_mapping, roots, ends;
  std::vector<cuda_detail::ArcBinding> bindings;
  cuda_detail::ResultLayout layout;
  timing_edit::TimingEditSessionToken token;
  timing_edit::TransactionGeneration generation;
  timing_edit::AppendOnlyHighWater high_water;
  std::shared_ptr<const Impl> origin;
  mutable bool poisoned{false};
  std::optional<CsrComputationCode> code;
  std::optional<CsrTimingValues> values;

  Impl(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water);
  Impl(std::shared_ptr<const Impl> before, const CudaDualCsrPatch& patch, CudaLevelizationMode mode);
  static void validatePatch(const Impl& before, const CudaDualCsrPatch& patch);
  cuda_detail::PackedPatch prepare(const Impl& before, const CudaDualCsrPatch& patch);
  void allocate();
  void execute(CudaLevelizationMode mode = CudaLevelizationMode::kFull);
  std::optional<CsrFifoLevelization> reconstructMixed();
  void transferPatch(const Impl& before, const CudaDualCsrPatch& patch, const cuda_detail::PackedPatch& packed);
};
}  // namespace ista::csr
