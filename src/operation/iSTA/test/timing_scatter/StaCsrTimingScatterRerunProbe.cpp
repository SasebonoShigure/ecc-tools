// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Arc.hpp"
#include "CsrNativeTimingOracleTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrTimingScatterTestFixture.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "Logger.hpp"
#include "TimingCellArc.hpp"
#include "TimingPropagator.hpp"
#include "json.hpp"

namespace {

using Json = nlohmann::json;
using namespace ista::csr;
using namespace ista::csr::test;

inline constexpr std::string_view kProbeSchema = "ista-csr-timing-scatter-rerun-probe/1.0.0";

struct Options
{
  ReferenceNativeFixtureCase fixture_case{ReferenceNativeFixtureCase::kDiamond};
  std::string case_name;
  std::string fallback_mode;
  std::string run_nonce;
};

struct FallbackTarget
{
  std::size_t arc{0};
  std::size_t entry{0};
  std::size_t analysis{0};
  std::size_t input_transition{0};
  std::size_t output_transition{0};
};

struct ForwardNodeObservation
{
  NodeKey key;
  double arrival{0.0};
  double launch{0.0};
  std::optional<std::string> clock_name;
  std::optional<NodeKey> predecessor_node;
  std::optional<ArcKey> predecessor_arc;
  CsrNativeTimingLaneMatrix<double> data_slews;

  friend bool operator==(const ForwardNodeObservation&, const ForwardNodeObservation&) = default;
};

struct DynamicArcObservation
{
  ArcKey key;
  CsrNativeTimingArcLaneMatrix<double> graph_delays;
  std::vector<CsrNativeTimingArcEntry> timing_arcs;

  friend bool operator==(const DynamicArcObservation&, const DynamicArcObservation&) = default;
};

struct SummaryObservation
{
  std::size_t path_group_count{0};
  std::size_t timing_path_num{0};
  std::size_t checked_end_point_num{0};
  std::size_t unconstrained_end_point_num{0};
  std::size_t violating_end_point_num{0};
  double worst_slack{0.0};
  double total_negative_slack{0.0};
  std::string worst_end_point;

  friend bool operator==(const SummaryObservation&, const SummaryObservation&) = default;
};

struct ArcStorageObservation
{
  const ista::Arc* data{nullptr};
  std::vector<const ista::Arc*> addresses;
  std::vector<ArcKey> keys;
  std::vector<std::size_t> native_ranks;

  friend bool operator==(const ArcStorageObservation&, const ArcStorageObservation&) = default;
};

class ScopedOutputSilence
{
 public:
  ScopedOutputSilence()
  {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    _saved_stdout = ::dup(STDOUT_FILENO);
    _saved_stderr = ::dup(STDERR_FILENO);
    const int sink = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (_saved_stdout < 0 || _saved_stderr < 0 || sink < 0) {
      const int saved_errno = errno;
      if (sink >= 0) {
        static_cast<void>(::close(sink));
      }
      restore();
      throw std::runtime_error("failed to prepare probe output isolation: " + std::string(std::strerror(saved_errno)));
    }
    if (::dup2(sink, STDOUT_FILENO) < 0 || ::dup2(sink, STDERR_FILENO) < 0) {
      const int saved_errno = errno;
      static_cast<void>(::close(sink));
      restore();
      throw std::runtime_error("failed to install probe output isolation: " + std::string(std::strerror(saved_errno)));
    }
    static_cast<void>(::close(sink));
  }

  ~ScopedOutputSilence()
  {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    restore();
  }

  ScopedOutputSilence(const ScopedOutputSilence&) = delete;
  ScopedOutputSilence& operator=(const ScopedOutputSilence&) = delete;

 private:
  void restore() noexcept
  {
    if (_saved_stdout >= 0) {
      static_cast<void>(::dup2(_saved_stdout, STDOUT_FILENO));
      static_cast<void>(::close(_saved_stdout));
      _saved_stdout = -1;
    }
    if (_saved_stderr >= 0) {
      static_cast<void>(::dup2(_saved_stderr, STDERR_FILENO));
      static_cast<void>(::close(_saved_stderr));
      _saved_stderr = -1;
    }
  }

  int _saved_stdout{-1};
  int _saved_stderr{-1};
};

class NativeTimingEnvironment
{
 public:
  NativeTimingEnvironment()
  {
    ista::Logger::initInst();
    ista::DataManager::initInst();
    ista::DelayCalculator::initInst();
    ista::TimingPropagator::initInst();
  }

  ~NativeTimingEnvironment()
  {
    ista::TimingPropagator::destroyInst();
    ista::DelayCalculator::destroyInst();
    ista::DataManager::destroyInst();
    ista::Logger::destroyInst();
  }

  NativeTimingEnvironment(const NativeTimingEnvironment&) = delete;
  NativeTimingEnvironment& operator=(const NativeTimingEnvironment&) = delete;
};

void require(bool condition, std::string_view message)
{
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct CellArcBinding
{
  std::size_t native_arc_rank{0};
  std::string cell_map_key;
  std::size_t cell_arc_index{0};
  const ista::TimingCellArc* source_pointer{nullptr};
};

std::vector<CellArcBinding> captureCellArcBindings(const ista::Database& database)
{
  std::vector<CellArcBinding> result;
  const auto& cells = database.get_timing_library().get_cell_map();
  for (std::size_t native_arc_rank = 0; native_arc_rank < database.get_arc_list().size(); ++native_arc_rank) {
    const ista::TimingCellArc* pointer = database.get_arc_list()[native_arc_rank].get_timing_cell_arc();
    if (pointer == nullptr) {
      continue;
    }
    std::optional<CellArcBinding> match;
    for (const auto& [cell_map_key, cell] : cells) {
      const auto& cell_arcs = cell.get_cell_arc_list();
      for (std::size_t cell_arc_index = 0; cell_arc_index < cell_arcs.size(); ++cell_arc_index) {
        if (std::addressof(cell_arcs[cell_arc_index]) != pointer) {
          continue;
        }
        require(!match.has_value(), "bound timing-cell Arc pointer matches multiple library entries");
        match = CellArcBinding{native_arc_rank, cell_map_key, cell_arc_index, pointer};
      }
    }
    require(match.has_value(), "bound timing-cell Arc pointer is outside the source timing library");
    result.push_back(std::move(*match));
  }
  return result;
}

std::size_t copyDatabaseAndRebindCellArcs(const ista::Database& source, ista::Database& target)
{
  require(std::addressof(source) != std::addressof(target), "database copy source and target unexpectedly alias");
  const std::vector<CellArcBinding> bindings = captureCellArcBindings(source);
  target = source;
  require(target.get_arc_list().size() == source.get_arc_list().size(), "database copy changed native Arc cardinality");
  for (std::size_t rank = 0; rank < source.get_arc_list().size(); ++rank) {
    if (source.get_arc_list()[rank].get_timing_cell_arc() == nullptr) {
      require(target.get_arc_list()[rank].get_timing_cell_arc() == nullptr, "database copy invented a timing-cell Arc binding");
    }
  }
  for (const CellArcBinding& binding : bindings) {
    ista::Arc& target_arc = target.get_arc_list().at(binding.native_arc_rank);
    const ista::Arc& source_arc = source.get_arc_list().at(binding.native_arc_rank);
    require(target_arc.get_timing_cell_arc() == binding.source_pointer, "database copy did not expose the expected shallow raw binding");
    const auto target_cell = target.get_timing_library().get_cell_map().find(binding.cell_map_key);
    require(target_cell != target.get_timing_library().get_cell_map().end(), "database copy lost a bound timing cell");
    auto& target_cell_arcs = target_cell->second.get_cell_arc_list();
    require(binding.cell_arc_index < target_cell_arcs.size(), "database copy lost a bound timing-cell Arc");
    ista::TimingCellArc* candidate = std::addressof(target_cell_arcs[binding.cell_arc_index]);
    require(candidate != binding.source_pointer, "database copy unexpectedly shared timing-cell Arc storage");
    require(target_arc.get_type() == ista::ArcType::kCell && target_arc.get_owner_name() == source_arc.get_owner_name()
                && target_arc.get_library_source_port() == candidate->get_source_port() && target_arc.get_library_sink_port() == candidate->get_sink_port(),
            "database copy changed bound timing-cell Arc semantics");
    target_arc.set_timing_cell_arc(candidate);
    require(target_arc.get_timing_cell_arc() == candidate, "database copy timing-cell Arc rebind did not stick");
  }
  const std::vector<CellArcBinding> rebound = captureCellArcBindings(target);
  require(rebound.size() == bindings.size(), "database copy changed timing-cell Arc binding cardinality");
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    require(rebound[index].native_arc_rank == bindings[index].native_arc_rank && rebound[index].cell_map_key == bindings[index].cell_map_key
                && rebound[index].cell_arc_index == bindings[index].cell_arc_index,
            "database copy changed timing-cell Arc binding identity");
  }
  return bindings.size();
}

bool isLowerHex(std::string_view value, std::size_t size)
{
  if (value.size() != size) {
    return false;
  }
  for (const char character : value) {
    if ((character < '0' || character > '9') && (character < 'a' || character > 'f')) {
      return false;
    }
  }
  return true;
}

Options parseOptions(int argc, char* argv[])
{
  if (argc != 7) {
    throw std::runtime_error("expected exact --case, --fallback-mode, and --run-nonce pairs");
  }
  std::map<std::string, std::string> values;
  for (int index = 1; index < argc; index += 2) {
    const std::string name = argv[index];
    if (!name.starts_with("--") || !values.emplace(name, argv[index + 1]).second) {
      throw std::runtime_error("invalid or duplicate probe option");
    }
  }
  if (values.size() != 3 || !values.contains("--case") || !values.contains("--fallback-mode") || !values.contains("--run-nonce")) {
    throw std::runtime_error("unknown or missing probe option");
  }

  Options options;
  options.case_name = values.at("--case");
  options.fallback_mode = values.at("--fallback-mode");
  options.run_nonce = values.at("--run-nonce");
  if (options.case_name == "chain") {
    options.fixture_case = ReferenceNativeFixtureCase::kChain;
    if (options.fallback_mode != "equal" && options.fallback_mode != "different") {
      throw std::runtime_error("chain fallback mode must be equal or different");
    }
  } else if (options.case_name == "diamond") {
    options.fixture_case = ReferenceNativeFixtureCase::kDiamond;
    if (options.fallback_mode != "none") {
      throw std::runtime_error("diamond fallback mode must be none");
    }
  } else {
    throw std::runtime_error("case must be chain or diamond");
  }
  if (!isLowerHex(options.run_nonce, 32)) {
    throw std::runtime_error("run nonce must be 32 lowercase hexadecimal characters");
  }
  return options;
}

std::vector<ForwardNodeObservation> observeForwardNodes(const CsrNativeTimingOracle& oracle)
{
  std::vector<ForwardNodeObservation> result;
  result.reserve(oracle.nodes.size());
  for (const CsrNativeTimingNode& node : oracle.nodes) {
    result.push_back({node.key, node.arrival, node.launch, node.clock_name, node.predecessor_node, node.predecessor_arc, node.data_slews});
  }
  return result;
}

std::vector<DynamicArcObservation> observeDynamicArcs(const CsrNativeTimingOracle& oracle)
{
  std::vector<DynamicArcObservation> result;
  result.reserve(oracle.arcs.size());
  for (const CsrNativeTimingArc& arc : oracle.arcs) {
    result.push_back({arc.key, arc.graph_delays, arc.timing_arcs});
  }
  return result;
}

std::vector<std::pair<NodeKey, double>> observeRequired(const CsrNativeTimingOracle& oracle)
{
  std::vector<std::pair<NodeKey, double>> result;
  result.reserve(oracle.nodes.size());
  for (const CsrNativeTimingNode& node : oracle.nodes) {
    result.emplace_back(node.key, node.required);
  }
  return result;
}

std::vector<std::pair<NodeKey, std::vector<CsrNativePathTimingState>>> observePaths(const CsrNativeTimingOracle& oracle)
{
  std::vector<std::pair<NodeKey, std::vector<CsrNativePathTimingState>>> result;
  result.reserve(oracle.nodes.size());
  for (const CsrNativeTimingNode& node : oracle.nodes) {
    result.emplace_back(node.key, node.paths);
  }
  return result;
}

SummaryObservation observeSummary(ista::Database& database)
{
  const ista::TASummary& summary = database.get_summary().ta_summary;
  return {database.get_timing_path_group_list().size(),
          summary.timing_path_num,
          summary.checked_end_point_num,
          summary.unconstrained_end_point_num,
          summary.violating_end_point_num,
          summary.worst_slack,
          summary.total_negative_slack,
          summary.worst_end_point};
}

ArcStorageObservation observeArcStorage(const ista::Database& database, const CsrReferenceInput& input)
{
  const auto& arcs = database.get_arc_list();
  ArcStorageObservation result;
  result.data = arcs.data();
  result.keys.resize(arcs.size());
  std::vector<bool> keys_seen(arcs.size(), false);
  for (const ArcRecord& record : input.arcs) {
    require(record.native_traversal_rank < arcs.size(), "committed Arc rank is outside native storage");
    const std::size_t rank = static_cast<std::size_t>(record.native_traversal_rank);
    require(!keys_seen[rank], "committed Arc ranks are not unique");
    result.keys[rank] = record.key;
    keys_seen[rank] = true;
  }
  for (bool key_seen : keys_seen) {
    require(key_seen, "committed Arc ranks do not cover native storage");
  }
  result.addresses.reserve(arcs.size());
  result.native_ranks.reserve(arcs.size());
  for (std::size_t rank = 0; rank < arcs.size(); ++rank) {
    result.addresses.push_back(&arcs[rank]);
    result.native_ranks.push_back(rank);
  }
  return result;
}
double distinctFiniteCanary(double baseline)
{
  require(std::isfinite(baseline), "guaranteed-overwrite canary baseline must be finite");
  double canary = std::nextafter(baseline, std::numeric_limits<double>::infinity());
  if (!std::isfinite(canary)) {
    canary = std::nextafter(baseline, -std::numeric_limits<double>::infinity());
  }
  require(std::isfinite(canary) && canary != baseline, "could not construct a distinct finite guaranteed-overwrite canary");
  return canary;
}

void installGuaranteedOverwriteCanaries(ista::Database& database, const CsrReferenceInput& input, const CsrNativeTimingOracle& baseline_oracle,
                                        const Options& options)
{
  const std::string_view start_name = options.fixture_case == ReferenceNativeFixtureCase::kChain ? "din" : "root";
  std::optional<std::size_t> start_node;
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    if (input.nodes[node].key.full_name == start_name) {
      require(!start_node, "guaranteed-overwrite start node is duplicated");
      start_node = node;
    }
  }
  require(start_node.has_value(), "guaranteed-overwrite start node does not resolve");
  const CsrNativeTimingNode* baseline_start = nullptr;
  for (const CsrNativeTimingNode& node : baseline_oracle.nodes) {
    if (node.key == input.nodes[*start_node].key) {
      require(baseline_start == nullptr, "guaranteed-overwrite baseline start key is duplicated");
      baseline_start = &node;
    }
  }
  require(baseline_start != nullptr, "guaranteed-overwrite baseline start key does not resolve");
  bool is_native_start = false;
  for (const std::string& native_start : database.get_start_point_list()) {
    is_native_start |= native_start == start_name;
  }
  require(is_native_start, "guaranteed-overwrite start node is absent from the native start list");
  bool is_seeded_start = false;
  for (const StartSeedModel& seed : input.start_seeds) {
    is_seeded_start |= seed.seed_node.value() == *start_node;
  }
  require(is_seeded_start, "guaranteed-overwrite start node has no committed seed");

  ista::TimingPoint& start = database.get_timing_point_map().at(input.nodes[*start_node].key.full_name);
  require(baseline_start->arrival == start.get_arrival(), "guaranteed-overwrite start scalar differs from baseline before canary");
  start.set_arrival(distinctFiniteCanary(start.get_arrival()));
  auto& start_slews = start.get_data_slew_map();
  require(start_slews.contains(ista::AnalysisType::kMax) && start_slews.at(ista::AnalysisType::kMax).contains(ista::TransType::kRise),
          "guaranteed-overwrite start max/rise data-slew is absent");
  double& start_slew = start_slews.at(ista::AnalysisType::kMax).at(ista::TransType::kRise);
  const auto& baseline_start_slew = baseline_start->data_slews[0][0];
  require(baseline_start_slew && *baseline_start_slew == start_slew, "guaranteed-overwrite start max/rise data-slew differs from baseline before canary");
  start_slew = distinctFiniteCanary(start_slew);
  require(start_slew >= 0.0, "guaranteed-overwrite start data-slew canary became negative");

  const std::string_view arc_source = start_name;
  const std::string_view arc_sink = options.fixture_case == ReferenceNativeFixtureCase::kChain ? "ff0:D" : "sink";
  const ArcRecord* target_record = nullptr;
  for (const ArcRecord& record : input.arcs) {
    if (record.kind == ArcKind::kNet && record.key.base.source.full_name == arc_source && record.key.base.sink.full_name == arc_sink) {
      require(target_record == nullptr, "guaranteed-overwrite graph-delay Arc is duplicated");
      target_record = &record;
    }
  }
  require(target_record != nullptr, "guaranteed-overwrite graph-delay Arc does not resolve");
  require(!target_record->is_disabled && !target_record->is_loop_disabled && target_record->eligibility.data_slew,
          "guaranteed-overwrite graph-delay Arc is not one active data-slew net Arc");
  require(target_record->native_traversal_rank < database.get_arc_list().size(), "guaranteed-overwrite graph-delay Arc rank is out of range");

  ista::Arc& target_arc = database.get_arc_list().at(static_cast<std::size_t>(target_record->native_traversal_rank));
  require(target_arc.get_source_pin() == arc_source && target_arc.get_sink_pin() == arc_sink && target_arc.get_type() == ista::ArcType::kNet
              && !target_arc.get_is_disable_arc() && !target_arc.get_is_loop_disable(),
          "guaranteed-overwrite native Arc does not match the committed active net");
  const CsrNativeTimingArc* baseline_target_arc = nullptr;
  for (const CsrNativeTimingArc& arc : baseline_oracle.arcs) {
    if (arc.key == target_record->key) {
      require(baseline_target_arc == nullptr, "guaranteed-overwrite baseline Arc key is duplicated");
      baseline_target_arc = &arc;
    }
  }
  require(baseline_target_arc != nullptr, "guaranteed-overwrite baseline Arc key does not resolve");
  auto& graph_delays = target_arc.get_graph_delay_map();
  require(graph_delays.contains(ista::AnalysisType::kMax) && graph_delays.at(ista::AnalysisType::kMax).contains(ista::TransType::kRise)
              && graph_delays.at(ista::AnalysisType::kMax).at(ista::TransType::kRise).contains(ista::TransType::kRise),
          "guaranteed-overwrite max/rise-to-rise graph-delay is absent");
  double& graph_delay = graph_delays.at(ista::AnalysisType::kMax).at(ista::TransType::kRise).at(ista::TransType::kRise);
  const auto& baseline_graph_delay = baseline_target_arc->graph_delays[0][0][0];
  require(baseline_graph_delay && std::isfinite(*baseline_graph_delay) && *baseline_graph_delay == graph_delay,
          "guaranteed-overwrite max/rise-to-rise graph-delay differs from baseline before canary");
  require(std::isfinite(graph_delay), "guaranteed-overwrite graph-delay baseline is not finite");
  graph_delay = -std::numeric_limits<double>::infinity();
}

FallbackTarget requireDynamicFallbackTarget(const CsrNativeTimingOracle& oracle)
{
  for (std::size_t arc = 0; arc < oracle.arcs.size(); ++arc) {
    for (std::size_t entry = 0; entry < oracle.arcs[arc].timing_arcs.size(); ++entry) {
      if (oracle.arcs[arc].timing_arcs[entry].timing_arc_index != 12) {
        continue;
      }
      for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis) {
        for (std::size_t input = 0; input < kTransitionCount; ++input) {
          for (std::size_t output = 0; output < kTransitionCount; ++output) {
            const auto& lane = oracle.arcs[arc].timing_arcs[entry].lanes[analysis][input][output];
            if (lane && !lane->is_fallback) {
              return {arc, entry, analysis, input, output};
            }
          }
        }
      }
    }
  }
  throw std::runtime_error("chain baseline has no dynamically replaced data-cell timing-arc lane");
}

CsrNativeTimingArcDelay& targetLane(CsrNativeTimingOracle& oracle, const FallbackTarget& target)
{
  auto& lane = oracle.arcs.at(target.arc).timing_arcs.at(target.entry).lanes.at(target.analysis).at(target.input_transition).at(target.output_transition);
  if (!lane) {
    throw std::runtime_error("selected timing-arc lane became absent");
  }
  return *lane;
}

const CsrNativeTimingArcDelay& targetLane(const CsrNativeTimingOracle& oracle, const FallbackTarget& target)
{
  const auto& lane = oracle.arcs.at(target.arc).timing_arcs.at(target.entry).lanes.at(target.analysis).at(target.input_transition).at(target.output_transition);
  if (!lane) {
    throw std::runtime_error("selected timing-arc lane became absent");
  }
  return *lane;
}

void installFallback(CsrNativeTimingOracle& oracle, const FallbackTarget& target, bool equal_value)
{
  CsrNativeTimingArcDelay& lane = targetLane(oracle, target);
  require(!lane.is_fallback && std::isfinite(lane.delay), "fallback target baseline is not one finite dynamic lane");
  lane.is_fallback = true;
  if (!equal_value) {
    const double replacement = lane.delay + 123.0;
    require(std::isfinite(replacement) && replacement != lane.delay, "could not construct a distinct finite fallback value");
    lane.delay = replacement;
  }
}

void rerunNativeTiming()
{
  ista::DelayCalculator::getInst().init();
  ista::TimingPropagator::getInst().propagate();
}

Json runCase(const Options& options)
{
  CsrTimingScatterTestFixture fixture = makeTimingScatterFixture(options.fixture_case);
  const CsrNativeTimingOracle baseline_oracle = fixture.expected_oracle;
  const CsrTimingResult baseline_result = materializeTimingResultFromNativeOracle(fixture.committed_input, baseline_oracle);
  require(validateTimingResult(fixture.committed_input, baseline_result).empty(), "baseline materialized result is not strict-valid");
  requireNativeTimingOracleEqual(readNativeTimingOracle(fixture.database()), baseline_oracle, "native baseline");

  CsrNativeTimingOracle installed_oracle = baseline_oracle;
  std::optional<FallbackTarget> fallback_target;
  std::optional<bool> installed_fallback_equal_to_dynamic;
  if (options.fixture_case == ReferenceNativeFixtureCase::kChain) {
    fallback_target = requireDynamicFallbackTarget(baseline_oracle);
    installed_fallback_equal_to_dynamic = options.fallback_mode == "equal";
    installFallback(installed_oracle, *fallback_target, *installed_fallback_equal_to_dynamic);
  }
  const CsrTimingResult installed_result = materializeTimingResultFromNativeOracle(fixture.committed_input, installed_oracle);
  require(validateTimingResult(fixture.committed_input, installed_result).empty(), "installed fallback result is not strict-valid");

  const auto baseline_required = observeRequired(baseline_oracle);
  const auto baseline_paths = observePaths(baseline_oracle);
  const SummaryObservation baseline_summary = observeSummary(fixture.database());
  const auto installed_forward_nodes = observeForwardNodes(installed_oracle);
  const auto installed_dynamic_arcs = observeDynamicArcs(installed_oracle);
  poisonTimingScatterFixture(fixture);
  const CsrNativeTimingOracle poisoned_oracle = readNativeTimingOracle(fixture.database());
  require(poisoned_oracle != installed_oracle, "timing poison did not change the installed oracle");
  require(observeForwardNodes(poisoned_oracle) != installed_forward_nodes, "timing poison did not change forward node families");
  require(observeDynamicArcs(poisoned_oracle) != installed_dynamic_arcs, "timing poison did not change dynamic Arc families");
  require(observeRequired(poisoned_oracle) != baseline_required, "timing poison did not change required state");
  require(observePaths(poisoned_oracle) != baseline_paths, "timing poison did not change path state");
  require(observeSummary(fixture.database()) == baseline_summary, "timing poison unexpectedly changed path-group or summary state");

  const std::vector<std::uint8_t> source_bytes = fixture.forbidden_bytes;
  NativeTimingEnvironment environment;
  const std::size_t rebound_cell_arcs = copyDatabaseAndRebindCellArcs(fixture.database(), STADM.getDatabase());
  require(rebound_cell_arcs == (options.fixture_case == ReferenceNativeFixtureCase::kChain ? 4U : 0U),
          "database copy rebound the wrong number of timing-cell Arcs");
  ista::Database& database = STADM.getDatabase();
  require(timingScatterForbiddenBytes(database) == source_bytes, "copying and rebinding the fixture changed source/reference bytes");

  const CsrTimingResultScatterAttempt attempt
      = scatterCsrTimingResult(database, fixture.committed_input, installed_result, fixture.context, fixture.session_token, fixture.graph_generation);
  requireScatterCode(attempt, CsrScatterCode::kSuccess, expectedTimingScatterWrites(fixture.committed_input), std::nullopt, "rerun scatter");
  const CsrNativeTimingOracle scattered_oracle = readNativeTimingOracle(database);
  requireNativeTimingOracleEqual(scattered_oracle, installed_oracle, "immediate post-scatter restore");
  require(observeRequired(scattered_oracle) == baseline_required, "scatter did not restore non-rerun required state");
  require(observePaths(scattered_oracle) == baseline_paths, "scatter did not restore non-rerun path state");
  require(observeSummary(database) == baseline_summary, "scatter changed non-rerun path-group or summary state");
  require(timingScatterForbiddenBytes(database) == source_bytes, "scatter changed source/reference bytes");

  if (fallback_target) {
    const CsrNativeTimingArcDelay& installed_lane = targetLane(scattered_oracle, *fallback_target);
    const CsrNativeTimingArcDelay& dynamic_lane = targetLane(baseline_oracle, *fallback_target);
    require(installed_lane.is_fallback, "scatter did not install the fallback marker");
    require((installed_lane.delay == dynamic_lane.delay) == *installed_fallback_equal_to_dynamic,
            "installed fallback equality case differs from its declared mode");
  }
  const auto scattered_forward_nodes = observeForwardNodes(scattered_oracle);
  const auto scattered_dynamic_arcs = observeDynamicArcs(scattered_oracle);
  installGuaranteedOverwriteCanaries(database, fixture.committed_input, baseline_oracle, options);
  const CsrNativeTimingOracle canary_oracle = readNativeTimingOracle(database);
  const auto canary_forward_nodes = observeForwardNodes(canary_oracle);
  const auto canary_dynamic_arcs = observeDynamicArcs(canary_oracle);
  require(canary_forward_nodes != scattered_forward_nodes, "guaranteed-overwrite canary did not change forward projection");
  require(canary_dynamic_arcs != scattered_dynamic_arcs, "guaranteed-overwrite canary did not change dynamic Arc projection");
  require(canary_forward_nodes != observeForwardNodes(baseline_oracle), "pre-rerun forward projection unexpectedly equals baseline");
  require(canary_dynamic_arcs != observeDynamicArcs(baseline_oracle), "pre-rerun dynamic Arc projection unexpectedly equals baseline");

  const ArcStorageObservation storage_before = observeArcStorage(database, fixture.committed_input);
  rerunNativeTiming();
  const ArcStorageObservation storage_after = observeArcStorage(database, fixture.committed_input);
  require(storage_after == storage_before, "native rerun rebuilt or reordered Arc storage/key/rank/address");

  const CsrNativeTimingOracle rerun_oracle = readNativeTimingOracle(database);
  const auto rerun_forward_nodes = observeForwardNodes(rerun_oracle);
  const auto baseline_forward_nodes = observeForwardNodes(baseline_oracle);
  require(rerun_forward_nodes.size() == baseline_forward_nodes.size(), "native rerun forward node cardinality differs from baseline");
  for (std::size_t index = 0; index < rerun_forward_nodes.size(); ++index) {
    const ForwardNodeObservation& rerun_node = rerun_forward_nodes[index];
    const ForwardNodeObservation& baseline_node = baseline_forward_nodes[index];
    require(rerun_node.key == baseline_node.key, "native rerun forward node ordering differs from baseline");
    const std::string name = rerun_node.key.full_name;
    require(rerun_node.arrival == baseline_node.arrival, "native rerun arrival differs from baseline at " + name);
    require(rerun_node.launch == baseline_node.launch, "native rerun launch differs from baseline at " + name);
    require(rerun_node.clock_name == baseline_node.clock_name, "native rerun clock name differs from baseline at " + name);
    require(rerun_node.predecessor_node == baseline_node.predecessor_node, "native rerun predecessor node differs from baseline at " + name);
    require(rerun_node.predecessor_arc == baseline_node.predecessor_arc, "native rerun predecessor Arc differs from baseline at " + name);
    for (std::size_t analysis = 0; analysis < kAnalysisCount; ++analysis)
      for (std::size_t transition = 0; transition < kTransitionCount; ++transition)
        require(rerun_node.data_slews[analysis][transition] == baseline_node.data_slews[analysis][transition],
                "native rerun data slew differs from baseline at " + name + "/" + std::to_string(analysis) + "/" + std::to_string(transition));
  }
  require(observeDynamicArcs(rerun_oracle) == observeDynamicArcs(baseline_oracle), "native rerun dynamic Arc families differ from baseline");
  require(timingScatterForbiddenBytes(database) == source_bytes, "native rerun changed source/reference bytes");
  if (fallback_target) {
    const CsrNativeTimingArcDelay& rerun_lane = targetLane(rerun_oracle, *fallback_target);
    const CsrNativeTimingArcDelay& dynamic_lane = targetLane(baseline_oracle, *fallback_target);
    require(rerun_lane == dynamic_lane && !rerun_lane.is_fallback, "first native dynamic write did not replace and clear the fallback lane");
  }

  return {
      {"schema", kProbeSchema},
      {"case", options.case_name},
      {"fallback_mode", options.fallback_mode},
      {"run_nonce", options.run_nonce},
      {"baseline_result_materialized", true},
      {"poison_changed", true},
      {"scatter_immediate_exact_restore", true},
      {"writes_applied", attempt.scatter_result.writes_applied},
      {"delay_calculator_init_calls", 1},
      {"timing_propagator_calls", 1},
      {"graph_builder_calls", 0},
      {"forward_rerun_families_exact", true},
      {"arc_rerun_families_exact", true},
      {"arc_storage_preserved", true},
      {"source_bytes_preserved", true},
      {"non_rerun_required_restored_by_scatter", true},
      {"non_rerun_paths_restored_by_scatter", true},
      {"non_rerun_summary_preserved_by_scatter", true},
      {"fallback_target_observed", fallback_target.has_value()},
      {"installed_fallback_equal_to_dynamic", installed_fallback_equal_to_dynamic ? Json(*installed_fallback_equal_to_dynamic) : Json(nullptr)},
      {"fallback_behavior_verified", true},
      {"complete", true},
  };
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    const Options options = parseOptions(argc, argv);
    Json record;
    {
      ScopedOutputSilence silence;
      record = runCase(options);
    }
    std::cout << record.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "StaCsrTimingScatterRerunProbe: " << error.what() << '\n';
    return 1;
  }
}
