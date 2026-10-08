// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "CsrFingerprint.hpp"
#include "json.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "PhysicalStaBatchJson.hpp"
#include "NativeCandidateExecutor.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>

namespace ista {
namespace {
std::string physicalGeometryIdentity(idb::IdbDesign& design)
{
  csr::CanonicalEncoder bytes;
  bytes.appendDomain("physical-checkpoint-geometry", "1");
  auto instances = design.get_instance_list()->get_instance_list();
  auto ports = design.get_io_pin_list()->get_pin_list();
  const auto names = [](auto* left, auto* right) { return left->get_name() < right->get_name(); };
  if (std::find(instances.begin(), instances.end(), nullptr) != instances.end()
      || std::find(ports.begin(), ports.end(), nullptr) != ports.end())
    throw std::invalid_argument("physical checkpoint has a null instance or port");
  std::sort(instances.begin(), instances.end(), names);
  std::sort(ports.begin(), ports.end(), [](auto* left, auto* right) { return left->get_pin_name() < right->get_pin_name(); });
  bytes.appendU64(instances.size());
  for (auto* instance : instances) {
    if (!instance || !instance->get_cell_master() || !instance->get_coordinate())
      throw std::invalid_argument("physical checkpoint instance geometry is missing");
    bytes.appendString(instance->get_name());
    bytes.appendString(instance->get_cell_master()->get_name());
    bytes.appendI32(instance->get_coordinate()->get_x());
    bytes.appendI32(instance->get_coordinate()->get_y());
    bytes.appendU8(static_cast<std::uint8_t>(instance->get_orient()));
    bytes.appendU8(static_cast<std::uint8_t>(instance->get_status()));
  }
  bytes.appendU64(ports.size());
  for (auto* port : ports) {
    if (!port || !port->get_average_coordinate())
      throw std::invalid_argument("physical checkpoint port geometry is missing");
    bytes.appendString(port->get_pin_name());
    bytes.appendI32(port->get_average_coordinate()->get_x());
    bytes.appendI32(port->get_average_coordinate()->get_y());
  }
  return csr::sha256Hex(bytes.bytes());
}
}
void PhysicalStaSessionState::bindRecovery(const PhysicalStaConfig& config, const csr::CsrReferenceInput& input, bool recovery)
{
  if (_journal || config.recovery_directory.empty() || config.checkpoint_files.empty())
    throw std::invalid_argument("physical baseline requires a recovery directory and checkpoint inputs");
  using Json = nlohmann::json;
  Json files = Json::array();
  for (const auto& [role, name] : config.checkpoint_files) {
    if (role.empty() || name.empty()) throw std::invalid_argument("physical checkpoint input identity is empty");
    const auto path = std::filesystem::canonical(name);
    if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("physical checkpoint input is not a regular file");
    const auto size = std::filesystem::file_size(path);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::invalid_argument("physical checkpoint input cannot be opened");
    files.push_back({{"role", role}, {"path", path.string()}, {"size", size}, {"sha256", csr::sha256Hex(stream, size)}});
  }
  const auto layer = [](const izh::PreRouteLayerRc& value) {
    return Json{{"name", value.name}, {"width_um", value.width_um}, {"resistance_per_um", value.resistance_per_um},
      {"area_capacitance_per_um", value.area_capacitance_per_um}, {"edge_capacitance_per_um", value.edge_capacitance_per_um}};
  };
  const auto& identity = input.source_identity;
  const auto baseline = Json{
      {"schema_version", "physical-sta-baseline/1.0.0"}, {"inputs", std::move(files)},
      {"physical_geometry", physicalGeometryIdentity(design)},
      {"build", {{"compiler_id", incregpusta_compiler_id}, {"compiler_version", incregpusta_compiler_version},
                 {"build_config", incregpusta_build_config}, {"cuda_enabled", incregpusta_cuda_enabled},
                 {"debug_checks_enabled", incregpusta_debug_checks_enabled}}},
      {"request", {{"backend", toString(request.backend)}, {"update_strategy", toString(request.update_strategy)},
                   {"fallback_policy", toString(request.fallback_policy)}}},
      {"analysis", {{"liberty_corner", input.analysis_context.liberty_corner}, {"temperature", input.analysis_context.temperature},
                    {"rc_corner", input.analysis_context.rc_corner}}},
      {"rc", {{"dbu_per_um", technology.dbu_per_um}, {"horizontal", layer(technology.horizontal)}, {"vertical", layer(technology.vertical)}}},
      {"identity", {{"schema_version", identity.fingerprint_schema_version}, {"topology", identity.topology_fingerprint},
                    {"clock_constraint", identity.clock_constraint_fingerprint}, {"library", identity.library_fingerprint},
                    {"corner", identity.corner_fingerprint}, {"parasitic", identity.parasitic_fingerprint},
                    {"timing_input", identity.timing_input_fingerprint}}}}.dump();
  auto id = csr::sha256Hex(std::span(reinterpret_cast<const std::uint8_t*>(baseline.data()), baseline.size()));
  auto journal = std::make_unique<PhysicalStaJournal>(config.recovery_directory, baseline);
  // Reopening committed history belongs to explicit recovery, never a new baseline.
  if (!recovery && journal->generation() != 0) throw std::invalid_argument("physical checkpoint has committed history; recovery is required");
  _recovery_directory = std::filesystem::canonical(config.recovery_directory);
  _baseline_id = std::move(id);
  _journal = std::move(journal);
}
StaExecutionRecord PhysicalStaSessionState::recover(Database& database, StaCsrSessionContext& context,
    const timing_edit::NativeExecutionAuthority& authority, StaExecutionRecord execution)
{
  if (!_journal)
    throw std::invalid_argument("physical recovery backend is not enabled");
  try {
    while (generation(context).value() != _journal->generation()) {
      const auto before = generation(context).value();
      const auto decoded = decodePhysicalStaRecoveryBatch(_journal->readCommitted(before + 1), _baseline_id,
                                                          before, request, technology.dbu_per_um);
      const auto actual = applyBatchImpl(database, context, authority, decoded.batch, decoded.locations, true);
      if (actual.generation != before + 1 || actual.execution != decoded.execution)
        throw std::logic_error("physical recovery execution differs from the committed record");
      execution = actual.execution;
    }
    return execution;
  } catch (...) {
    authority.poison();
    throw;
  }
}
}  // namespace ista
