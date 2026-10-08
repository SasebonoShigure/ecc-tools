// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "CsrReferencePrimitives.hpp"
#include <vector>

namespace ista::csr {

// Native model order, independent of sink count.
inline constexpr int kCsrArnoldiOrder = 5;

struct CsrRcTreeNode
{
  std::size_t parent{0};
  double resistance_kohm{0.0};
  // Wire and pin capacitance, normalized and added exactly once.
  double capacitance_pf{0.0};
};

struct CsrArnoldiModel
{
  int order{0};
  double total_capacitance{0.0};
  double sqrt_total_capacitance{0.0};
  std::array<double, kCsrArnoldiOrder> diagonal{};
  std::array<double, kCsrArnoldiOrder> off_diagonal{};
  // Terminal-major; terminal zero is the driver. No sink count limit.
  std::vector<std::array<double, kCsrArnoldiOrder>> projection;
  friend bool operator==(const CsrArnoldiModel&, const CsrArnoldiModel&) = default;
};

struct CsrDmpLoadModel
{
  int order{0};
  std::array<double, 2> poles{};
  std::array<double, 2> residues{};
  friend bool operator==(const CsrDmpLoadModel&, const CsrDmpLoadModel&) = default;
};

struct CsrDmpModel
{
  bool valid{false};
  double driver_capacitance{0.0};
  double resistance{0.0};
  double load_capacitance{0.0};
  std::vector<CsrDmpLoadModel> loads;
  friend bool operator==(const CsrDmpModel&, const CsrDmpModel&) = default;
};

struct CsrRcFallbackLoad
{
  double delay{0.0};
  double impulse{0.0};
  bool valid{false};
  friend bool operator==(const CsrRcFallbackLoad&, const CsrRcFallbackLoad&) = default;
};

struct CsrParasiticModel
{
  CsrArnoldiModel arnoldi;
  CsrDmpModel dmp;
  // Native DFS subtree sum; ordinary NLDM ceil makes its summation order observable.
  double root_load{0.0};
  // Native raw-tree fallback, prepared once per affected net/lane.
  std::vector<CsrRcFallbackLoad> fallback;
  friend bool operator==(const CsrParasiticModel&, const CsrParasiticModel&) = default;
};

struct CsrParasiticTree
{
  std::string net_name;
  // Driver first, then terminals in native resistor-list DFS order.
  std::vector<NodeId> terminals;
  // Identical pin-load lanes share the immutable reduction.
  std::array<std::shared_ptr<const CsrParasiticModel>, kAnalysisCount * kTransitionCount> lanes;
};

struct CsrParasiticBinding
{
  std::shared_ptr<const CsrParasiticTree> tree;
  std::size_t terminal{0};
  // Expected-before is an owner/slot check, never a per-arc tree traversal.
  friend bool operator==(const CsrParasiticBinding&, const CsrParasiticBinding&) = default;
};

// One rooted tree in native DFS order: root parent=0/R=0, all other
// parents precede their children. Terminals are root then native DFS pins.
// This reduces only structure/load. Drive resistance, eigenvalues, poles,
// Ceff and waveforms must be evaluated by the target backend.
// Invalid input throws invalid_argument; Arnoldi overflow throws overflow_error.
// DMP overflow leaves only DMP invalid. Zero capacitance leaves both invalid.
[[nodiscard]] CsrParasiticModel buildCsrParasiticModel(
    std::span<const CsrRcTreeNode> tree, std::span<const std::size_t> terminals);

}  // namespace ista::csr
