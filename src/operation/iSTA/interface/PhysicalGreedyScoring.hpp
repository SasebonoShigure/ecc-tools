// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "PhysicalGreedySelection.hpp"
#include "CsrReferenceTypes.hpp"
#include <optional>
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
