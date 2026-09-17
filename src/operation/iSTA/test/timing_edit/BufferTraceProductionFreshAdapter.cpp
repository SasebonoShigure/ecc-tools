// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceProductionFreshAdapter.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "CsrFingerprint.hpp"
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include "BufferReplayContext.hpp"
#include "BufferTraceReplayer.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "Database.hpp"
#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "ProductionFreshSessionLoader.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include "idm.h"
#pragma GCC diagnostic pop
namespace ista::buffer_trace {
namespace {
namespace fs = std::filesystem;
using timing_edit::oracle::FreshBaselineArtifacts;
using timing_edit::oracle::ProductionFreshSessionView;
std::atomic<OneShotProcessState> g_one_shot_state{OneShotProcessState::kUnused};
std::atomic<std::uint64_t> g_loader_invocation_count{0}, g_guarded_work_invocation_count{0};
std::atomic<std::uint64_t> g_temp_ordinal{0};
[[noreturn]] void freshFail(FreshBaselineFailureCode code, std::string message)
{
  throw FreshBaselineError(code, std::move(message));
}
struct FinishOneShot
{
  ~FinishOneShot()
  {
    g_one_shot_state.store(OneShotProcessState::kFinished,
                           std::memory_order_release);
  }
};
fs::path makeOwnedTemporaryDirectory()
{
  std::error_code error;
  const fs::path root = fs::temp_directory_path(error);
  if (error || root.empty()) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              "system temporary directory is unavailable");
  }
  const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
  for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
    const std::uint64_t ordinal
        = g_temp_ordinal.fetch_add(1, std::memory_order_relaxed);
    const fs::path candidate
        = root / ("ista-buffer-trace-" + std::to_string(epoch) + "-"
                  + std::to_string(ordinal));
    if (fs::create_directory(candidate, error)) {
      return candidate;
    }
    if (error && error != std::errc::file_exists) {
      freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
                "cannot create an isolated artifact directory");
    }
    error.clear();
  }
  freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
            "cannot allocate a unique isolated artifact directory");
}
struct OwnedRunDirectory
{
  fs::path root;
  OwnedRunDirectory() : root(makeOwnedTemporaryDirectory()) {}
  ~OwnedRunDirectory()
  {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  OwnedRunDirectory(const OwnedRunDirectory&) = delete;
  OwnedRunDirectory& operator=(const OwnedRunDirectory&) = delete;
};
void writeArtifact(const fs::path& path, const std::string& source)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              "cannot open a synthetic source artifact for writing");
  }
  output.write(source.data(), static_cast<std::streamsize>(source.size()));
  output.close();
  if (!output) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              "cannot write a synthetic source artifact");
  }
}
std::string requireLeafName(const std::string& name, std::string_view role)
{
  const fs::path path(name);
  if (name.empty() || name.find('\0') != std::string::npos
      || path.filename() != path || path == "." || path == "..") {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              std::string(role) + " logical_name must be one safe leaf name");
  }
  return name;
}
SourceArtifactRefs materializeSynthetic(const SyntheticArtifactRecipe& recipe,
                                        const fs::path& directory)
{
  const std::array<std::string, 5> names{
      requireLeafName(recipe.tech_lef.logical_name, "tech_lef"),
      requireLeafName(recipe.cell_lef.logical_name, "cell_lef"),
      requireLeafName(recipe.liberty.logical_name, "liberty"),
      requireLeafName(recipe.verilog.logical_name, "verilog"),
      requireLeafName(recipe.sdc.logical_name, "sdc")};
  std::set<std::string> unique(names.begin(), names.end());
  if (unique.size() != names.size()) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              "five synthetic artifact roles require distinct paths");
  }
  const fs::path tech = directory / names[0];
  const fs::path cell = directory / names[1];
  const fs::path liberty = directory / names[2];
  const fs::path verilog = directory / names[3];
  const fs::path sdc = directory / names[4];
  writeArtifact(tech, recipe.tech_lef.source_text);
  writeArtifact(cell, recipe.cell_lef.source_text);
  writeArtifact(liberty, recipe.liberty.source_text);
  writeArtifact(verilog, recipe.verilog.source_text);
  writeArtifact(sdc, recipe.sdc.source_text);
  return {{tech.string()}, {{cell.string()}}, {{liberty.string()}},
          {verilog.string()}, {sdc.string()}, recipe.top, {"slow", "25", "__no_rc_corner__"}};
}
SourceArtifactRefs sourceRefs(const ReplayProgram& program,
                              const fs::path& temporary_directory)
{
  if (const auto* synthetic
      = std::get_if<SyntheticReplayPayload>(&program.workload)) {
    return materializeSynthetic(synthetic->source_recipe, temporary_directory);
  }
  return std::get<RealReplayPayload>(program.workload).artifacts;
}
std::vector<fs::path> canonicalArtifactPaths(const SourceArtifactRefs& refs)
{
  std::vector<fs::path> supplied{refs.tech_lef.path};
  for (const auto& source : refs.cell_lefs)
    supplied.emplace_back(source.path);
  for (const auto& source : refs.liberties)
    supplied.emplace_back(source.path);
  supplied.emplace_back(refs.verilog.path);
  supplied.emplace_back(refs.sdc.path);
  std::vector<fs::path> canonical(supplied.size());
  std::set<fs::path> unique;
  for (std::size_t index = 0; index < supplied.size(); ++index) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(supplied[index], error);
    if (error || status.type() != fs::file_type::regular) {
      freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
                "every source reference must name a regular non-symlink file");
    }
    canonical[index] = fs::canonical(supplied[index], error);
    if (error || !canonical[index].is_absolute()
        || !unique.insert(canonical[index]).second) {
      freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
                "source artifacts require distinct canonical paths");
    }
  }
  return canonical;
}
FreshBaselineArtifacts productionArtifacts(const SourceArtifactRefs& refs,
                                           const fs::path& root)
{
  if (refs.top.empty() || refs.top.find('\0') != std::string::npos) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              "source top name is empty or contains NUL");
  }
  const auto paths = canonicalArtifactPaths(refs);
  const fs::path run_directory = root / "run";
  std::error_code error;
  if (!fs::create_directory(run_directory, error) || error) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              "cannot create the isolated production run directory");
  }
  FreshBaselineArtifacts result{paths.front(), {}, {}, paths[paths.size() - 2], paths.back(),
                                run_directory, refs.top, refs.analysis_context};
  std::size_t index = 1;
  for (std::size_t count = 0; count < refs.cell_lefs.size(); ++count)
    result.cell_lefs.push_back(paths[index++]);
  for (std::size_t count = 0; count < refs.liberties.size(); ++count)
    result.liberties.push_back(paths[index++]);
  return result;
}
std::vector<SourceArtifactFunctionalRecord> captureSourceArtifacts(
    const FreshBaselineArtifacts& artifacts)
{
  const auto files = timing_edit::oracle::freshArtifactFiles(artifacts);
  std::vector<SourceArtifactFunctionalRecord> result;
  result.reserve(files.size());
  for (std::size_t index = 0; index < files.size(); ++index) {
    std::error_code error;
    const fs::path canonical = fs::canonical(files[index].path, error);
    std::ifstream input(canonical, std::ios::binary);
    if (error || !input) {
      freshFail(FreshBaselineFailureCode::kArtifactEvidenceMismatch,
                "source artifact disappeared during the guarded run");
    }
    const std::uintmax_t size = fs::file_size(canonical, error);
    if (error || size > static_cast<std::uintmax_t>(
                            std::numeric_limits<std::streamsize>::max())) {
      freshFail(FreshBaselineFailureCode::kArtifactEvidenceMismatch,
                "source artifact size could not be captured exactly");
    }
    std::string digest;
    try {
      digest = csr::sha256Hex(input, size);
    } catch (const std::exception& error) {
      freshFail(FreshBaselineFailureCode::kArtifactEvidenceMismatch, error.what());
    }
    result.push_back({files[index].role, files[index].logical_name,
                      canonical.string(), static_cast<std::uint64_t>(size),
                      digest});
  }
  return result;
}
std::string endpointName(idb::IdbPin* pin)
{
  if (pin == nullptr) {
    return "<null>";
  }
  return pin->get_instance() == nullptr
             ? "PIN/" + pin->get_pin_name()
             : pin->get_instance()->get_name() + "/" + pin->get_pin_name();
}
SourcePinFunctionalRecord capturePin(idb::IdbPin* pin, bool io)
{
  if (pin == nullptr) {
    return {.name = "<null>", .instance = {}, .net = {},
            .direction = -1, .io = io};
  }
  const auto* term = pin->get_term();
  return {.name = pin->get_pin_name(),
          .instance = pin->get_instance() == nullptr
                          ? "" : pin->get_instance()->get_name(),
          .net = pin->get_net_name(),
          .direction = term == nullptr
                           ? -1 : static_cast<std::int32_t>(term->get_direction()),
          .io = io};
}
SourceIdbFunctionalProjection captureIdb(idb::IdbDesign* design)
{
  SourceIdbFunctionalProjection result;
  if (design == nullptr) {
    return result;
  }
  result.present = true;
  result.design_name = design->get_design_name();
  if (auto* list = design->get_instance_list(); list != nullptr) {
    for (auto* instance : list->get_instance_list()) {
      if (instance == nullptr) {
        result.instances.push_back({.name = "<null>", .master = {}, .type = 0,
                                    .status = 0, .orient = 0, .x = 0, .y = 0, .pins = {}});
        continue;
      }
      auto* coordinate = instance->get_coordinate();
      SourceInstanceFunctionalRecord record{
          .name = instance->get_name(),
          .master = instance->get_cell_master() == nullptr
                        ? "" : instance->get_cell_master()->get_name(),
          .type = static_cast<std::int32_t>(instance->get_type()),
          .status = static_cast<std::int32_t>(instance->get_status()),
          .orient = static_cast<std::int32_t>(instance->get_orient()),
          .x = coordinate == nullptr ? 0 : coordinate->get_x(),
          .y = coordinate == nullptr ? 0 : coordinate->get_y(),
          .pins = {}};
      if (auto* pins = instance->get_pin_list(); pins != nullptr) {
        for (auto* pin : pins->get_pin_list()) {
          record.pins.push_back(capturePin(pin, false));
          result.connected_pin_count += pin != nullptr && pin->get_net() != nullptr;
        }
      }
      std::ranges::sort(record.pins, {}, &SourcePinFunctionalRecord::name);
      result.placed_instance_count += instance->is_placed();
      result.fixed_instance_count += instance->is_fixed();
      result.unplaced_instance_count += instance->is_unplaced();
      result.instances.push_back(std::move(record));
    }
  }
  std::ranges::sort(result.instances, {}, &SourceInstanceFunctionalRecord::name);
  if (auto* pins = design->get_io_pin_list(); pins != nullptr) {
    for (auto* pin : pins->get_pin_list()) {
      result.io_pins.push_back(capturePin(pin, true));
      result.connected_pin_count += pin != nullptr && pin->get_net() != nullptr;
    }
  }
  std::ranges::sort(result.io_pins, {}, &SourcePinFunctionalRecord::name);
  if (auto* list = design->get_net_list(); list != nullptr) {
    for (auto* net : list->get_net_list()) {
      if (net == nullptr) {
        result.nets.push_back({.name = "<null>", .type = 0, .driver = {},
                               .loads = {}, .wires = {}});
        continue;
      }
      SourceNetFunctionalRecord record{.name = net->get_net_name(),
                                       .type = static_cast<std::int32_t>(net->get_connect_type()),
                                       .driver = endpointName(net->get_driving_pin()),
                                       .loads = {}, .wires = {}};
      const auto capture_loads = [&](idb::IdbPins* pins) {
        if (pins != nullptr) for (auto* pin : pins->get_pin_list())
          if (pin != net->get_driving_pin()) record.loads.push_back(endpointName(pin));
      };
      capture_loads(net->get_instance_pin_list());
      capture_loads(net->get_io_pins());
      std::ranges::sort(record.loads);
      if (auto* wires = net->get_wire_list(); wires != nullptr) {
        result.wire_count += wires->get_wire_list().size();
        for (auto* wire : wires->get_wire_list()) {
          SourceWireFunctionalRecord wire_record;
          if (wire != nullptr) {
            wire_record.statement = static_cast<std::int32_t>(wire->get_wire_statement());
            wire_record.shield = wire->get_shiled_name();
            result.wire_length += wire->wireLength();
            result.segment_count += wire->get_segment_list().size();
            for (auto* segment : wire->get_segment_list()) {
              SourceWireSegmentFunctionalRecord segment_record;
              if (segment != nullptr) {
                segment_record = {.layer = segment->get_layer_name(),
                                  .new_layer = segment->is_new_layer(),
                                  .wire = segment->is_wire(), .via = segment->is_via(),
                                  .rect = segment->is_rect(), .points = {}, .vias = {},
                                  .delta_rect = std::nullopt};
                for (auto* point : segment->get_point_list()) if (point != nullptr)
                  segment_record.points.push_back({point->get_x(), point->get_y(), segment->is_virtual(point) ? 1 : 0});
                for (auto* via : segment->get_via_list())
                  segment_record.vias.push_back(via == nullptr ? "<null>" : via->get_name());
                if (auto* rect = segment->get_delta_rect(); rect != nullptr)
                  segment_record.delta_rect = {{rect->get_low_x(), rect->get_low_y(), rect->get_high_x(), rect->get_high_y()}};
                result.wire_point_count += segment_record.points.size();
                result.via_count += segment_record.vias.size();
              }
              wire_record.segments.push_back(std::move(segment_record));
            }
          }
          record.wires.push_back(std::move(wire_record));
        }
      }
      result.nets.push_back(std::move(record));
    }
  }
  std::ranges::sort(result.nets, {}, &SourceNetFunctionalRecord::name);
  return result;
}
SourcePhysicalFunctionalReceipt captureSourcePhysical()
{
  return {.primary_idb = captureIdb(dmInst->get_idb_design()),
          .optional_netlist_view = captureIdb(dmInst->get_netlist_idb_design()),
          .spef_reader_present = dmInst->get_spef_reader() != nullptr};
}
timing_edit::ComponentFingerprints fingerprints(
    const csr::CsrSnapshotIdentity& identity)
{
  return {identity.fingerprint_schema_version, identity.topology_fingerprint,
          identity.clock_constraint_fingerprint, identity.library_fingerprint,
          identity.corner_fingerprint, identity.parasitic_fingerprint,
          identity.timing_input_fingerprint};
}
timing_edit::CanonicalPreStateEvidence canonicalEvidence(
    const Database& database, const csr::CsrReferenceInput& input)
{
  timing_edit::CanonicalPreStateEvidence result;
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(instance);
    result.instances.push_back(name);
  }
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    result.nets.push_back(name);
  }
  for (const csr::NodeRecord& node : input.nodes) {
    result.nodes.push_back(node.key);
  }
  for (const csr::ArcRecord& arc : input.arcs) {
    result.arcs.push_back(arc.key);
  }
  std::ranges::sort(result.nodes, csr::nodeKeyLess);
  std::ranges::sort(result.arcs, csr::arcKeyLess);
  return result;
}
timing_edit::BaselineSemanticIdentity baselineIdentity(
    const ProductionFreshSessionView& view)
{
  return {view.database.get_design_name(),
          fingerprints(view.reference_input.source_identity),
          canonicalEvidence(view.database, view.reference_input)};
}
std::optional<timing_edit::BaselineSemanticIdentity> expectedBaseline(
    const ReplayProgram& program)
{
  if (const auto* real = std::get_if<RealReplayPayload>(&program.workload)) {
    return real->baseline;
  }
  const auto& synthetic = std::get<SyntheticReplayPayload>(program.workload);
  if (!synthetic.batches.empty()) {
    return synthetic.batches.front().baseline;
  }
  return std::nullopt;
}
std::unique_ptr<csr::CsrCommittedSessionSnapshot> buildHostSnapshot(
    const ProductionFreshSessionView& view)
{
  csr::CsrSessionSnapshotBuildResult built = csr::buildCsrSessionSnapshot(
      view.database, view.reference_input.analysis_context, view.session_token);
  auto* snapshot
      = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&built);
  if (snapshot == nullptr || *snapshot == nullptr) {
    freshFail(FreshBaselineFailureCode::kBaselineCaptureFailed,
              "generation-zero host CSR snapshot build failed");
  }
  return std::move(*snapshot);
}
void requireArtifactEvidence(const ProductionFreshSessionView& view,
                             const FreshBaselineArtifacts& artifacts)
{
  const auto files = timing_edit::oracle::freshArtifactFiles(artifacts);
  if (view.artifact_evidence.size() != files.size()) {
    freshFail(FreshBaselineFailureCode::kArtifactEvidenceMismatch,
              "production loader returned the wrong evidence cardinality");
  }
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto& evidence = view.artifact_evidence[index];
    if (evidence.role != files[index].role
        || evidence.logical_name != files[index].logical_name
        || evidence.content_sha256.empty()) {
      freshFail(FreshBaselineFailureCode::kArtifactEvidenceMismatch,
                "production loader artifact evidence differs by role/name");
    }
  }
}
struct InstalledRollback
{
  timing_edit::InstalledNativeDatabase* installed{nullptr};
};
bool rollbackInstalled(void* opaque) noexcept
{
  auto& signal = *static_cast<InstalledRollback*>(opaque);
  return signal.installed != nullptr && signal.installed->rollback();
}
void requireTransactionOk(timing_edit::TransactionStatus status,
                          std::string_view operation)
{
  if (!status.ok()) {
    throw BufferReplayError(
        BufferReplayFailureCode::kPublishRejected,
        std::string(operation) + " returned transaction status "
            + std::to_string(static_cast<std::uint32_t>(status.code)));
  }
}
}  // namespace
class BufferTraceProductionFreshAdapter::Services final
    : public GuardedReplayServices
{
 public:
  Services(const ProductionFreshSessionView& view, timing_edit::TimingEditCoordinator& coordinator) noexcept
      : _view(view), _coordinator(coordinator) {}
 private:
  Database& database() noexcept override { return STADM.getDatabase(); }
  const csr::StaAnalysisContext& analysisContext() const noexcept override
  { return _view.reference_input.analysis_context; }
  const timing_edit::NativeExecutionAuthority& authority() const noexcept override
  { return _view.authority; }
  timing_edit::TimingEditCoordinator& coordinator() noexcept override
  { return _coordinator; }
  void proveSealAndPublish(PendingVerifiedBatch&& pending) override
  {
    BufferTraceProductionFreshAdapter::proveSealAndPublish(_view, _coordinator,
                                                           std::move(pending));
  }
  const ProductionFreshSessionView& _view;
  timing_edit::TimingEditCoordinator& _coordinator;
};
OneShotProcessState BufferTraceProductionFreshAdapter::oneShotState() noexcept
{ return g_one_shot_state.load(std::memory_order_acquire); }
std::uint64_t BufferTraceProductionFreshAdapter::loaderInvocationCount() noexcept
{ return g_loader_invocation_count.load(std::memory_order_acquire); }
std::uint64_t BufferTraceProductionFreshAdapter::guardedWorkInvocationCount() noexcept
{ return g_guarded_work_invocation_count.load(std::memory_order_acquire); }
void BufferTraceProductionFreshAdapter::proveSealAndPublish(
    const ProductionFreshSessionView& view,
    timing_edit::TimingEditCoordinator& coordinator,
    PendingVerifiedBatch&& pending)
{
  try {
    timing_edit::CsrExecutionAuthorization proof = StaTimingEditSessionProbeAccess::proveCsrFullExact(
        view.authority, *pending._host_snapshot, pending._installed.database(), pending._prepared.targetGeneration());
    requireTransactionOk(
        coordinator.sealVerified(pending._prepared, pending._post_identity),
        "sealVerified");
    timing_edit::VerifiedPublishBundle bundle(
        std::move(pending._installed), std::move(pending._host_snapshot),
        std::move(proof), std::move(pending._prepared),
        std::move(pending._cumulative), std::move(pending._staged_resolver));
    requireTransactionOk(
        coordinator.publish(view.authority, std::move(bundle)), "publish");
  } catch (...) {
    if (pending._installed.active()) {
      InstalledRollback rollback{&pending._installed};
      const timing_edit::TransactionStatus status
          = coordinator.rollbackPrecommit({&rollback, &rollbackInstalled});
      if (!status.ok()) {
        throw BufferReplayError(BufferReplayFailureCode::kPublishRejected,
                                "proof/seal failure also failed exact rollback");
      }
    }
    throw;
  }
}
ReplayReceipt BufferTraceProductionFreshAdapter::runOnce(
    const ReplayProgram& program, BufferReplayWork work,
    bool generation_template)
{
  OneShotProcessState expected = OneShotProcessState::kUnused;
  if (!g_one_shot_state.compare_exchange_strong(
          expected, OneShotProcessState::kEntered, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    freshFail(FreshBaselineFailureCode::kFreshProcessReuseRejected,
              "a production-fresh process may enter the loader only once");
  }
  FinishOneShot finish_one_shot;
  if (!work) {
    freshFail(FreshBaselineFailureCode::kGuardedWorkFailed,
              "guarded replay work is empty");
  }
  const BufferTraceContractViolations violations
      = generation_template ? validateReplayTemplate(program)
                            : validateReplayProgram(program);
  if (!violations.empty()) {
    freshFail(FreshBaselineFailureCode::kArtifactMaterializationRejected,
              violations.front().field + ": " + violations.front().message);
  }
  OwnedRunDirectory owned;
  const SourceArtifactRefs refs = sourceRefs(program, owned.root);
  const FreshBaselineArtifacts artifacts = productionArtifacts(refs, owned.root);
  _derived_freshness.emplace();
  _source_isolation.emplace();
  _source_isolation->artifacts_before = captureSourceArtifacts(artifacts);
  const auto requireArtifactsUnchanged = [&] {
    _source_isolation->artifacts_after = captureSourceArtifacts(artifacts);
    if (_source_isolation->artifacts_before
        != _source_isolation->artifacts_after) {
      freshFail(FreshBaselineFailureCode::kArtifactEvidenceMismatch,
                "production loader or guarded work changed source artifacts");
    }
  };
  std::optional<ReplayReceipt> result;
  std::exception_ptr guarded_work_failure; std::size_t callback_count = 0;
  g_loader_invocation_count.fetch_add(1, std::memory_order_acq_rel);
  try {
    timing_edit::oracle::ProductionFreshSessionLoader::run(
      artifacts, [&](const ProductionFreshSessionView& view) {
        ++callback_count;
        if (callback_count != 1 || result) {
          freshFail(FreshBaselineFailureCode::kGuardedWorkFailed,
                    "production loader invoked replay work more than once");
        }
        requireArtifactEvidence(view, artifacts);
        const timing_edit::BaselineSemanticIdentity actual_baseline
            = baselineIdentity(view);
        if (const auto expected_baseline = expectedBaseline(program);
            expected_baseline
            && (!generation_template
                || *expected_baseline
                       != timing_edit::BaselineSemanticIdentity{})
            && *expected_baseline != actual_baseline) {
          freshFail(FreshBaselineFailureCode::kBaselineIdentityMismatch,
                    "fresh Database/CSR baseline differs from replay identity");
        }
        const timing_edit::TimingEditSessionToken token_value{view.session_token};
        const timing_edit::CommittedEditIdentity initial_identity
            = timing_edit::NativeTimingEditPreparer::captureCurrentIdentity(
                view.database, timing_edit::TransactionGeneration{0},
                view.reference_input.analysis_context, token_value);
        if (initial_identity
            != timing_edit::CommittedEditIdentity{
                timing_edit::TransactionGeneration{0},
                actual_baseline.fingerprints,
                actual_baseline.canonical_pre_state}) {
          freshFail(FreshBaselineFailureCode::kBaselineIdentityMismatch,
                    "generation-zero native identity differs from CSR baseline");
        }
        _source_isolation->physical_before = captureSourcePhysical();
        if (!_source_isolation->physical_before.primary_idb.present) {
          freshFail(FreshBaselineFailureCode::kGuardedWorkFailed,
                    "production Verilog loader exposed no authoritative primary IDB");
        }
        const auto requirePhysicalUnchanged = [&] {
          _source_isolation->physical_after = captureSourcePhysical();
          if (_source_isolation->physical_before
              != _source_isolation->physical_after) {
            freshFail(FreshBaselineFailureCode::kGuardedWorkFailed,
                      "guarded work changed source IDM/IDB state");
          }
        };
        try {
        if (program.header.workload_kind == SemanticWorkloadKind::kRealDesign) {
          *_derived_freshness
              = test_support::proveDerivedTimingFreshness(view,
                                                          initial_identity);
        }
        timing_edit::TimingEditResolverState resolver
            = timing_edit::TimingEditResolverState::bindBaseline(view.database);
        std::unique_ptr<csr::CsrCommittedSessionSnapshot> initial_host
            = buildHostSnapshot(view);
        const StaCsrCaptureAttempt capture
            = StaTimingEditSessionProbeAccess::captureSnapshotForTesting(
                view.session_context, view.database);
        const auto* captured = std::get_if<csr::CsrCaptureResult>(&capture.value);
        if (captured == nullptr || captured->error) {
          freshFail(FreshBaselineFailureCode::kBaselineCaptureFailed,
                    "fresh session CSR observation capture failed");
        }
        std::unique_ptr<csr::CsrSnapshotObservation> released;
        StaTimingEditSessionProbeAccess::releaseSnapshotForMutation(
            view.session_context, &released);
        if (released == nullptr) {
          freshFail(FreshBaselineFailureCode::kBaselineCaptureFailed,
                    "fresh session CSR observation was not released for mutation");
        }
        timing_edit::TimingEditCoordinator coordinator(
            token_value, StaTimingEditSessionProbeAccess::stateSignal(view.session),
            actual_baseline, initial_identity, {}, std::move(resolver),
            std::move(initial_host));
        Services services(view, coordinator);
        try {
          BufferReplayContext context(services);
          g_guarded_work_invocation_count.fetch_add(1, std::memory_order_acq_rel);
          result.emplace(work(context));
          if (result->header != program.header
              || result->committed_identity != coordinator.committedIdentity()
              || result->cumulative_trace != coordinator.cumulativeTrace()
              || result->resolver_state != coordinator.resolverState()) {
            freshFail(FreshBaselineFailureCode::kGuardedWorkFailed,
                      "guarded work returned a receipt that differs from committed state");
          }
        } catch (...) {
          if (_prove_first_empty_failure)
            _first_empty_generation_zero_exact
                = test_support::proveFirstEmptyGenerationZero(view, coordinator, initial_identity);
          guarded_work_failure = std::current_exception();
        }
        } catch (...) { requirePhysicalUnchanged(); throw; }
        requirePhysicalUnchanged();
      });
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    requireArtifactsUnchanged();
    test_support::rethrowTypedLoaderFailure(callback_count, failure);
  }
  requireArtifactsUnchanged();
  if (guarded_work_failure) test_support::rethrowTypedLoaderFailure(callback_count, guarded_work_failure);
  if (callback_count != 1 || !result
      || loaderInvocationCount() != 1
      || guardedWorkInvocationCount() != 1
      || oneShotState() != OneShotProcessState::kEntered) {
    freshFail(FreshBaselineFailureCode::kGuardedWorkFailed,
              "one-shot production loader did not complete exactly one callback");
  }
  return std::move(*result);
}
const SourceIsolationFunctionalReceipt& BufferTraceProductionFreshAdapter::sourceIsolationReceipt() const
{
  if (!_source_isolation) throw std::logic_error("source isolation receipt is unavailable before a run");
  return *_source_isolation;
}
const DerivedTimingFreshnessFunctionalReceipt& BufferTraceProductionFreshAdapter::derivedTimingFreshnessReceipt() const
{
  if (!_derived_freshness) throw std::logic_error("derived timing freshness receipt is unavailable before a run");
  return *_derived_freshness;
}
ReplayReceipt BufferTraceProductionFreshAdapter::withFreshBaseline(
    const ReplayProgram& program, BufferReplayWork work)
{ return runOnce(program, std::move(work), false); }
ReplayReceipt BufferTraceProductionFreshAdapter::withFreshGenerationBaseline(
    const ReplayProgram& empty_template, BufferReplayWork work)
{ return runOnce(empty_template, std::move(work), true); }
}  // namespace ista::buffer_trace
