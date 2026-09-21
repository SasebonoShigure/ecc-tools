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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TimingEditTypes.hpp"
#include "CsrReferencePrimitives.hpp"

namespace ista::buffer_trace {

inline constexpr std::uint32_t kBufferFixedTraceSchemaVersion = 2;
inline constexpr auto kBufferFixedTraceSchema = "buffer-fixed-trace/2.0.0";

enum class SelectionMode : std::uint8_t
{
  kFixedTrace
};

enum class SemanticWorkloadKind : std::uint8_t
{
  kSynthetic,
  kRealDesign
};

enum class ProfileId : std::uint8_t
{
  kPaperProtocolTop1000X1,
  kPaperProtocolTop100X10,
  kEngineeringSmoke
};

enum class ComparabilityStatus : std::uint8_t
{
  kExact,
  kProtocolOnly,
  kNonComparable
};

enum class ComparabilityReason : std::uint8_t
{
  kEngineeringRcNoPhysical
};

enum class AnalysisMode : std::uint8_t
{
  kCpuFull
};

[[nodiscard]] std::string_view token(SelectionMode value);
[[nodiscard]] std::string_view token(SemanticWorkloadKind value);
[[nodiscard]] std::string_view token(ProfileId value);
[[nodiscard]] std::string_view token(ComparabilityStatus value);
[[nodiscard]] std::string_view token(ComparabilityReason value);
[[nodiscard]] std::string_view token(AnalysisMode value);

struct OperatingPoint
{
  double input_slew_ns{0.0};
  double output_load_pf{0.0};

  friend bool operator==(const OperatingPoint&, const OperatingPoint&) = default;
};

struct EngineeringRcProfile
{
  std::string id;
  double upstream_resistance_kohm{0.0};
  double buffer_resistance_kohm{0.0};

  friend bool operator==(const EngineeringRcProfile&, const EngineeringRcProfile&) = default;
};

struct SelectionProfile
{
  ProfileId id{ProfileId::kEngineeringSmoke};
  std::uint64_t top_k{0};
  std::uint64_t rounds{0};
  std::uint64_t max_total_insertions{0};
  AnalysisMode analysis{AnalysisMode::kCpuFull};
  OperatingPoint operating_point;
  EngineeringRcProfile rc_profile;

  friend bool operator==(const SelectionProfile&, const SelectionProfile&) = default;
};

[[nodiscard]] SelectionProfile frozenSelectionProfile(ProfileId id);

struct ReplayHeader
{
  std::uint32_t schema_version{kBufferFixedTraceSchemaVersion};
  SelectionMode selection_mode{SelectionMode::kFixedTrace};
  SemanticWorkloadKind workload_kind{SemanticWorkloadKind::kSynthetic};
  ProfileId profile_id{ProfileId::kEngineeringSmoke};
  ComparabilityStatus comparability_status{ComparabilityStatus::kNonComparable};
  std::optional<ComparabilityReason> comparability_reason{ComparabilityReason::kEngineeringRcNoPhysical};

  friend bool operator==(const ReplayHeader&, const ReplayHeader&) = default;
};

// These are intentionally distinct types: a source role cannot be silently
// substituted for another role at the C++ boundary.
struct TechLefRecipe
{
  std::string logical_name;
  std::string source_text;
  friend bool operator==(const TechLefRecipe&, const TechLefRecipe&) = default;
};
struct CellLefRecipe
{
  std::string logical_name;
  std::string source_text;
  friend bool operator==(const CellLefRecipe&, const CellLefRecipe&) = default;
};
struct LibertyRecipe
{
  std::string logical_name;
  std::string source_text;
  friend bool operator==(const LibertyRecipe&, const LibertyRecipe&) = default;
};
struct VerilogRecipe
{
  std::string logical_name;
  std::string source_text;
  friend bool operator==(const VerilogRecipe&, const VerilogRecipe&) = default;
};
struct SdcRecipe
{
  std::string logical_name;
  std::string source_text;
  friend bool operator==(const SdcRecipe&, const SdcRecipe&) = default;
};

struct SyntheticArtifactRecipe
{
  TechLefRecipe tech_lef;
  CellLefRecipe cell_lef;
  LibertyRecipe liberty;
  VerilogRecipe verilog;
  SdcRecipe sdc;
  std::string top;

  friend bool operator==(const SyntheticArtifactRecipe&, const SyntheticArtifactRecipe&) = default;
};

struct TechLefSourceRef
{
  std::string path;
  friend bool operator==(const TechLefSourceRef&, const TechLefSourceRef&) = default;
};
struct CellLefSourceRef
{
  std::string path;
  friend bool operator==(const CellLefSourceRef&, const CellLefSourceRef&) = default;
};
struct LibertySourceRef
{
  std::string path;
  friend bool operator==(const LibertySourceRef&, const LibertySourceRef&) = default;
};
struct VerilogSourceRef
{
  std::string path;
  friend bool operator==(const VerilogSourceRef&, const VerilogSourceRef&) = default;
};
struct SdcSourceRef
{
  std::string path;
  friend bool operator==(const SdcSourceRef&, const SdcSourceRef&) = default;
};

struct SourceArtifactRefs
{
  TechLefSourceRef tech_lef;
  std::vector<CellLefSourceRef> cell_lefs;
  std::vector<LibertySourceRef> liberties;
  VerilogSourceRef verilog;
  SdcSourceRef sdc;
  std::string top;
  csr::StaAnalysisContext analysis_context;

  friend bool operator==(const SourceArtifactRefs&, const SourceArtifactRefs&) = default;
};

struct SyntheticReplayPayload
{
  SyntheticArtifactRecipe source_recipe;
  timing_edit::BaselineSemanticIdentity baseline;
  std::vector<timing_edit::EditBatch> batches;

  friend bool operator==(const SyntheticReplayPayload&, const SyntheticReplayPayload&) = default;
};

struct RealReplayPayload
{
  SourceArtifactRefs artifacts;
  timing_edit::BaselineSemanticIdentity baseline;
  std::vector<timing_edit::EditBatch> batches;

  friend bool operator==(const RealReplayPayload&, const RealReplayPayload&) = default;
};

using ReplayPayload = std::variant<SyntheticReplayPayload, RealReplayPayload>;

struct ReplayProgram
{
  ReplayHeader header;
  ReplayPayload workload;

  [[nodiscard]] const timing_edit::BaselineSemanticIdentity& baseline() const
  {
    return std::visit([](const auto& payload) -> const timing_edit::BaselineSemanticIdentity& { return payload.baseline; }, workload);
  }

  friend bool operator==(const ReplayProgram&, const ReplayProgram&) = default;
};

struct MaterializedSetupTiming
{
  double arrival_ns{0.0};
  double required_ns{0.0};
  double slack_ns{0.0};

  friend bool operator==(const MaterializedSetupTiming&, const MaterializedSetupTiming&) = default;
};

struct BufferCandidateObservation
{
  timing_edit::StableNetRef net;
  timing_edit::StableNodeRef driver;
  timing_edit::StableNodeRef candidate_pin;
  std::vector<timing_edit::StableNodeRef> moved_sinks;
  std::optional<MaterializedSetupTiming> setup_timing;
  double upstream_resistance_kohm{0.0};
  double candidate_load_pf{0.0};
  double buffer_capacitance_pf{0.0};
  double buffer_resistance_kohm{0.0};
  double buffer_delay_ns{0.0};
  timing_edit::LibraryCellRef buffer;
  timing_edit::PlacementInputs placement;
  timing_edit::RcInputs rc;

  friend bool operator==(const BufferCandidateObservation&, const BufferCandidateObservation&) = default;
};

struct GainComputationReceipt
{
  double arrival_ns{0.0};
  double required_ns{0.0};
  double slack_ns{0.0};
  double upstream_resistance_kohm{0.0};
  double candidate_load_pf{0.0};
  double buffer_capacitance_pf{0.0};
  double delta_capacitance_pf{0.0};
  double benefit_ns{0.0};
  double buffer_resistance_kohm{0.0};
  double penalty_ns{0.0};
  double benefit_minus_penalty_ns{0.0};
  double buffer_delay_ns{0.0};
  double gain_ns{0.0};

  friend bool operator==(const GainComputationReceipt&, const GainComputationReceipt&) = default;
};

enum class CandidateDecision : std::uint8_t
{
  kAccepted,
  kNonCriticalNet,
  kLowerRankedCandidate,
  kRejected
};

enum class CandidateRejectionReason : std::uint8_t
{
  kInvalidSemanticReference,
  kInvalidMovedSinkSet,
  kInvalidBufferCell,
  kInvalidEnvironmentInputs,
  kTimingNotMaterialized,
  kNonFiniteTiming,
  kSlackMismatch,
  kNegativeOperand,
  kNonFiniteIntermediate,
  kNonPositiveGain
};

[[nodiscard]] std::string_view token(CandidateDecision value);
[[nodiscard]] std::string_view token(CandidateRejectionReason value);

struct CandidateEvaluationReceipt
{
  timing_edit::StableNetRef net;
  timing_edit::StableNodeRef driver;
  timing_edit::StableNodeRef candidate_pin;
  timing_edit::LibraryCellRef buffer;
  std::optional<GainComputationReceipt> numeric;
  CandidateDecision decision{CandidateDecision::kRejected};
  std::optional<CandidateRejectionReason> rejection_reason;
  std::optional<timing_edit::CreationToken> accepted_creation_token;

  friend bool operator==(const CandidateEvaluationReceipt&, const CandidateEvaluationReceipt&) = default;
};

struct GenerationRoundReceipt
{
  std::uint64_t batch_ordinal{0};
  std::vector<CandidateEvaluationReceipt> evaluations;
  std::vector<timing_edit::CreationToken> accepted_tokens;

  friend bool operator==(const GenerationRoundReceipt&, const GenerationRoundReceipt&) = default;
};

enum class GenerationStopReason : std::uint8_t
{
  kEmptyAcceptedBatch,
  kRoundsReached,
  kMaxTotalInsertionsReached
};

[[nodiscard]] std::string_view token(GenerationStopReason value);

struct GenerationReceipt
{
  SelectionProfile profile;
  std::vector<GenerationRoundReceipt> rounds;
  GenerationStopReason stop_reason{GenerationStopReason::kEmptyAcceptedBatch};

  friend bool operator==(const GenerationReceipt&, const GenerationReceipt&) = default;
};

struct BufferTraceArtifact
{
  ReplayProgram replay;
  GenerationReceipt generation_only;

  friend bool operator==(const BufferTraceArtifact&, const BufferTraceArtifact&) = default;
};

enum class BufferTraceContractCode : std::uint8_t
{
  kSchemaMismatch,
  kInvalidValue,
  kInvalidTaggedUnion,
  kInvalidProfile,
  kInvalidSourceArtifacts,
  kGenerationMismatch,
  kIdentityMismatch,
  kForwardReference,
  kDuplicate,
  kNotCanonical
};

[[nodiscard]] std::string_view token(BufferTraceContractCode value);

struct BufferTraceContractViolation
{
  BufferTraceContractCode code{BufferTraceContractCode::kInvalidValue};
  std::string field;
  std::string message;

  friend bool operator==(const BufferTraceContractViolation&, const BufferTraceContractViolation&) = default;
};

using BufferTraceContractViolations = std::vector<BufferTraceContractViolation>;

[[nodiscard]] BufferTraceContractViolations validateSelectionProfile(const SelectionProfile& profile);

// A generator template validates the complete replay-safe header and tagged
// source payload before entering a loader, but intentionally has no edits yet.
[[nodiscard]] BufferTraceContractViolations validateReplayTemplate(const ReplayProgram& program);
[[nodiscard]] BufferTraceContractViolations validateReplayProgram(const ReplayProgram& program);
[[nodiscard]] BufferTraceContractViolations validateBufferTraceArtifact(const BufferTraceArtifact& artifact);

}  // namespace ista::buffer_trace
