// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "PhysicalStaSessionState.hpp"
#include "CsrFingerprint.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrReferenceCanonicalEncoderInternal.hpp"
#include "json.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "PhysicalStaBatchJson.hpp"
#include "NativeCandidateExecutor.hpp"
#include "Database.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <set>

namespace ista {
namespace {
std::string nativeCheckpointIdentity(Database& database)
{
  csr::CanonicalEncoder bytes;
  bytes.appendDomain("physical-checkpoint-native-input", "1");
  bytes.appendString(database.get_design_name());
  const auto names = [&](const auto& values) {
    bytes.appendU64(values.size());
    for (const auto& name : values) bytes.appendString(name);
  };
  const auto& instances = database.get_instance_map();
  std::set<std::string> used_cells;
  bytes.appendU64(instances.size());
  for (const auto& [name, instance] : instances) {
    bytes.appendString(name);
    bytes.appendString(instance.get_cell_name());
    names(instance.get_pin_name_list());
    used_cells.insert(instance.get_cell_name());
  }
  bytes.appendU64(used_cells.size());
  for (const auto& cell : used_cells)
    csr::detail::appendReferencedLibraryCellModelFields(bytes, csr::detail::projectCanonicalReferencedLibraryCell(database, cell));
  const auto& library = database.get_timing_library();
  for (double value : {library.get_slew_derate_from_library(),
      library.get_slew_lower_threshold_pct_rise(), library.get_slew_lower_threshold_pct_fall(),
      library.get_slew_upper_threshold_pct_rise(), library.get_slew_upper_threshold_pct_fall()})
    bytes.appendDouble(value);
  const auto& pins = database.get_pin_map();
  bytes.appendU64(pins.size());
  for (const auto& [name, pin] : pins) {
    bytes.appendString(name);
    bytes.appendString(pin.get_instance_name());
    bytes.appendString(pin.get_pin_name());
    bytes.appendString(pin.get_net_name());
    bytes.appendBool(pin.get_is_port());
    bytes.appendU8(static_cast<std::uint8_t>(pin.get_direction()));
  }
  const auto& nets = database.get_net_map();
  // Empty DEF nets carry no timing connectivity and may disappear on reload.
  bytes.appendU64(std::ranges::count_if(nets, [](const auto& net) { return !net.second.get_pin_name_list().empty(); }));
  for (const auto& [name, net] : nets) {
    if (net.get_pin_name_list().empty()) continue;
    bytes.appendString(name);
    names(net.get_pin_name_list());
  }
  const auto& constraints = database.get_timing_constraint();
  bytes.appendU64(constraints.get_clock_map().size());
  for (const auto& [name, clock] : constraints.get_clock_map()) {
    bytes.appendString(name);
    names(clock.get_source_list());
    bytes.appendDouble(clock.get_period());
    bytes.appendDouble(clock.get_rise_edge());
    bytes.appendDouble(clock.get_fall_edge());
    bytes.appendBool(clock.get_is_propagated());
  }
  bytes.appendU64(constraints.get_port_constraint_map().size());
  for (const auto& [name, port] : constraints.get_port_constraint_map()) {
    bytes.appendString(name);
    bytes.appendString(port.get_clock_name());
    for (const auto& [present, value] : {
        std::pair{port.get_has_input_delay_min(), port.get_input_delay_min()},
        std::pair{port.get_has_input_delay_max(), port.get_input_delay_max()},
        std::pair{port.get_has_output_delay_min(), port.get_output_delay_min()},
        std::pair{port.get_has_output_delay_max(), port.get_output_delay_max()},
        std::pair{port.get_has_input_transition(), port.get_input_transition()},
        std::pair{port.get_has_load(), port.get_load()}})
      bytes.appendOptionalDouble(present ? std::optional{value} : std::nullopt);
  }
  bytes.appendU64(constraints.get_case_analysis_map().size());
  for (const auto& [name, value] : constraints.get_case_analysis_map()) {
    bytes.appendString(name);
    bytes.appendBool(value);
  }
  return csr::sha256Hex(bytes.bytes());
}
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
void PhysicalStaSessionState::bindRecovery(const PhysicalStaConfig& config, Database& database, const csr::StaAnalysisContext& analysis, bool recovery)
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
  const auto baseline = Json{
      {"schema_version", "physical-sta-baseline/2.0.0"}, {"inputs", std::move(files)},
      {"physical_geometry", physicalGeometryIdentity(design)},
      {"build", {{"compiler_id", incregpusta_compiler_id}, {"compiler_version", incregpusta_compiler_version},
                 {"build_config", incregpusta_build_config}, {"cuda_enabled", incregpusta_cuda_enabled},
                 {"debug_checks_enabled", incregpusta_debug_checks_enabled}}},
      {"request", {{"backend", toString(request.backend)}, {"update_strategy", toString(request.update_strategy)},
                   {"fallback_policy", toString(request.fallback_policy)}}},
      {"analysis", {{"liberty_corner", analysis.liberty_corner}, {"temperature", analysis.temperature},
                    {"rc_corner", analysis.rc_corner}}},
      {"rc", {{"dbu_per_um", technology.dbu_per_um}, {"horizontal", layer(technology.horizontal)}, {"vertical", layer(technology.vertical)}}},
      // Bind loaded used-cell models too: public callers may supply only part of the input manifest.
      {"native_input", nativeCheckpointIdentity(database)}}.dump();
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
