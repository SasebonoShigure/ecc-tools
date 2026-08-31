// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDatabaseCandidate.hpp"

#include <stdexcept>
#include <utility>

#include "DataManager.hpp"
#include "Database.hpp"

namespace ista::timing_edit {

NativeDatabaseCandidate::NativeDatabaseCandidate(std::unique_ptr<Database> database) noexcept : _database(std::move(database)) {}

NativeDatabaseCandidate::NativeDatabaseCandidate(NativeDatabaseCandidate&&) noexcept = default;
NativeDatabaseCandidate& NativeDatabaseCandidate::operator=(NativeDatabaseCandidate&&) noexcept = default;
NativeDatabaseCandidate::~NativeDatabaseCandidate() = default;

NativeDatabaseCandidate NativeDatabaseCandidate::create(const Database& source)
{
  auto clone = std::make_unique<Database>(source);
  resetDerivedState(*clone);
  return NativeDatabaseCandidate(std::move(clone));
}

Database& NativeDatabaseCandidate::database()
{
  if (_database == nullptr) {
    throw std::logic_error("native Database candidate has already been installed");
  }
  return *_database;
}

const Database& NativeDatabaseCandidate::database() const
{
  if (_database == nullptr) {
    throw std::logic_error("native Database candidate has already been installed");
  }
  return *_database;
}

void NativeDatabaseCandidate::resetDerivedState(Database& database) noexcept
{
  database.get_arc_list().clear();
  database.get_outgoing_arc_list_map().clear();
  database.get_incoming_arc_list_map().clear();
  database.get_start_point_list().clear();
  database.get_end_point_list().clear();
  database.get_timing_order_list().clear();
  database.get_timing_point_map().clear();
  database.get_timing_path_group_list().clear();

  for (auto& [name, net] : database.get_net_map()) {
    static_cast<void>(name);
    net.get_driver_pin().clear();
    net.get_driver_pin_list().clear();
    net.get_load_pin_list().clear();
  }

  database.get_power_activity_map().clear();
  database.get_instance_power_map().clear();
  database.get_power_summary() = PowerSummary{};
  database.get_summary() = Summary{};
}

std::unique_ptr<Database> NativeDatabaseCandidate::exchangeActiveDatabase(std::unique_ptr<Database> replacement) noexcept
{
  return STADM.exchangeDatabase(std::move(replacement));
}

Database& NativeDatabaseCandidate::activeDatabaseForVerification()
{
  return STADM.getDatabase();
}

InstalledNativeDatabase NativeDatabaseCandidate::installForExecution(
    TimingEditSessionToken session_token,
    std::shared_ptr<NativeExecutionScopeState> execution_scope)
{
  if (_database == nullptr) {
    throw std::logic_error("cannot install an empty native Database candidate");
  }
  return InstalledNativeDatabase(exchangeActiveDatabase(std::move(_database)), session_token,
                                 std::move(execution_scope));
}

InstalledNativeDatabase::InstalledNativeDatabase(InstalledNativeDatabase&& other) noexcept
    : _rollback_database(std::move(other._rollback_database)),
      _execution_scope(std::move(other._execution_scope)),
      _execution_session_token(other._execution_session_token),
      _scope_attached(std::exchange(other._scope_attached, false)),
      _installed(std::exchange(other._installed, false))
{
}

Database& InstalledNativeDatabase::database()
{
  if (!active()) {
    throw std::logic_error("native Database installation is no longer active");
  }
  return STADM.getDatabase();
}

const Database& InstalledNativeDatabase::database() const
{
  if (!active()) {
    throw std::logic_error("native Database installation is no longer active");
  }
  return STADM.getDatabase();
}

}  // namespace ista::timing_edit
