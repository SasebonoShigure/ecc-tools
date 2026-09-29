// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include "NativeEditTransaction.hpp"

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <utility>
#include <type_traits>

#include "DataManager.hpp"
#include "Database.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTargetedEditInternal.hpp"

namespace ista::timing_edit {
namespace {
struct NetDerivedUndo
{
  Net* net;
  std::string driver;
  std::vector<std::string> drivers, loads;
  void swap() noexcept
  {
    net->get_driver_pin().swap(driver);
    net->get_driver_pin_list().swap(drivers);
    net->get_load_pin_list().swap(loads);
  }
};
struct PinDirectionUndo
{
  Pin* pin;
  PinDirection direction;
};
struct FullDerivedUndo
{
  std::vector<Arc> arcs;
  std::map<std::string, std::vector<std::size_t>> outgoing, incoming;
  std::vector<std::string> starts, ends, order;
  std::map<std::string, TimingPoint> points;
  std::vector<TimingPathGroup> paths;
  std::map<std::string, PowerActivity> power_activity;
  std::map<std::string, InstancePower> instance_power;
  PowerSummary power;
  TASummary timing;
  std::vector<NetDerivedUndo> nets;
  std::vector<PinDirectionUndo> pins;

  explicit FullDerivedUndo(Database& database)
  {
    nets.reserve(database.get_net_map().size());
    pins.reserve(database.get_pin_map().size());
    for (auto& [name, net] : database.get_net_map()) {
      static_cast<void>(name);
      nets.push_back({&net, {}, {}, {}});
    }
    for (auto& [name, pin] : database.get_pin_map()) {
      static_cast<void>(name);
      pins.push_back({&pin, pin.get_direction()});
    }
  }
  void swap(Database& database) noexcept
  {
    arcs.swap(database.get_arc_list());
    outgoing.swap(database.get_outgoing_arc_list_map());
    incoming.swap(database.get_incoming_arc_list_map());
    starts.swap(database.get_start_point_list());
    ends.swap(database.get_end_point_list());
    order.swap(database.get_timing_order_list());
    points.swap(database.get_timing_point_map());
    paths.swap(database.get_timing_path_group_list());
    power_activity.swap(database.get_power_activity_map());
    instance_power.swap(database.get_instance_power_map());
    power.get_group_power_map().swap(database.get_power_summary().get_group_power_map());
    std::swap(power.get_total_power_value(), database.get_power_summary().get_total_power_value());
    auto& current = database.get_summary().ta_summary;
    timing.worst_end_point.swap(current.worst_end_point);
    std::swap(timing.timing_path_num, current.timing_path_num);
    std::swap(timing.checked_end_point_num, current.checked_end_point_num);
    std::swap(timing.unconstrained_end_point_num, current.unconstrained_end_point_num);
    std::swap(timing.violating_end_point_num, current.violating_end_point_num);
    std::swap(timing.worst_slack, current.worst_slack);
    std::swap(timing.total_negative_slack, current.total_negative_slack);
    for (auto& net : nets)
      net.swap();
  }
  void restore(Database& database) noexcept
  {
    swap(database);
    for (const auto& saved : pins)
      saved.pin->set_direction(saved.direction);
  }
};
}  // namespace

struct NativeEditTransaction::Undo
{
  Database& database;
  detail::PreparedNativeBufferInsertions raw;
  std::optional<FullDerivedUndo> full;
  std::unique_ptr<detail::PreparedNativeTargetedMetadata> metadata;
  std::optional<detail::PreparedNativeGraphPatch> targeted;
  explicit Undo(Database& target) : database(target)
  {
    raw.database = &target;
  }
  Undo(Database& target, std::span<const BufferInsertionMutation> edits, TimingEditResolverView resolver, std::uint64_t first_instance_id)
      : database(target),
        raw(detail::prepareNativeBufferInsertions(target, edits, resolver, first_instance_id)),
        full(std::in_place, target)
  {
  }
};

NativeEditTransaction::NativeEditTransaction(std::unique_ptr<Undo> undo) noexcept : _undo(std::move(undo)) {}
NativeEditTransaction::NativeEditTransaction(NativeEditTransaction&& other) noexcept
    : _undo(std::move(other._undo)),
      _execution_scope(std::move(other._execution_scope)),
      _session_token(other._session_token), _attached(std::exchange(other._attached, false)),
      _applied(std::exchange(other._applied, false))
{
}
NativeEditTransaction::~NativeEditTransaction()
{
  if (!rollback())
    std::fputs("ERROR: native edit transaction cleanup failed; session poisoned.\n", stderr);
}
NativeEditTransaction NativeEditTransaction::prepareFull(const NativeExecutionAuthority& authority)
{
  if (!authority.active())
    throw std::invalid_argument("native full transaction received an expired authority");
  NativeEditTransaction transaction(std::make_unique<Undo>(STADM.getDatabase()));
  transaction.attach(authority);
  transaction._undo->full.emplace(transaction._undo->database);
  transaction.apply();
  return transaction;
}
NativeEditTransaction NativeEditTransaction::prepareFull(
    const NativeExecutionAuthority& authority, const PreparedBatch& batch, TimingEditResolverView resolver)
{
  if (!authority.active() || authority.sessionToken() != batch.sessionToken())
    throw std::invalid_argument("native edit transaction received a foreign or expired authority");
  if (!batch.baselineOwner())
    throw std::invalid_argument("native edit transaction received an empty prepared batch");
  if (batch.policyReceipt().disposition != EditDisposition::kSupported)
    throw std::invalid_argument("native edit transaction requires supported normalized edits");
  return prepareFull(authority, batch.normalizedEdits(), resolver, detail::nextNativeInstanceId(STADM.getDatabase()));
}
NativeEditTransaction NativeEditTransaction::prepareFull(
    const NativeExecutionAuthority& authority, std::span<const BufferInsertionMutation> edits,
    TimingEditResolverView resolver, std::uint64_t first_instance_id)
{
  if (!authority.active())
    throw std::invalid_argument("native full edit received an expired authority");
  auto undo = std::make_unique<Undo>(STADM.getDatabase(), edits, resolver, first_instance_id);
  NativeEditTransaction transaction(std::move(undo));
  transaction.attach(authority);
  transaction.apply();
  return transaction;
}
NativeEditTransaction NativeEditTransaction::beginTargeted(const NativeExecutionAuthority& authority)
{
  if (!authority.active())
    throw std::invalid_argument("targeted transaction received an expired authority");
  NativeEditTransaction transaction(std::make_unique<Undo>(STADM.getDatabase()));
  transaction.attach(authority);
  return transaction;
}
void NativeEditTransaction::applyMetadata(std::unique_ptr<detail::PreparedNativeTargetedMetadata> metadata) noexcept
{
  _undo->metadata = std::move(metadata);
  _undo->metadata->apply();
}
void NativeEditTransaction::applyTargeted(detail::PreparedNativeBufferInsertions&& raw,
                                        detail::PreparedNativeGraphPatch&& graph) noexcept
{
  static_assert(std::is_nothrow_move_assignable_v<detail::PreparedNativeBufferInsertions>);
  static_assert(std::is_nothrow_move_constructible_v<detail::PreparedNativeGraphPatch>);
  _undo->raw = std::move(raw);
  _undo->targeted.emplace(std::move(graph));
  apply();
}
void NativeEditTransaction::apply() noexcept
{
  if (_undo->full)
    _undo->full->swap(_undo->database);
  _undo->raw.apply();
  if (_undo->targeted)
    _undo->targeted->apply(_undo->database);
  _applied = true;
}
void NativeEditTransaction::undo() noexcept
{
  if (_undo->metadata)
    _undo->metadata->undo();
  if (_applied) {
    if (_undo->full)
      _undo->full->restore(_undo->database);
    if (_undo->targeted)
      _undo->targeted->undo(_undo->database);
    _undo->raw.undo();
    _applied = false;
  }
}
void NativeEditTransaction::discardUndo() noexcept
{
  if (_undo && _undo->metadata)
    _undo->metadata->commit();
  _undo.reset();
}
Database& NativeEditTransaction::database()
{
  if (!active() || !_undo)
    throw std::logic_error("native edit transaction is not active");
  return _undo->database;
}
const Database& NativeEditTransaction::database() const
{
  if (!active() || !_undo)
    throw std::logic_error("native edit transaction is not active");
  return _undo->database;
}
}  // namespace ista::timing_edit
