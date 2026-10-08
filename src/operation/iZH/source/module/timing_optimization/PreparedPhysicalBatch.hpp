// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "IdbDesign.h"
#include "PreRouteRc.hpp"

namespace izh {
// Internal request: semantic names have been resolved while the production
// owner holds its single-operation guard. No arbitrary physical edit callback.
struct PhysicalBufferInsertion
{
  idb::IdbNet* source_net{};
  idb::IdbCellMaster* master{};
  std::vector<idb::IdbPin*> sinks;
  std::string instance_name, net_name, input_pin, output_pin;
  std::int32_t x{}, y{};
  idb::IdbOrient orientation{idb::IdbOrient::kNone};
};

// Owns actual old connectivity until the cross-layer owner publishes. Preparing
// may allocate; apply/rollback/commit do not. The owner excludes all other IDB
// mutation and queries for the lifetime of an applied batch.
class PreparedPhysicalBatch
{
 public:
  PreparedPhysicalBatch(idb::IdbDesign& design, std::span<const PhysicalBufferInsertion> edits);
  ~PreparedPhysicalBatch() noexcept;
  PreparedPhysicalBatch(const PreparedPhysicalBatch&) = delete;
  PreparedPhysicalBatch& operator=(const PreparedPhysicalBatch&) = delete;
  PreparedPhysicalBatch(PreparedPhysicalBatch&&) noexcept;
  PreparedPhysicalBatch& operator=(PreparedPhysicalBatch&&) = delete;

  // Build only affected replacement/new trees before mutating IDB.
  [[nodiscard]] std::map<std::string, ista::ParasiticNet> prepareRc(const PreRouteRcTechnology& technology) const;
  [[nodiscard]] bool apply() noexcept;
  void rollback() noexcept;
  void commit() noexcept;
  [[nodiscard]] bool applied() const noexcept { return _applied; }
  [[nodiscard]] const auto& instances() const noexcept { return _instances; }
  [[nodiscard]] const auto& nets() const noexcept { return _nets; }

 private:
  struct PinChange {
    idb::IdbPin* pin{};
    idb::IdbNet* before{};
    idb::IdbNet* after{};
    std::string name;  // replacement before apply; actual previous name after
  };
  static void swapReferences(idb::IdbNet& left, idb::IdbNet& right) noexcept;

  idb::IdbDesign* _design;
  bool _applied{false}, _finished{false};
  std::size_t _instance_count{}, _net_count{};
  std::uint64_t _instance_id{}, _net_id{};
  std::vector<std::unique_ptr<idb::IdbInstance>> _instances;
  std::vector<std::unique_ptr<idb::IdbNet>> _nets;
  std::vector<std::pair<idb::IdbNet*, std::unique_ptr<idb::IdbNet>>> _references;
  std::vector<PinChange> _pins;
  std::unordered_map<std::string, idb::IdbInstance*> _instance_entries;
  std::unordered_map<std::string, idb::IdbNet*> _net_entries;
};
}  // namespace izh
