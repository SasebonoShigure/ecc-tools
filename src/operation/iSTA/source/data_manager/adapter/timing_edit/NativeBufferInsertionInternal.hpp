// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include "CsrReferenceNativeModels.hpp"
#include <span>
#include "Database.hpp"
#include "TimingEditResolver.hpp"
namespace ista {
class Pin;
}
namespace ista::timing_edit::detail {
void requireDataOnlyNet(const Database& database, const std::string& name);
void requireSupportedMovedSink(const Database& database, const Pin& pin);
void requireSupportedMovedSink(const csr::NodeRecord& pin, const TimingCell& cell);
[[nodiscard]] std::uint64_t nextNativeInstanceId(const Database& database);
// Prepared map nodes and replacement strings/vectors. Apply and undo only
// transfer ownership; all validation and allocation happen during preparation.
struct PreparedNativeBufferInsertions
{
  PreparedNativeBufferInsertions() = default;
  PreparedNativeBufferInsertions(const PreparedNativeBufferInsertions&) = delete;
  PreparedNativeBufferInsertions& operator=(const PreparedNativeBufferInsertions&) = delete;
  PreparedNativeBufferInsertions(PreparedNativeBufferInsertions&&) noexcept = default;
  PreparedNativeBufferInsertions& operator=(PreparedNativeBufferInsertions&&) noexcept = default;
  Database* database{};
  std::map<std::string, Instance> instances;
  std::map<std::string, Pin> pins;
  std::map<std::string, Net> nets;
  std::map<std::string, std::vector<std::string>> memberships;
  std::map<std::string, std::string> connections;
  std::vector<std::string> instance_keys, pin_keys, net_keys;
  std::map<std::string, ParasiticNet> parasitics;
  // Before apply: preallocated slots. Afterwards: actual old map nodes.
  std::vector<std::pair<std::string, decltype(parasitics)::node_type>> previous_parasitics;
  void prepareParasitics(std::map<std::string, ParasiticNet> replacements);

  void apply() noexcept;
  void undo() noexcept;
};
struct PreparedNetDriverLoad
{
  Net* net{};
  std::string driver;
  std::vector<std::string> drivers, loads;
  void swap() noexcept;
};

// A native graph patch owns replacements until apply, and actual old values
// afterwards. Appends/insertions are undone by their original length/keys.
struct PreparedNativeGraphPatch
{
  std::size_t arc_count_before{};
  std::map<std::size_t, Arc> replacements;
  std::vector<Arc> appends;
  std::map<std::string, std::vector<std::size_t>> outgoing, incoming;
  std::map<std::string, TimingPoint> points;
  std::vector<std::string> new_outgoing, new_incoming, new_points;
  std::vector<PreparedNetDriverLoad> net_caches;

  void apply(Database& database) noexcept;
  void undo(Database& database) noexcept;
};
[[nodiscard]] PreparedNativeBufferInsertions prepareNativeBufferInsertions(
    Database& database, std::span<const BufferInsertionMutation> edits,
    TimingEditResolverView resolver, std::uint64_t first_instance_id,
    std::map<std::string, ParasiticNet> prepared_rc = {});
// Appends one cell arc and one input net arc per edit, in edit order. Existing
// moved sinks keep their physical arc slot, including across an unqueried suffix.
[[nodiscard]] PreparedNativeGraphPatch prepareNativeBufferGraph(
    Database& database, PreparedNativeBufferInsertions& raw,
    std::span<const BufferInsertionMutation> edits, TimingEditResolverView resolver);
void applyNativeBufferInsertion(Database& database, const BufferInsertionMutation& edit, TimingEditResolverView resolver, std::uint64_t instance_id);
}  // namespace ista::timing_edit::detail
