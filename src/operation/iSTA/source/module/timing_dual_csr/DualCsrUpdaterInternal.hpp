// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <memory>
#include "DualCsrArcGroupIndex.hpp"
#include "DualCsrDescriptors.hpp"

namespace ista::timing_dual_csr::detail {

// Preparation allocates new values and capacity. Apply/rollback only move,
// swap, or write reserved storage. Undo always captures the actual old values.
class PreparedDualCsrGraphPatch final
{
 public:
  PreparedDualCsrGraphPatch(DualCsrState& state, const DualCsrBatchDescriptor& descriptor);
  PreparedDualCsrGraphPatch(const PreparedDualCsrGraphPatch&) = delete;
  PreparedDualCsrGraphPatch& operator=(const PreparedDualCsrGraphPatch&) = delete;
  PreparedDualCsrGraphPatch(PreparedDualCsrGraphPatch&&) noexcept;
  PreparedDualCsrGraphPatch& operator=(PreparedDualCsrGraphPatch&&) = delete;
  ~PreparedDualCsrGraphPatch();
  void apply() noexcept;
  void rollback() noexcept;
  void commit() noexcept;

 private:
  struct Data;
  std::unique_ptr<Data> _data;
};

class PreparedDualCsrIndexPatch final
{
 public:
  PreparedDualCsrIndexPatch(const DualCsrState& graph, DualCsrArcGroupIndex& index,
                           const DualCsrBatchDescriptor& descriptor);
  PreparedDualCsrIndexPatch(const PreparedDualCsrIndexPatch&) = delete;
  PreparedDualCsrIndexPatch& operator=(const PreparedDualCsrIndexPatch&) = delete;
  PreparedDualCsrIndexPatch(PreparedDualCsrIndexPatch&&) noexcept;
  PreparedDualCsrIndexPatch& operator=(PreparedDualCsrIndexPatch&&) = delete;
  ~PreparedDualCsrIndexPatch();
  void apply() noexcept;
  void rollback() noexcept;
  void commit() noexcept;

 private:
  struct Data;
  std::unique_ptr<Data> _data;
};

}  // namespace ista::timing_dual_csr::detail
