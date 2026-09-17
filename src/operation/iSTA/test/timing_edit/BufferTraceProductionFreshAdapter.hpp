// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "FreshBaselineRunner.hpp"
#include "BufferTraceProductionFreshSupport.hpp"

namespace ista::timing_edit::oracle {
struct ProductionFreshSessionView;
}

namespace ista::buffer_trace {

class PendingVerifiedBatch;

enum class OneShotProcessState : std::uint8_t
{
  kUnused,
  kEntered,
  kFinished
};

struct SourceArtifactFunctionalRecord
{
  std::string role;
  std::string logical_name;
  std::string canonical_path;
  std::uint64_t size{0};
  std::string sha256;
  friend bool operator==(const SourceArtifactFunctionalRecord&,
                         const SourceArtifactFunctionalRecord&) = default;
};

struct SourcePinFunctionalRecord
{
  std::string name;
  std::string instance;
  std::string net;
  std::int32_t direction{-1};
  bool io{false};
  friend bool operator==(const SourcePinFunctionalRecord&,
                         const SourcePinFunctionalRecord&) = default;
};

struct SourceInstanceFunctionalRecord
{
  std::string name;
  std::string master;
  std::int32_t type{0};
  std::int32_t status{0};
  std::int32_t orient{0};
  std::int32_t x{0};
  std::int32_t y{0};
  std::vector<SourcePinFunctionalRecord> pins;
  friend bool operator==(const SourceInstanceFunctionalRecord&,
                         const SourceInstanceFunctionalRecord&) = default;
};

struct SourceWireSegmentFunctionalRecord
{
  std::string layer;
  bool new_layer{false};
  bool wire{false};
  bool via{false};
  bool rect{false};
  std::vector<std::array<std::int32_t, 3>> points;
  std::vector<std::string> vias;
  std::optional<std::array<std::int32_t, 4>> delta_rect;
  friend bool operator==(const SourceWireSegmentFunctionalRecord&,
                         const SourceWireSegmentFunctionalRecord&) = default;
};

struct SourceWireFunctionalRecord
{
  std::int32_t statement{0};
  std::string shield;
  std::vector<SourceWireSegmentFunctionalRecord> segments;
  friend bool operator==(const SourceWireFunctionalRecord&,
                         const SourceWireFunctionalRecord&) = default;
};

struct SourceNetFunctionalRecord
{
  std::string name;
  std::int32_t type{0};
  std::string driver;
  std::vector<std::string> loads;
  std::vector<SourceWireFunctionalRecord> wires;
  friend bool operator==(const SourceNetFunctionalRecord&,
                         const SourceNetFunctionalRecord&) = default;
};

struct SourceIdbFunctionalProjection
{
  bool present{false};
  std::string design_name;
  std::vector<SourceInstanceFunctionalRecord> instances;
  std::vector<SourcePinFunctionalRecord> io_pins;
  std::vector<SourceNetFunctionalRecord> nets;
  std::uint64_t connected_pin_count{0};
  std::uint64_t placed_instance_count{0};
  std::uint64_t fixed_instance_count{0};
  std::uint64_t unplaced_instance_count{0};
  std::uint64_t wire_count{0};
  std::uint64_t segment_count{0};
  std::uint64_t wire_point_count{0};
  std::uint64_t via_count{0};
  std::uint64_t wire_length{0};
  friend bool operator==(const SourceIdbFunctionalProjection&,
                         const SourceIdbFunctionalProjection&) = default;
};

struct SourcePhysicalFunctionalReceipt
{
  // readVerilog populates IDM's primary service slot.
  SourceIdbFunctionalProjection primary_idb;
  // addVerilog may expose this separate optional logical view.
  SourceIdbFunctionalProjection optional_netlist_view;
  bool spef_reader_present{false};
  friend bool operator==(const SourcePhysicalFunctionalReceipt&,
                         const SourcePhysicalFunctionalReceipt&) = default;
};

struct SourceIsolationFunctionalReceipt
{
  std::vector<SourceArtifactFunctionalRecord> artifacts_before;
  std::vector<SourceArtifactFunctionalRecord> artifacts_after;
  SourcePhysicalFunctionalReceipt physical_before;
  SourcePhysicalFunctionalReceipt physical_after;
};

// Test-executable-only adapter. It is the only Stage 4 class allowed to see
// the Stage 3 production loader/probe seams. Runtime libraries link only the
// FreshBaselineRunner interface above.
class BufferTraceProductionFreshAdapter final : public FreshBaselineRunner
{
 public:
  explicit BufferTraceProductionFreshAdapter(
      bool prove_first_empty_failure = false) noexcept
      : _prove_first_empty_failure(prove_first_empty_failure) {}
  ~BufferTraceProductionFreshAdapter() override = default;

  BufferTraceProductionFreshAdapter(
      const BufferTraceProductionFreshAdapter&) = delete;
  BufferTraceProductionFreshAdapter& operator=(
      const BufferTraceProductionFreshAdapter&) = delete;
  BufferTraceProductionFreshAdapter(BufferTraceProductionFreshAdapter&&) = delete;
  BufferTraceProductionFreshAdapter& operator=(
      BufferTraceProductionFreshAdapter&&) = delete;

  [[nodiscard]] ReplayReceipt withFreshBaseline(
      const ReplayProgram& program, BufferReplayWork work) override;

  [[nodiscard]] const SourceIsolationFunctionalReceipt&
  sourceIsolationReceipt() const;
  [[nodiscard]] static OneShotProcessState oneShotState() noexcept;
  [[nodiscard]] static std::uint64_t loaderInvocationCount() noexcept;
  [[nodiscard]] static std::uint64_t guardedWorkInvocationCount() noexcept;
  [[nodiscard]] const DerivedTimingFreshnessFunctionalReceipt&
  derivedTimingFreshnessReceipt() const;
  [[nodiscard]] bool firstEmptyGenerationZeroExact() const noexcept
  {
    return _first_empty_generation_zero_exact;
  }

 private:
  [[nodiscard]] ReplayReceipt withFreshGenerationBaseline(
      const ReplayProgram& empty_template, BufferReplayWork work) override;
  [[nodiscard]] ReplayReceipt runOnce(const ReplayProgram& program,
                                      BufferReplayWork work,
                                      bool generation_template);
  class Services;
  static void proveSealAndPublish(
      const timing_edit::oracle::ProductionFreshSessionView& view,
      timing_edit::TimingEditCoordinator& coordinator,
      PendingVerifiedBatch&& pending);

  std::optional<SourceIsolationFunctionalReceipt> _source_isolation;
  std::optional<DerivedTimingFreshnessFunctionalReceipt> _derived_freshness;
  bool _prove_first_empty_failure{false};
  bool _first_empty_generation_zero_exact{false};
};

}  // namespace ista::buffer_trace
