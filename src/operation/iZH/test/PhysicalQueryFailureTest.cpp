// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "IncrementalStaSession.hpp"
#include "AllocationFailureSupport.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include <iostream>
#include <stdexcept>
#include "Utility.hpp"
#include "verilog_write.h"
#include "IdbDesign.h"
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
#include "IdbDesign.h"
#include "DataManager.hpp"
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
