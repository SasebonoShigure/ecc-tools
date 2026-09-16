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

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ista::csr {

inline constexpr const char* kCsrSchemaVersion = "1.0.0";
inline constexpr std::size_t kAnalysisCount = 2;
inline constexpr std::size_t kTransitionCount = 2;
inline constexpr std::size_t kPathSourceCount = 2;

template <typename Tag>
class StrongId
{
 public:
  constexpr StrongId() = default;
  explicit constexpr StrongId(std::uint64_t value) : _value(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const { return _value; }
  friend constexpr bool operator==(StrongId, StrongId) = default;
  friend constexpr bool operator<(StrongId lhs, StrongId rhs) { return lhs._value < rhs._value; }

 private:
  std::uint64_t _value{0};
};

using NodeId = StrongId<struct NodeIdTag>;
using ArcId = StrongId<struct ArcIdTag>;
using ClockTagId = StrongId<struct ClockTagIdTag>;

enum class NodeKind : std::uint8_t
{
  kPort,
  kInstancePin
};

enum class ArcKind : std::uint8_t
{
  kCell,
  kNet
};

enum class PinDirection : std::uint8_t
{
  kNone,
  kInput,
  kOutput,
  kInout
};

enum class AnalysisMode : std::uint8_t
{
  kMax,
  kMin
};

enum class Transition : std::uint8_t
{
  kRise,
  kFall,
  kNone
};

enum class PathSource : std::uint8_t
{
  kInput,
  kRegister
};

enum class StartSeedAvailability : std::uint8_t
{
  kMaterialized,
  kDeferredSequential
};

enum class TimingSense : std::uint8_t
{
  kNoneUsesPositiveFallback,
  kPositiveUnate,
  kNegativeUnate,
  kNonUnate
};

enum class TableVariable : std::uint8_t
{
  kNone,
  kInputTransition,
  kOutputCapacitance,
  kConstrainedTransition
};

enum class TimingCheckKind : std::uint8_t
{
  kNone,
  kSetup,
  kHold,
  kRecovery,
  kRemoval,
  kWidth,
  kPeriod
};

enum class CsrCaptureCode : std::uint8_t
{
  kMalformedTopology,
  kSemanticKeyCollision,
  kIllegalNumeric,
  kInvalidTableShape,
  kInvalidAnalysisContext,
  kActiveCycle
};

enum class CsrReferenceUnsupportedReason : std::uint8_t
{
  kParasiticNet,
  kSequentialInstance,
  kSequentialLaunch,
  kTimingCheck,
  kCrpr,
  kPropagatedClock,
  kClockTreeFanout,
  kClockGatingOrDivider,
  kConditionalTimingArc,
  kCaseAnalysis
};

enum class CsrComputationCode : std::uint8_t
{
  kCycle,
  kInvariantViolation,
  kNumericFailure
};

enum class CsrScatterCode : std::uint8_t
{
  kSuccess,
  kInvalidState,
  kSnapshotInvalidated,
  kSchemaMismatch,
  kSessionMismatch,
  kGenerationMismatch,
  kSourceIdentityMismatch,
  kShapeMismatch,
  kResultNumericInvalid,
  kIdMappingMismatch,
  kPredecessorMismatch
};

enum class CsrInvalidationReason : std::uint8_t
{
  kTopologyChanged,
  kClockConstraintChanged,
  kLibraryChanged,
  kCornerChanged,
  kParasiticChanged,
  kTimingInputChanged
};

enum class CsrSnapshotState : std::uint8_t
{
  kReady,
  kInvalidated,
  kReleased
};

enum class CapacityAction : std::uint8_t
{
  kInitialAllocate
};

using OptionalLaneMatrix = std::array<std::array<std::optional<double>, kTransitionCount>, kAnalysisCount>;
using ArcOutputLoadMatrix = std::array<std::array<double, kTransitionCount>, kAnalysisCount>;

struct NodeKey
{
  NodeKind kind{NodeKind::kPort};
  std::string full_name;
  std::string instance_name;
  std::string pin_name;

  friend bool operator==(const NodeKey&, const NodeKey&) = default;
};

struct TimingTableSignature
{
  bool present{false};
  TableVariable variable1{TableVariable::kNone};
  TableVariable variable2{TableVariable::kNone};

  friend bool operator==(const TimingTableSignature&, const TimingTableSignature&) = default;
};

struct LibraryArcSignature
{
  std::string library_name;
  std::int32_t arc_index{-1};
  TimingSense sense{TimingSense::kNoneUsesPositiveFallback};
  Transition trigger_transition{Transition::kNone};
  Transition check_transition{Transition::kNone};
  std::string sdf_condition;
  std::array<TimingTableSignature, kTransitionCount> delay_tables;
  std::array<TimingTableSignature, kTransitionCount> slew_tables;

  friend bool operator==(const LibraryArcSignature&, const LibraryArcSignature&) = default;
};

struct ArcKeyBase
{
  NodeKey source;
  NodeKey sink;
  ArcKind kind{ArcKind::kNet};
  std::string owner_name;
  std::string library_source_port;
  std::string library_sink_port;
  std::vector<LibraryArcSignature> library_arcs;

  friend bool operator==(const ArcKeyBase&, const ArcKeyBase&) = default;
};

struct ArcKey
{
  ArcKeyBase base;
  std::uint64_t parallel_ordinal{0};

  friend bool operator==(const ArcKey&, const ArcKey&) = default;
};

struct NodeIdLookupEntry
{
  NodeKey key;
  NodeId id;

  friend bool operator==(const NodeIdLookupEntry&, const NodeIdLookupEntry&) = default;
};

struct ArcIdLookupEntry
{
  ArcKey key;
  ArcId id;

  friend bool operator==(const ArcIdLookupEntry&, const ArcIdLookupEntry&) = default;
};

struct StaAnalysisContext
{
  std::string liberty_corner;
  std::string temperature;
  std::string rc_corner;

  friend bool operator==(const StaAnalysisContext&, const StaAnalysisContext&) = default;
};

struct CsrSnapshotIdentity
{
  std::string fingerprint_schema_version{kCsrSchemaVersion};
  std::uint64_t session_token{0};
  std::uint64_t graph_generation{0};
  std::string topology_fingerprint;
  std::string clock_constraint_fingerprint;
  std::string library_fingerprint;
  std::string corner_fingerprint;
  std::string parasitic_fingerprint;
  std::string timing_input_fingerprint;

  friend bool operator==(const CsrSnapshotIdentity&, const CsrSnapshotIdentity&) = default;
};

}  // namespace ista::csr
