// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <array>
#include <map>
#include <span>
#include <string>
#include <vector>
#include "PreparedPhysicalBatch.hpp"

namespace izh {
// Functional row occupancy for fixed existing cells and new one-row buffers.
// The IDB remains authoritative; this index only rules out illegal locations.
class PhysicalPlacementRows
{
  using Interval = std::array<std::int32_t, 2>;
  struct Row {
    std::string orientation;
    std::vector<Interval> blocked;
  };
 public:
  struct Geometry {
    std::array<std::int32_t, 4> bounds;
    std::int32_t site_width{}, row_height{};
    std::vector<std::pair<std::int32_t, std::string>> rows;
    std::vector<std::array<std::int32_t, 4>> obstacles;
    bool operator==(const Geometry&) const = default;
  };
  class Prepared {
   public:
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    Prepared(Prepared&&) noexcept = default;
    void commit() noexcept;
   private:
    std::vector<std::pair<Row*, Interval>> additions;
    friend class PhysicalPlacementRows;
    Prepared() = default;
  };
  explicit PhysicalPlacementRows(idb::IdbDesign& design);
  void requireBufferMaster(idb::IdbCellMaster& master) const;
  [[nodiscard]] Prepared prepare(const PreparedPhysicalBatch& batch);
  [[nodiscard]] Geometry geometry() const;
 private:
  std::map<std::int32_t, Row> _rows;
  std::array<std::int32_t, 4> _bounds{};
  std::int32_t _site_width{}, _row_height{};
  std::string _site_name;
};
}  // namespace izh
