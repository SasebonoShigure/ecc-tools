// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "PhysicalGreedySelection.hpp"
#include "CsrReferenceTypes.hpp"
#include <optional>
namespace idb { class IdbPin; }
namespace ista::physical_greedy {
using Bytes = std::vector<std::uint8_t>;
struct Buffer {
  timing_edit::LibraryCellRef reference;
  const csr::CellTransferModel* model;
  double capacitance;
  Bytes key;
};
struct DriverEstimate { double delay, resistance; };
double pinCapacitance(const csr::LibraryPortModel& pin);
std::vector<Buffer> buffers(const Database& database, csr::detail::NativeLibraryModels& models,
                            std::span<const std::string> names);
std::optional<DriverEstimate> estimate(const csr::CellTransferModel& model, double input_slew, double load);
struct Sink {
  timing_edit::StableNodeRef identity;
  std::array<double, 2> position_dbu;
  double slack, load, path_resistance;
};
struct Net {
  timing_edit::StableNetRef identity;
  std::array<double, 2> driver_dbu;
  double input_slew, driver_resistance;
  std::vector<Sink> sinks;
};
std::optional<PhysicalGreedyCandidate> score(const Net& net, std::span<const Buffer> choices,
    const timing_edit::CreationToken& token, std::int32_t dbu_per_um);
}  // namespace ista::physical_greedy

namespace ista::physical_greedy {
// All pointers borrow committed state under the same physical operation guard.
struct PreparedCsrCandidates {
  struct Member {
    const csr::NodeRecord* node;
    idb::IdbPin* pin;
    const timing_edit::StableNodeRef* identity;
    std::size_t timing_slot{};
    bool data_sink{};
  };
  struct DriverInput { const csr::CellTransferModel* transfer; std::size_t timing_slot; };
  struct Net {
    std::vector<Member> members;
    std::size_t driver;
    std::vector<DriverInput> inputs;
    const timing_edit::StableNetRef* identity;
  };
  std::vector<Net> nets;
  std::vector<csr::NodeId> node_ids;
};
}  // namespace ista::physical_greedy
namespace ista {
physical_greedy::PreparedCsrCandidates prepareCsrPhysicalBuffers(const Database& library,
    csr::detail::NativeLibraryModels& models, idb::IdbDesign& design, const timing_dual_csr::DualCsrState& graph,
    const timing_dual_csr::DualCsrArcGroupIndex& index, const timing_edit::TimingEditResolverState& resolver);
PhysicalGreedySelection selectCsrPhysicalBuffers(const Database& library, csr::detail::NativeLibraryModels& models,
    idb::IdbDesign& design, const timing_dual_csr::DualCsrState& graph,
    const physical_greedy::PreparedCsrCandidates& prepared, std::span<const csr::BufferCandidateTiming> timing,
    std::span<const std::string> buffer_cells, std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um);
}  // namespace ista
