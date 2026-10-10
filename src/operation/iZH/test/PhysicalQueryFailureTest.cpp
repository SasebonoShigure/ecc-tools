// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "IncrementalStaSession.hpp"
#include "StaCsrSessionContext.hpp"
#include "CsrSnapshotObservation.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "BenchmarkTimingConsumer.hpp"
#include "PreRouteRcTestSupport.hpp"
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include <iostream>
#include <stdexcept>
#include "Utility.hpp"
#include "verilog_write.h"
#include "IdbDesign.h"
#include "DataManager.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include <filesystem>

namespace ista {
void physicalOutputFailureRetry(idb::IdbDesign& design)
{
  bool failed = false;
  try {
    auto file = Utility::openOutputFile("/dev/full");
    file << "current timing";
    file.close();
  } catch (const std::ios_base::failure&) { failed = true; }
  if (!failed) throw std::runtime_error("report close failure was accepted");
  std::set<std::string> excluded;
  failed = false;
  try {
    idb::VerilogWriter writer("/dev/full", excluded, design, false);
    writer.writeModule();
  } catch (const std::runtime_error&) { failed = true; }
  if (!failed) throw std::runtime_error("netlist write/close failure was accepted");
  const auto path = std::filesystem::temp_directory_path() / "stage13-netlist-retry.v";
  idb::VerilogWriter writer(path.c_str(), excluded, design, false);
  writer.writeModule();
  if (!std::filesystem::file_size(path)) throw std::runtime_error("netlist output retry is empty");
  std::filesystem::remove(path);
}
void physicalColdViewRetry(IncrementalStaSession& session)
{
  if constexpr (!incregpusta_debug_checks_enabled) return;
  auto& database = STADM.getDatabase();
  const auto old_graph = csr::exportCsrGraphSnapshot(database);
  const auto old_timing = csr::test::readNativeTimingOracle(database);
  const auto old_reports = TimingReporterProbeAccess::read();
  const auto raw_values = [&] {
    nlohmann::json result;
    for (const auto& [name, net] : database.get_parasitic_library().get_net_map())
      result["rc"][name] = izh::test::rcValues(net);
    for (const auto& [name, pin] : database.get_pin_map())
      result["pins"][name] = {pin.get_pin_name(), pin.get_full_name(), pin.get_instance_name(),
          pin.get_net_name(), pin.get_direction(), pin.get_x(), pin.get_y(), pin.get_is_port()};
    for (const auto& [name, net] : database.get_net_map())
      result["nets"][name] = {net.get_net_name(), net.get_driver_pin(), net.get_driver_pin_list(),
          net.get_load_pin_list(), net.get_pin_name_list()};
    const auto power = [](const PowerValue& value) {
      return std::array{value.get_internal_power(), value.get_switching_power(), value.get_leakage_power()};
    };
    result["power_total"] = power(database.get_power_summary().get_total_power_value());
    for (const auto& [group, value] : database.get_power_summary().get_group_power_map())
      result["power_groups"].push_back({group, power(value)});
    for (auto& [name, value] : database.get_instance_power_map())
      result["instance_power"][name] = {value.get_instance_id(), value.get_instance_name(),
          value.get_power_group_type(), value.get_voltage(), power(value.get_power_value())};
    for (const auto& [name, value] : database.get_power_activity_map())
      result["activity"][name] = {value.get_rise_transition_density(), value.get_fall_transition_density(),
          value.get_static_probability(), value.get_origin(), value.get_is_valid()};
    return result;
  };
  const auto old_raw = raw_values();
  bool snapshot_rejected = false;
  try { snapshot_rejected = !session.snapshotObservation(); }
  catch (const StaSessionException&) { snapshot_rejected = true; }
  const auto reference = session.computeCsrReferenceForTesting();
  const auto scatter = session.scatterCsrTimingResultForTesting({});
  if (!snapshot_rejected || !std::holds_alternative<StaCsrSessionOperationFailure>(reference.value)
      || scatter.value.scatter_result.code != csr::CsrScatterCode::kInvalidState
      || csr::exportCsrGraphSnapshot(database) != old_graph)
    throw std::runtime_error("legacy snapshot diagnostics bypassed physical generation ownership");
  struct Failure {
    Database& database;
    std::size_t old_instances;
    bool during_graph, saw_empty{false}, fired{false};
  };
  for (bool during_graph : {false, true}) {
    Failure failure{database, database.get_instance_map().size(), during_graph};
    ista_test_allocation::armFailureWhen([](void* opaque) noexcept {
      auto& state = *static_cast<Failure*>(opaque);
      state.saw_empty |= state.database.get_arc_list().empty();
      state.fired = state.saw_empty && (state.during_graph ? !state.database.get_arc_list().empty()
          : state.database.get_instance_map().size() > state.old_instances);
      return state.fired;
    }, &failure);
    bool rejected = false;
    try { static_cast<void>(session.readPhysicalResult()); }
    catch (const std::exception&) { rejected = true; }
    catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
    static_cast<void>(ista_test_allocation::disable());
    if (!failure.fired || !rejected || session.state() != StaSessionState::kBaselineExecuted
        || csr::exportCsrGraphSnapshot(database) != old_graph
        || csr::test::readNativeTimingOracle(database) != old_timing
        || TimingReporterProbeAccess::read() != old_reports || raw_values() != old_raw)
      throw std::runtime_error("cold native view allocation failure lost raw/derived state or query retry eligibility");
  }
}
void physicalQueryAllocationRetry(IncrementalStaSession& session)
{
  const auto expected = session.readPhysicalResult();  // Warm materialization/report caches.
  const auto summary = session.readPhysicalResult(true);
  if (summary.generation != expected.generation || summary.qor_json != expected.qor_json
      || !summary.timing.empty() || !summary.paths_json.empty())
    throw std::runtime_error("physical QOR-only query changed results or returned unrequested details");
  if constexpr (!incregpusta_debug_checks_enabled) return;
  ista_test_allocation::armCountOnly();
  try { static_cast<void>(session.readPhysicalResult()); }
  catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
  const auto count = ista_test_allocation::disable();
  if (!count) throw std::runtime_error("physical result fixture has no report allocations");
  ista_test_allocation::armCountOnly();
  try { static_cast<void>(session.readPhysicalResult(true)); }
  catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
  const auto summary_count = ista_test_allocation::disable();
  if (summary_count >= count)
    throw std::runtime_error("QOR-only query did not remove unrequested result allocations");
  std::cout << "physical query allocations: summary=" << summary_count << ", full=" << count << '\n';
  for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
    bool failed = false;
    ista_test_allocation::armFailure(ordinal);
    try { static_cast<void>(session.readPhysicalResult()); }
    catch (const std::exception&) { failed = true; }
    catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
    static_cast<void>(ista_test_allocation::disable());
    if (!failed || session.state() != StaSessionState::kBaselineExecuted)
      throw std::runtime_error("physical report allocation failure returned partial success or poisoned the session");
    const auto retried = session.readPhysicalResult();
    if (retried.generation != expected.generation || retried.timing != expected.timing
        || retried.qor_json != expected.qor_json || retried.paths_json != expected.paths_json)
      throw std::runtime_error("physical report allocation retry changed current results");
  }
  std::cout << "physical public result allocation failures/retries: " << count << '\n';
}
}  // namespace ista

#if ECC_INCREGPUSTA_DEBUG_CHECKS
#include "StaTimingEditSessionProbeAccess.hpp"
namespace ista {
void physicalDiagnosticCanary(IncrementalStaSession& session, idb::IdbDesign& design)
{
  const auto before = StaTimingEditSessionProbeAccess::physicalDiagnostic(session);
  auto* pin = design.get_io_pin_list()->get_pin_list().front();
  auto* net = pin->get_net();
  if (!net) throw std::runtime_error("physical diagnostic canary has no connected pin");
  pin->set_net(nullptr); // Leave the forward net list intact: only reverse binding is wrong.
  bool rejected = false;
  try { (void) StaTimingEditSessionProbeAccess::physicalDiagnostic(session); }
  catch (const std::runtime_error&) { rejected = true; }
  pin->set_net(net);
  if (!rejected || StaTimingEditSessionProbeAccess::physicalDiagnostic(session) != before)
    throw std::runtime_error("physical diagnostic missed reverse connectivity corruption or changed committed data");
  auto& database = STADM.getDatabase();
  const auto& arc = database.get_arc_list().front();
  if (arc.get_is_disable_arc() || arc.get_is_loop_disable())
    throw std::runtime_error("level canary requires an active edge");
  auto& sink = database.get_timing_point_map().at(arc.get_sink_pin());
  const auto previous_level = sink.get_level();
  sink.set_level(database.get_timing_point_map().at(arc.get_source_pin()).get_level());
  rejected = false;
  try { (void) StaTimingEditSessionProbeAccess::physicalDiagnostic(session); }
  catch (const std::runtime_error&) { rejected = true; }
  sink.set_level(previous_level);
  if (!rejected || StaTimingEditSessionProbeAccess::physicalDiagnostic(session) != before)
    throw std::runtime_error("physical diagnostic missed a materialized level violation");
}
}
#endif
