// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <any>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CsrSnapshotObservation.hpp"
#include "STAInterface.hpp"
#include "StaProductionLifecycle.hpp"
#include "py_ista_snapshot.h"
#include "py_register_ista.h"

namespace py = pybind11;

PYBIND11_EMBEDDED_MODULE(ista_step7_binding_fixture, module)
{
  python_interface::register_ista(module);
}

namespace {

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

class FakeProductionOperations final : public ista::StaProductionLifecycleOperations
{
 public:
  ista::StaOperationResult initialize(std::map<std::string, std::any>&) override
  {
    ++initialize_calls;
    return ista::StaOperationResult::success();
  }

  ista::StaOperationResult runFullBaseline() override
  {
    ++full_calls;
    return ista::StaOperationResult::success();
  }

  ista::StaOperationResult extractLib() override { return ista::StaOperationResult::success(); }
  ista::StaOperationResult updateTiming() override { return ista::StaOperationResult::success(); }
  ista::StaOperationResult writeSdf(const std::string&) override { return ista::StaOperationResult::success(); }
  ista::StaOperationResult reportTiming() override { return ista::StaOperationResult::success(); }

  ista::StaOperationResult close() override
  {
    ++close_calls;
    return close_result;
  }

  int initialize_calls{0};
  int full_calls{0};
  int close_calls{0};
  ista::StaOperationResult close_result = ista::StaOperationResult::success();
};

py::dict request(std::string_view strategy)
{
  py::dict result;
  result["backend"] = "cpu";
  result["update_strategy"] = strategy;
  result["fallback_policy"] = "error";
  return result;
}

py::dict analysisContext()
{
  py::dict result;
  result["liberty_corner"] = "slow";
  result["temperature"] = "25";
  result["rc_corner"] = "__no_rc_corner__";
  return result;
}

void requireExactKeys(const py::dict& value, std::initializer_list<std::string_view> expected, const std::string& name)
{
  std::set<std::string> actual;
  for (auto item : value)
    actual.insert(py::cast<std::string>(item.first));
  std::set<std::string> exact;
  for (std::string_view key : expected)
    exact.emplace(key);
  require(actual == exact, name + " keys drifted");
}

template <typename Callable>
void requirePythonError(Callable&& callable, PyObject* expected, const std::string& name)
{
  try {
    std::forward<Callable>(callable)();
  } catch (const py::error_already_set& error) {
    require(error.matches(expected), name + " raised the wrong Python exception");
    return;
  }
  throw std::runtime_error(name + " did not raise");
}

std::uint64_t requireToken(const py::object& session, const std::string& name)
{
  py::object token = session.attr("session_token");
  require(PyLong_CheckExact(token.ptr()) && !PyBool_Check(token.ptr()), name + " token is not an exact Python int");
  const std::uint64_t value = py::cast<std::uint64_t>(token);
  require(value != 0, name + " token is zero");
  return value;
}

void requireObservationShape(const py::dict& observation, std::uint64_t token, std::string_view state)
{
  requireExactKeys(
      observation,
      {"schema_version", "state", "identity", "counts", "buffers", "capacity", "support", "timing_ms", "host_bytes", "last_rejection", "last_invalidation"},
      "observation");
  require(py::cast<std::string>(observation["schema_version"]) == "1.0.0", "observation schema drifted");
  require(py::cast<std::string>(observation["state"]) == state, "observation state drifted");

  const py::dict identity = py::cast<py::dict>(observation["identity"]);
  requireExactKeys(identity,
                   {"fingerprint_schema_version", "session_token", "graph_generation", "topology_fingerprint", "clock_constraint_fingerprint",
                    "library_fingerprint", "corner_fingerprint", "parasitic_fingerprint", "timing_input_fingerprint"},
                   "observation identity");
  require(py::cast<std::uint64_t>(identity["session_token"]) == token, "observation identity token drifted");
  require(py::cast<std::uint64_t>(identity["graph_generation"]) == 0, "observation generation drifted");

  requireExactKeys(py::cast<py::dict>(observation["counts"]), {"nodes", "arcs", "levels"}, "observation counts");
  requireExactKeys(py::cast<py::dict>(observation["buffers"]),
                   {"node_keys", "arc_keys", "out_row_offsets", "out_arc_ids", "in_row_offsets", "in_arc_ids", "level_offsets", "level_nodes", "node_to_level"},
                   "observation buffers");
  requireExactKeys(py::cast<py::dict>(observation["capacity"]), {"last_action", "event_count"}, "observation capacity");
  requireExactKeys(py::cast<py::dict>(observation["support"]), {"reference_supported", "unsupported_reasons"}, "observation support");
  const py::dict timing = py::cast<py::dict>(observation["timing_ms"]);
  requireExactKeys(timing, {"export", "reference", "scatter"}, "observation timing");
  const double export_ms = py::cast<double>(timing["export"]);
  require(std::isfinite(export_ms) && export_ms >= 0.0, "observation export duration is invalid");
  requireExactKeys(py::cast<py::dict>(observation["host_bytes"]), {"logical", "capacity"}, "observation host bytes");
}

void requireNoRawProperties(const py::object& session)
{
  for (const char* name : {"input", "result", "reference", "scatter", "raw"})
    require(!py::hasattr(session, name), std::string("binding exposed forbidden property: ") + name);
}

void testFactoryAndFull(py::module_& module, FakeProductionOperations& operations)
{
  py::object session = module.attr("create_incremental_sta_session")(request("full"));
  require(session.attr("session_token").is_none(), "factory token was not None");
  require(session.attr("csr_snapshot_observation").is_none(), "factory observation was not None");
  requireNoRawProperties(session);

  py::dict invalid_context;
  invalid_context["liberty_corner"] = "slow";
  invalid_context["temperature"] = "25";
  requirePythonError([&] { session.attr("initialize")(invalid_context); }, PyExc_ValueError, "invalid analysis context");
  require(py::cast<std::string>(session.attr("state")) == "created", "invalid mapping changed the session state");
  require(session.attr("session_token").is_none(), "invalid mapping consumed a session token");

  session.attr("initialize")(analysisContext());
  const std::uint64_t token = requireToken(session, "full session");
  require(session.attr("csr_snapshot_observation").is_none(), "initialized full session exposed an observation");
  session.attr("run_full_baseline")();
  require(session.attr("csr_snapshot_observation").is_none(), "executed full session exposed an observation");
  session.attr("close")();
  require(session.attr("csr_snapshot_observation").is_none(), "closed full session exposed an observation");
  require(requireToken(session, "closed full session") == token, "full session token changed after close");
  require(operations.initialize_calls == 1 && operations.full_calls == 1 && operations.close_calls == 1, "full binding operation counts drifted");
}

void testIncrementalReadyReleased(py::module_& module)
{
  py::object session = module.attr("create_incremental_sta_session")(request("incremental"));
  session.attr("initialize")(analysisContext());
  const std::uint64_t token = requireToken(session, "incremental session");
  session.attr("run_full_baseline")();

  py::dict active = py::cast<py::dict>(session.attr("csr_snapshot_observation"));
  requireObservationShape(active, token, "ready");
  active["state"] = "mutated-detached-value";
  py::dict stable_active = py::cast<py::dict>(session.attr("csr_snapshot_observation"));
  requireObservationShape(stable_active, token, "ready");

  session.attr("close")();
  py::dict released = py::cast<py::dict>(session.attr("csr_snapshot_observation"));
  requireObservationShape(released, token, "released");
  py::dict normalized_released = py::dict(released);
  normalized_released["state"] = "ready";
  require(normalized_released.equal(stable_active), "release changed fields other than state");

  released["state"] = "mutated-detached-value";
  py::dict stable_released = py::cast<py::dict>(session.attr("csr_snapshot_observation"));
  requireObservationShape(stable_released, token, "released");
  requireNoRawProperties(session);
}

void testCleanupFailureKeepsReleased(py::module_& module, FakeProductionOperations& operations)
{
  py::object session = module.attr("create_incremental_sta_session")(request("incremental"));
  session.attr("initialize")(analysisContext());
  const std::uint64_t token = requireToken(session, "cleanup failure session");
  session.attr("run_full_baseline")();

  const int close_calls_before = operations.close_calls;
  operations.close_result = ista::StaOperationResult::cleanupFailure("injected cleanup failure");
  requirePythonError([&] { session.attr("close")(); }, module.attr("StaSessionError").ptr(), "cleanup failure close");
  require(py::cast<std::string>(session.attr("state")) == "closed", "cleanup failure did not close the session");
  requireObservationShape(py::cast<py::dict>(session.attr("csr_snapshot_observation")), token, "released");
  session.attr("close")();
  require(operations.close_calls == close_calls_before + 1, "cleanup failure caused a second native close");
}

}  // namespace

int main()
{
  try {
    py::scoped_interpreter interpreter;
    py::module_ module = py::module_::import("ista_step7_binding_fixture");
    FakeProductionOperations operations;
    ista::STAInterface::destroyInst();
    ista::STAInterface::getInst().setLifecycleOperationsForTesting(&operations);
    testFactoryAndFull(module, operations);
    testIncrementalReadyReleased(module);
    testCleanupFailureKeepsReleased(module, operations);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
