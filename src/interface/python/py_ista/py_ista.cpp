// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "py_ista.h"

#include <pybind11/stl.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "CsrSnapshotObservation.hpp"
#include "STAInterface.hpp"
#include "TimingSemanticKey.hpp"
#include "operation/iSTA/source/module/timing_csr/CsrReferenceValidator.hpp"
#include "py_ista_snapshot.h"

namespace python_interface {
bool initStaConfigMapByJSON(const std::string& config, std::map<std::string, std::any>& config_map);
void initStaConfigMapByDict(std::map<std::string, std::string>& config_dict, std::map<std::string, std::any>& config_map);
}  // namespace python_interface

namespace python_interface {
namespace py = pybind11;
namespace {

const std::string& requireRequestField(const std::map<std::string, std::string>& request, const char* field)
{
  auto found = request.find(field);
  if (found == request.end())
    throw std::invalid_argument(std::string("missing STA request field: ") + field);
  return found->second;
}

py::dict executionRecordToDict(const ista::StaExecutionRecord& record)
{
  py::dict result;
  result["requested_backend"] = ista::toString(record.requested_backend);
  result["requested_update_strategy"] = ista::toString(record.requested_update_strategy);
  result["fallback_policy"] = ista::toString(record.fallback_policy);
  result["executed_backend"] = ista::toString(record.executed_backend);
  result["executed_update_strategy"] = ista::toString(record.executed_update_strategy);
  result["execution_kind"] = ista::toString(record.execution_kind);
  result["propagation_scope"] = ista::toString(record.propagation_scope);
  result["incremental_batch_executed"] = record.incremental_batch_executed;
  result["fallback_to_full"] = record.fallback_to_full;
  result["fallback_reason"] = record.fallback_reason ? py::cast(ista::toString(*record.fallback_reason)) : py::none();
  return result;
}

std::map<std::string, std::any> parseStaConfig(const std::string& config, std::map<std::string, std::string>& config_dict)
{
  std::map<std::string, std::any> config_map;
  if (!config.empty() && !initStaConfigMapByJSON(config, config_map))
    throw std::invalid_argument("invalid STA JSON configuration");
  initStaConfigMapByDict(config_dict, config_map);
  return config_map;
}

ista::csr::StaAnalysisContext parseStaAnalysisContext(py::handle mapping)
{
  if (!mapping || !PyDict_CheckExact(mapping.ptr()))
    throw std::invalid_argument("analysis_context must be an exact dict");
  if (PyDict_Size(mapping.ptr()) != 3)
    throw std::invalid_argument("analysis_context must contain exactly liberty_corner, temperature, and rc_corner");

  ista::csr::StaAnalysisContext context;
  for (auto item : py::reinterpret_borrow<py::dict>(mapping)) {
    if (!PyUnicode_CheckExact(item.first.ptr()))
      throw std::invalid_argument("analysis_context keys must be exact str values");
    if (!PyUnicode_CheckExact(item.second.ptr()))
      throw std::invalid_argument("analysis_context values must be exact str values");

    const std::string key = py::cast<std::string>(item.first);
    const std::string value = py::cast<std::string>(item.second);
    if (key == "liberty_corner")
      context.liberty_corner = value;
    else if (key == "temperature")
      context.temperature = value;
    else if (key == "rc_corner")
      context.rc_corner = value;
    else
      throw std::invalid_argument("analysis_context contains an unknown field: " + key);
  }

  const auto violations = ista::csr::validateAnalysisContextIntrinsic(context);
  if (!violations.empty())
    throw std::invalid_argument(violations.front().field + ": " + violations.front().message);
  return context;
}

py::object optionalString(const std::optional<std::string>& value)
{
  return value ? py::cast(*value) : py::none();
}

py::object optionalDouble(const std::optional<double>& value)
{
  return value ? py::cast(*value) : py::none();
}

py::dict snapshotIdentityToDict(const ista::csr::CsrSnapshotIdentity& identity)
{
  py::dict result;
  result["fingerprint_schema_version"] = identity.fingerprint_schema_version;
  result["session_token"] = identity.session_token;
  result["graph_generation"] = identity.graph_generation;
  result["topology_fingerprint"] = identity.topology_fingerprint;
  result["clock_constraint_fingerprint"] = identity.clock_constraint_fingerprint;
  result["library_fingerprint"] = identity.library_fingerprint;
  result["corner_fingerprint"] = identity.corner_fingerprint;
  result["parasitic_fingerprint"] = identity.parasitic_fingerprint;
  result["timing_input_fingerprint"] = identity.timing_input_fingerprint;
  return result;
}

py::dict currentFingerprintsToDict(const ista::csr::CsrCurrentFingerprints& fingerprints)
{
  py::dict result;
  result["topology_fingerprint"] = optionalString(fingerprints.topology_fingerprint);
  result["clock_constraint_fingerprint"] = optionalString(fingerprints.clock_constraint_fingerprint);
  result["library_fingerprint"] = optionalString(fingerprints.library_fingerprint);
  result["corner_fingerprint"] = optionalString(fingerprints.corner_fingerprint);
  result["parasitic_fingerprint"] = optionalString(fingerprints.parasitic_fingerprint);
  result["timing_input_fingerprint"] = optionalString(fingerprints.timing_input_fingerprint);
  return result;
}

py::dict bufferExtentToDict(const ista::csr::CsrBufferExtent& extent)
{
  py::dict result;
  result["logical_size"] = extent.logical_size;
  result["capacity"] = extent.capacity;
  return result;
}

py::dict snapshotBuffersToDict(const ista::csr::CsrSnapshotBufferMetrics& buffers)
{
  py::dict result;
  result["node_keys"] = bufferExtentToDict(buffers.node_keys);
  result["arc_keys"] = bufferExtentToDict(buffers.arc_keys);
  result["out_row_offsets"] = bufferExtentToDict(buffers.out_row_offsets);
  result["out_arc_ids"] = bufferExtentToDict(buffers.out_arc_ids);
  result["in_row_offsets"] = bufferExtentToDict(buffers.in_row_offsets);
  result["in_arc_ids"] = bufferExtentToDict(buffers.in_arc_ids);
  result["level_offsets"] = bufferExtentToDict(buffers.level_offsets);
  result["level_nodes"] = bufferExtentToDict(buffers.level_nodes);
  result["node_to_level"] = bufferExtentToDict(buffers.node_to_level);
  return result;
}

py::object scatterRejectionToObject(const std::optional<ista::csr::CsrScatterRejectionEvent>& event)
{
  if (!event)
    return py::none();

  py::dict result;
  result["reason"] = std::string(ista::csr::token(event->reason));
  result["writes_applied"] = event->writes_applied;
  if (event->source)
    result["source"] = snapshotIdentityToDict(*event->source);
  else
    result["source"] = py::none();
  result["current"] = snapshotIdentityToDict(event->current);
  return std::move(result);
}

py::object invalidationToObject(const std::optional<ista::csr::CsrInvalidationEvent>& event)
{
  if (!event)
    return py::none();

  py::dict result;
  result["reason"] = std::string(ista::csr::token(event->reason));
  result["writes_applied"] = event->writes_applied;
  result["previous_identity"] = snapshotIdentityToDict(event->previous_identity);
  result["current_fingerprints"] = currentFingerprintsToDict(event->current_fingerprints);
  return std::move(result);
}

}  // namespace

bool initStaConfigMapByJSON(const std::string& config, std::map<std::string, std::any>& config_map);
void initStaConfigMapByDict(std::map<std::string, std::string>& config_dict, std::map<std::string, std::any>& config_map);

bool initSTA(std::string& config, std::map<std::string, std::string>& config_dict)
{
  std::map<std::string, std::any> config_map;

  bool pass = false;
  pass = config.empty() ? true : initStaConfigMapByJSON(config, config_map);
  if (!pass) {
    return false;
  }
  initStaConfigMapByDict(config_dict, config_map);
  STAI.initSTA(config_map);
  return true;
}

bool runSTA()
{
  STAI.runSTA();
  return true;
}

bool extractLib()
{
  STAI.extractLib();
  return true;
}

bool destroySTA()
{
  STAI.destroySTA();
  return true;
}

ista::StaExecutionRequest parseStaExecutionRequest(const std::map<std::string, std::string>& request)
{
  if (request.size() != 3)
    throw std::invalid_argument("STA execution request must contain exactly backend, update_strategy, and fallback_policy");
  return {.backend = ista::parseStaBackend(requireRequestField(request, "backend")),
          .update_strategy = ista::parseStaUpdateStrategy(requireRequestField(request, "update_strategy")),
          .fallback_policy = ista::parseStaFallbackPolicy(requireRequestField(request, "fallback_policy"))};
}

py::dict resolveStaExecution(const std::map<std::string, std::string>& request)
{
  ista::StaExecutionRequest parsed = parseStaExecutionRequest(request);
  return executionRecordToDict(ista::requireStaExecutionRecord(ista::resolveStage1StaExecution(parsed), parsed));
}

std::unique_ptr<ista::IncrementalStaSession> createIncrementalStaSession(const std::map<std::string, std::string>& request)
{
  return std::make_unique<ista::IncrementalStaSession>(parseStaExecutionRequest(request));
}

void initializeIncrementalStaSession(ista::IncrementalStaSession& session, py::handle analysis_context, const std::string& config,
                                     std::map<std::string, std::string> config_dict)
{
  auto parsed_context = parseStaAnalysisContext(analysis_context);
  auto parsed_config = parseStaConfig(config, config_dict);
  ista::throwOnStaFailure(session.initialize(std::move(parsed_context), std::move(parsed_config)), session.request());
}

py::dict runIncrementalStaBaseline(ista::IncrementalStaSession& session)
{
  return executionRecordToDict(ista::requireStaExecutionRecord(session.runFullBaseline(), session.request()));
}

void closeIncrementalStaSession(ista::IncrementalStaSession& session)
{
  ista::throwOnStaFailure(session.close(), session.request());
}

py::object incrementalStaSessionToken(const ista::IncrementalStaSession& session)
{
  const std::optional<std::uint64_t> token = session.sessionToken();
  return token ? py::cast(*token) : py::none();
}

py::dict serializeCsrSnapshotObservation(const ista::csr::CsrSnapshotObservation& observation)
{
  py::dict counts;
  counts["nodes"] = observation.counts.nodes;
  counts["arcs"] = observation.counts.arcs;
  counts["levels"] = observation.counts.levels;

  py::dict capacity;
  capacity["last_action"] = std::string(ista::csr::token(observation.capacity.last_action));
  capacity["event_count"] = observation.capacity.event_count;

  py::list unsupported_reasons;
  for (const auto reason : observation.support.unsupported_reasons)
    unsupported_reasons.append(std::string(ista::csr::token(reason)));
  py::dict support;
  support["reference_supported"] = observation.support.reference_supported;
  support["unsupported_reasons"] = std::move(unsupported_reasons);

  py::dict timing;
  timing["export"] = observation.timing_ms.export_ms;
  timing["reference"] = optionalDouble(observation.timing_ms.reference_ms);
  timing["scatter"] = optionalDouble(observation.timing_ms.scatter_ms);

  py::dict host_bytes;
  host_bytes["logical"] = observation.host_bytes.logical;
  host_bytes["capacity"] = observation.host_bytes.capacity;

  py::dict result;
  result["schema_version"] = observation.schema_version;
  result["state"] = std::string(ista::csr::token(observation.state));
  result["identity"] = snapshotIdentityToDict(observation.identity);
  result["counts"] = std::move(counts);
  result["buffers"] = snapshotBuffersToDict(observation.buffers);
  result["capacity"] = std::move(capacity);
  result["support"] = std::move(support);
  result["timing_ms"] = std::move(timing);
  result["host_bytes"] = std::move(host_bytes);
  result["last_rejection"] = scatterRejectionToObject(observation.last_rejection);
  result["last_invalidation"] = invalidationToObject(observation.last_invalidation);
  return result;
}

py::object incrementalStaSnapshotObservation(ista::IncrementalStaSession& session)
{
  std::unique_ptr<ista::csr::CsrSnapshotObservation> observation = session.snapshotObservation();
  if (!observation)
    return py::none();

  const std::optional<std::uint64_t> token = session.sessionToken();
  if (!token || *token == 0 || observation->identity.session_token != *token)
    throw std::logic_error("CSR snapshot observation does not belong to the incremental STA session");
  return serializeCsrSnapshotObservation(*observation);
}

}  // namespace python_interface
