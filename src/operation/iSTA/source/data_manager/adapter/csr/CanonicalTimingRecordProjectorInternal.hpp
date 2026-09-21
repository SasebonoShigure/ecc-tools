// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "CsrGraphTypes.hpp"

namespace ista {
class Arc;
class Database;
class Instance;
class Pin;
class TimingCellPort;
class TimingCellArc;
}

namespace ista::csr::detail {

// Bound to one session's immutable native Liberty. Graph and load edits do not
// invalidate these models; replacing the Database requires a new store.
class NativeLibraryModels final
{
 public:
  explicit NativeLibraryModels(const ista::Database& database) : _database(database) {}
  NativeLibraryModels(const NativeLibraryModels&) = delete;
  NativeLibraryModels& operator=(const NativeLibraryModels&) = delete;
  void requireDatabase(const ista::Database& database) const;
  [[nodiscard]] const ReferencedLibraryCellModel& cell(const std::string& name);
  [[nodiscard]] const CellTransferModel& transfer(const std::string& cell_name, const ista::TimingCellArc& arc);

 private:
  const ista::Database& _database;
  std::map<std::string, ReferencedLibraryCellModel> _cells;
  std::map<const ista::TimingCellArc*, const CellTransferModel*> _arcs;
};

struct CanonicalArcKeyContext
{
  const ista::Database& database;
  const std::vector<NodeRecord>& nodes;
  const std::map<std::string, NodeId>& node_ids_by_full_name;
  const std::unordered_map<std::string, std::string>& ordered_pin_owners;
};

struct CanonicalLoadPinFacts
{
  NodeKey key;
  PinDirection direction{PinDirection::kNone};
  bool is_port{false};
  std::optional<double> port_output_load;
  std::optional<CellPinCapacitanceModel> cell_pin_capacitance;

  friend bool operator==(const CanonicalLoadPinFacts&,
                         const CanonicalLoadPinFacts&) = default;
};

[[nodiscard]] LibraryPortModel projectCanonicalLibraryPort(const ista::TimingCellPort& native_port);
[[nodiscard]] ReferencedLibraryCellModel projectCanonicalReferencedLibraryCell(const ista::Database& database,
                                                                               const std::string& cell_name);
[[nodiscard]] CellPinCapacitanceModel projectCanonicalCellPinCapacitance(
    const ista::Database& database, NodeId node, const NodeKey& key);
void applyCanonicalInstanceLibraryRoles(
    const ReferencedLibraryCellModel& cell, InstanceRecord& instance);
void applyCanonicalNodeLibraryRoles(
    const ReferencedLibraryCellModel& cell, NodeRecord& node);
[[nodiscard]] NodeRecord projectCanonicalNodeRecord(const ista::Database& database, const std::string& map_name, const ista::Pin& pin,
                                                      const std::set<std::string>& starts, const std::set<std::string>& ends,
                                                      const std::unordered_map<std::string, std::string>& ordered_pin_owners);
[[nodiscard]] InstanceRecord projectCanonicalInstanceRecord(const ista::Database& database, const std::string& map_name,
                                                             const ista::Instance& instance, const std::map<std::string, NodeId>& node_ids_by_full_name);
[[nodiscard]] ArcKeyBase projectCanonicalArcKeyBase(const CanonicalArcKeyContext& context, const ista::Arc& arc);
[[nodiscard]] ArcEligibility projectCanonicalArcEligibility(const ArcRecord& arc, const NodeRecord& sink, bool sink_clock_reachable,
                                                             bool replayed_clock_arc);
[[nodiscard]] std::optional<ArcOutputLoadMatrix> projectCanonicalNoParasiticOutputLoad(
    const std::string& sink_net_name, const std::map<std::string, ArcOutputLoadMatrix>& net_loads);
[[nodiscard]] CellTransferModel projectCanonicalArcTransfer(const ista::Database& database, NativeLibraryModels& models, const ArcRecord& arc, const ista::Arc& native_arc);
[[nodiscard]] ArcOutputLoadMatrix projectCanonicalNoParasiticNetLoad(
    const ista::Database& database, const std::string& net_name,
    const std::map<std::string, CanonicalLoadPinFacts>& load_pin_facts);

}  // namespace ista::csr::detail
