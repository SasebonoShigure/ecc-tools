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
#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "CsrReferencePrimitives.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr { class DualCsrStructuredBatchPreparer; }

namespace ista::timing_edit {

inline constexpr std::uint32_t kCreationTokenSchemaVersion = 1;
inline constexpr auto kDesignEditPlanSchema = "timing-edit-plan/1.0.0";
inline constexpr auto kTimingGraphDeltaSchema = "timing-graph-delta/1.0.0";

template <typename Tag>
class StrongOrdinal
{
 public:
  constexpr StrongOrdinal() = default;
  explicit constexpr StrongOrdinal(std::uint64_t value) : _value(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const { return _value; }
  friend constexpr auto operator<=>(StrongOrdinal, StrongOrdinal) = default;

 private:
  std::uint64_t _value{0};
};

using TransactionGeneration = StrongOrdinal<struct TransactionGenerationTag>;
using TimingEditSessionToken = StrongOrdinal<struct TimingEditSessionTokenTag>;
using AppendOnlySlot = StrongOrdinal<struct AppendOnlySlotTag>;

enum class CreatedNodeRole : std::uint8_t
{
  kInput,
  kOutput
};

enum class CreatedNetRole : std::uint8_t
{
  kOutputNet
};

enum class ParasiticEditPolicy : std::uint8_t
{
  kError,
  kFullWithRecord,
  kPreRouteTree
};

enum class EditDisposition : std::uint8_t
{
  kSupported,
  kFullWithRecord
};

enum class DeltaInvalidationKind : std::uint8_t
{
  kLoad,
  kParasitic,
  kArcDelay
};

[[nodiscard]] std::string_view token(CreatedNodeRole value);
[[nodiscard]] std::string_view token(CreatedNetRole value);
[[nodiscard]] std::string_view token(ParasiticEditPolicy value);
[[nodiscard]] std::string_view token(EditDisposition value);
[[nodiscard]] std::string_view token(DeltaInvalidationKind value);

struct CreationToken
{
  std::uint32_t schema_version{kCreationTokenSchemaVersion};
  std::string case_id;
  std::string profile_id;
  std::uint64_t seed{0};
  std::uint64_t batch_ordinal{0};
  std::uint64_t accepted_ordinal{0};

  friend auto operator<=>(const CreationToken&, const CreationToken&) = default;
};

struct ExistingNodeRef
{
  csr::NodeKey baseline_key;

  friend bool operator==(const ExistingNodeRef&, const ExistingNodeRef&) = default;
  friend bool operator<(const ExistingNodeRef& lhs, const ExistingNodeRef& rhs) { return csr::nodeKeyLess(lhs.baseline_key, rhs.baseline_key); }
};

struct CreatedNodeRef
{
  CreationToken creation_token;
  CreatedNodeRole role{CreatedNodeRole::kInput};

  friend auto operator<=>(const CreatedNodeRef&, const CreatedNodeRef&) = default;
};

using StableNodeRef = std::variant<ExistingNodeRef, CreatedNodeRef>;

struct ExistingNetRef
{
  std::string baseline_name;

  friend auto operator<=>(const ExistingNetRef&, const ExistingNetRef&) = default;
};

struct CreatedNetRef
{
  CreationToken creation_token;
  CreatedNetRole role{CreatedNetRole::kOutputNet};

  friend auto operator<=>(const CreatedNetRef&, const CreatedNetRef&) = default;
};

using StableNetRef = std::variant<ExistingNetRef, CreatedNetRef>;

struct ExistingArcRef
{
  csr::ArcKey baseline_key;

  friend bool operator==(const ExistingArcRef&, const ExistingArcRef&) = default;
  friend bool operator<(const ExistingArcRef& lhs, const ExistingArcRef& rhs) { return csr::arcKeyLess(lhs.baseline_key, rhs.baseline_key); }
};

struct CreatedCellArcRef
{
  CreationToken creation_token;
  std::string library_source_port;
  std::string library_sink_port;
  std::vector<csr::LibraryArcSignature> ordered_library_arcs;
  std::uint64_t stable_equivalence_ordinal{0};

  friend bool operator==(const CreatedCellArcRef&, const CreatedCellArcRef&) = default;
  friend bool operator<(const CreatedCellArcRef& lhs, const CreatedCellArcRef& rhs);
};

struct CreatedOldNetArcRef
{
  StableNetRef net;
  StableNodeRef driver;
  StableNodeRef sink;
  std::uint64_t stable_ordinal{0};

  friend bool operator==(const CreatedOldNetArcRef&, const CreatedOldNetArcRef&) = default;
  friend bool operator<(const CreatedOldNetArcRef& lhs, const CreatedOldNetArcRef& rhs);
};

struct CreatedNewNetArcRef
{
  CreationToken creation_token;
  StableNodeRef driver;
  StableNodeRef sink;
  std::uint64_t stable_ordinal{0};

  friend bool operator==(const CreatedNewNetArcRef&, const CreatedNewNetArcRef&) = default;
  friend bool operator<(const CreatedNewNetArcRef& lhs, const CreatedNewNetArcRef& rhs);
};

using StableArcRef = std::variant<ExistingArcRef, CreatedCellArcRef, CreatedOldNetArcRef, CreatedNewNetArcRef>;

struct ComponentFingerprints
{
  std::string schema_version{csr::kCsrSchemaVersion};
  std::string topology;
  std::string clock_constraint;
  std::string library;
  std::string corner;
  std::string parasitic;
  std::string timing_input;

  friend bool operator==(const ComponentFingerprints&, const ComponentFingerprints&) = default;
};

// Exact semantic records complement hashes. Lists are required to be sorted in
// canonical byte order and duplicate-free.
struct CanonicalPreStateEvidence
{
  std::vector<std::string> instances;
  std::vector<std::string> nets;
  std::vector<csr::NodeKey> nodes;
  std::vector<csr::ArcKey> arcs;

  friend bool operator==(const CanonicalPreStateEvidence&, const CanonicalPreStateEvidence&) = default;
};

struct BaselineSemanticIdentity
{
  std::string design_name;
  ComponentFingerprints fingerprints;
  CanonicalPreStateEvidence canonical_pre_state;

  friend bool operator==(const BaselineSemanticIdentity&, const BaselineSemanticIdentity&) = default;
};

struct CommittedEditIdentity
{
  TransactionGeneration generation;
  ComponentFingerprints current_fingerprints;
  CanonicalPreStateEvidence canonical_pre_state;

  friend bool operator==(const CommittedEditIdentity&, const CommittedEditIdentity&) = default;
};

struct LibraryCellRef
{
  std::string library_name;
  std::string cell_name;
  std::string input_port;
  std::string output_port;
  std::vector<csr::LibraryArcSignature> ordered_arc_signatures;

  friend bool operator==(const LibraryCellRef&, const LibraryCellRef&) = default;
  friend bool operator<(const LibraryCellRef& lhs, const LibraryCellRef& rhs);
};

struct UnplacedInputs
{
  friend bool operator==(const UnplacedInputs&, const UnplacedInputs&) = default;
};

struct PlacedInputs
{
  double x_microns{0.0};
  double y_microns{0.0};

  friend bool operator==(const PlacedInputs&, const PlacedInputs&) = default;
};

using PlacementInputs = std::variant<UnplacedInputs, PlacedInputs>;

struct NoParasiticRcInputs
{
  friend bool operator==(const NoParasiticRcInputs&, const NoParasiticRcInputs&) = default;
};

struct ParasiticRcInputs
{
  ParasiticEditPolicy parasitic_policy{ParasiticEditPolicy::kError};

  friend bool operator==(const ParasiticRcInputs&, const ParasiticRcInputs&) = default;
};

using RcInputs = std::variant<NoParasiticRcInputs, ParasiticRcInputs>;

struct BufferInsertionMutation
{
  StableNetRef original_net;
  std::vector<StableNodeRef> moved_sinks;
  LibraryCellRef library_cell;
  PlacementInputs placement_inputs;
  RcInputs rc_inputs;
  CreationToken creation_token;

  friend bool operator==(const BufferInsertionMutation&, const BufferInsertionMutation&) = default;
};

// Per-generation semantic edits. Session baseline ownership stays with the
// session/coordinator rather than being repeated in every prepared batch.
struct EditBatch
{
  TransactionGeneration expected_generation;
  std::vector<BufferInsertionMutation> edits;

  friend bool operator==(const EditBatch&, const EditBatch&) = default;
};

struct DesignEditPlan
{
  std::string schema_version{kDesignEditPlanSchema};
  BaselineSemanticIdentity baseline;
  TransactionGeneration expected_generation;
  std::vector<BufferInsertionMutation> edits;

  friend bool operator==(const DesignEditPlan&, const DesignEditPlan&) = default;
};

struct AddInstance
{
  CreationToken creation_token;
  LibraryCellRef library_cell;
  PlacementInputs placement_inputs;
  friend bool operator==(const AddInstance&, const AddInstance&) = default;
};

struct RemoveInstance
{
  CreationToken creation_token;
  friend bool operator==(const RemoveInstance&, const RemoveInstance&) = default;
};

struct AddTimingPoint
{
  CreatedNodeRef node;
  csr::PinDirection direction{csr::PinDirection::kNone};
  friend bool operator==(const AddTimingPoint&, const AddTimingPoint&) = default;
};

struct RemoveTimingPoint
{
  CreatedNodeRef node;
  friend bool operator==(const RemoveTimingPoint&, const RemoveTimingPoint&) = default;
};

struct AddCellArc
{
  CreatedCellArcRef arc;
  friend bool operator==(const AddCellArc&, const AddCellArc&) = default;
};

struct RemoveCellArc
{
  CreatedCellArcRef arc;
  friend bool operator==(const RemoveCellArc&, const RemoveCellArc&) = default;
};

struct AddNetArc
{
  StableArcRef arc;
  friend bool operator==(const AddNetArc&, const AddNetArc&) = default;
};

struct RemoveNetArc
{
  StableArcRef arc;
  friend bool operator==(const RemoveNetArc&, const RemoveNetArc&) = default;
};

struct ReconnectSink
{
  StableNodeRef sink;
  StableNetRef old_net;
  StableNetRef new_net;
  friend bool operator==(const ReconnectSink&, const ReconnectSink&) = default;
};

struct InvalidateLoad
{
  StableNetRef net;
  friend bool operator==(const InvalidateLoad&, const InvalidateLoad&) = default;
};

struct InvalidateParasitic
{
  StableNetRef net;
  friend bool operator==(const InvalidateParasitic&, const InvalidateParasitic&) = default;
};

struct InvalidateArcDelay
{
  StableArcRef arc;
  friend bool operator==(const InvalidateArcDelay&, const InvalidateArcDelay&) = default;
};

using TimingGraphEdit = std::variant<AddInstance, RemoveInstance, AddTimingPoint, RemoveTimingPoint, AddCellArc, RemoveCellArc, AddNetArc,
                                     RemoveNetArc, ReconnectSink, InvalidateLoad, InvalidateParasitic, InvalidateArcDelay>;

struct TimingGraphDelta
{
  std::string schema_version{kTimingGraphDeltaSchema};
  std::vector<TimingGraphEdit> operations;

  friend bool operator==(const TimingGraphDelta&, const TimingGraphDelta&) = default;
};

struct AppendOnlyHighWater
{
  std::uint64_t node_slots{0};
  std::uint64_t net_slots{0};
  std::uint64_t arc_slots{0};

  friend bool operator==(const AppendOnlyHighWater&, const AppendOnlyHighWater&) = default;
};

template <typename StableRef>
struct SlotAssignment
{
  StableRef ref;
  AppendOnlySlot slot;
  bool tombstone{false};

  friend bool operator==(const SlotAssignment&, const SlotAssignment&) = default;
};

struct AppendOnlySlotPlan
{
  AppendOnlyHighWater before;
  AppendOnlyHighWater after;
  std::vector<SlotAssignment<StableNodeRef>> nodes;
  std::vector<SlotAssignment<StableNetRef>> nets;
  std::vector<SlotAssignment<StableArcRef>> arcs;

  friend bool operator==(const AppendOnlySlotPlan&, const AppendOnlySlotPlan&) = default;
};

struct TokenMaterialization
{
  CreationToken creation_token;
  std::string instance_name;
  std::string output_net_name;
  std::string input_pin_name;
  std::string output_pin_name;

  friend bool operator==(const TokenMaterialization&, const TokenMaterialization&) = default;
};

struct InvalidationClosure
{
  std::vector<StableNetRef> load_nets;
  std::vector<StableNetRef> parasitic_nets;
  std::vector<StableArcRef> arc_delays;

  friend bool operator==(const InvalidationClosure&, const InvalidationClosure&) = default;
};

struct PolicyReceipt
{
  EditDisposition disposition{EditDisposition::kSupported};
  std::vector<std::string> reasons;

  friend bool operator==(const PolicyReceipt&, const PolicyReceipt&) = default;
};

struct PreparedBatchData
{
  TimingEditSessionToken session_token;
  TransactionGeneration expected_generation;
  TransactionGeneration target_generation;
  std::shared_ptr<const BaselineSemanticIdentity> baseline;
  std::optional<CommittedEditIdentity> expected_pre_committed_identity;
  std::vector<BufferInsertionMutation> normalized_edits;
  TimingGraphDelta normalized_forward;
  TimingGraphDelta normalized_reverse;
  AppendOnlySlotPlan slot_plan;
  std::vector<TokenMaterialization> token_materializations;
  InvalidationClosure invalidation_closure;
  PolicyReceipt policy_receipt;
};


class PreparedBatch
{
 public:
  PreparedBatch(const PreparedBatch&) = delete;
  PreparedBatch& operator=(const PreparedBatch&) = delete;
  PreparedBatch(PreparedBatch&&) noexcept = default;
  PreparedBatch& operator=(PreparedBatch&&) noexcept = default;

  [[nodiscard]] static PreparedBatch create(PreparedBatchData data);

  [[nodiscard]] TimingEditSessionToken sessionToken() const { return _data.session_token; }
  [[nodiscard]] TransactionGeneration expectedGeneration() const { return _data.expected_generation; }
  [[nodiscard]] TransactionGeneration targetGeneration() const { return _data.target_generation; }
  [[nodiscard]] const BaselineSemanticIdentity& baseline() const { return *_data.baseline; }
  [[nodiscard]] const std::shared_ptr<const BaselineSemanticIdentity>& baselineOwner() const { return _data.baseline; }
  [[nodiscard]] const std::optional<CommittedEditIdentity>& expectedPreCommittedIdentity() const { return _data.expected_pre_committed_identity; }
  [[nodiscard]] const std::vector<BufferInsertionMutation>& normalizedEdits() const { return _data.normalized_edits; }
  [[nodiscard]] const TimingGraphDelta& forwardDelta() const { return _data.normalized_forward; }
  [[nodiscard]] const TimingGraphDelta& reverseDelta() const { return _data.normalized_reverse; }
  [[nodiscard]] const AppendOnlySlotPlan& slotPlan() const { return _data.slot_plan; }
  [[nodiscard]] const std::vector<TokenMaterialization>& tokenMaterializations() const { return _data.token_materializations; }
  [[nodiscard]] const InvalidationClosure& invalidationClosure() const { return _data.invalidation_closure; }
  [[nodiscard]] const PolicyReceipt& policyReceipt() const { return _data.policy_receipt; }

 private:
  explicit PreparedBatch(PreparedBatchData data) : _data(std::move(data)) {}

  PreparedBatchData _data;

  friend class NativeTimingEditPreparer;
  friend class ista::timing_dual_csr::DualCsrStructuredBatchPreparer;
};

}  // namespace ista::timing_edit
