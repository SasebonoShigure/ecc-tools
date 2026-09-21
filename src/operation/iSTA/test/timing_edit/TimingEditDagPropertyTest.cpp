// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CsrFifoLevelization.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceValidator.hpp"
#include "DataManager.hpp"
#include "Database.hpp"
#include "DelayCalculator.hpp"
#include "GraphBuilder.hpp"
#include "Instance.hpp"
#include "Logger.hpp"
#include "LogicExpressionTerm.hpp"
#include "NativeDatabaseValidation.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "Net.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

constexpr std::array<std::uint64_t, 5> kSeeds{
    1, 7, 42, 20260831, 0xdeadbeefULL};
constexpr std::uint64_t kGraphSessionToken = 0x4752415048ULL;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::uint64_t nextXorshift64(std::uint64_t& state)
{
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

template <typename Value>
void xorshiftFisherYates(std::vector<Value>& values, std::uint64_t& state)
{
  for (std::size_t size = values.size(); size > 1; --size) {
    const std::size_t index
        = static_cast<std::size_t>(nextXorshift64(state) % size);
    std::swap(values[size - 1], values[index]);
  }
}

enum class Shape : std::uint8_t
{
  kSingleton,
  kChain,
  kDiamond,
  kMultiRoot,
  kParallel,
  kDisabled,
  kLoopDisabled
};

const char* shapeName(Shape shape)
{
  switch (shape) {
    case Shape::kSingleton:
      return "singleton";
    case Shape::kChain:
      return "chain";
    case Shape::kDiamond:
      return "diamond";
    case Shape::kMultiRoot:
      return "multi-root";
    case Shape::kParallel:
      return "parallel";
    case Shape::kDisabled:
      return "disabled";
    case Shape::kLoopDisabled:
      return "loop-disabled";
  }
  throw std::logic_error("unknown property shape");
}

struct PinSpec
{
  std::string full_name;
  std::string port_name;
  std::string instance_name;
  std::string net_name;
  PinDirection direction{PinDirection::kNone};
  bool is_port{false};
};

struct InstanceSpec
{
  std::string name;
  std::string cell;
  std::vector<std::string> pins;
};

struct NetSpec
{
  std::string name;
  std::string driver;
  std::vector<std::string> loads;
};

struct CellArcSpec
{
  std::string source;
  std::int32_t child_index{0};
  bool disabled{false};
};

struct CellSpec
{
  std::string name;
  std::vector<std::string> inputs;
  std::vector<CellArcSpec> arcs;
};

struct RawDagSpec
{
  Shape shape{Shape::kSingleton};
  std::vector<PinSpec> pins;
  std::vector<InstanceSpec> instances;
  std::vector<NetSpec> nets;
  std::vector<CellSpec> cells;
  std::size_t expected_nodes{0};
  std::size_t expected_arcs{0};
};

void addPort(RawDagSpec& spec, std::string name, PinDirection direction,
             std::string net = {})
{
  spec.pins.push_back(
      {name, name, {}, std::move(net), direction, true});
}

void addInstance(RawDagSpec& spec, std::string name, std::string cell,
                 std::vector<std::string> inputs,
                 std::string output = "Y")
{
  std::vector<std::string> pins;
  for (const std::string& input : inputs) {
    const std::string full = name + ":" + input;
    spec.pins.push_back(
        {full, input, name, {}, PinDirection::kInput, false});
    pins.push_back(full);
  }
  const std::string full_output = name + ":" + output;
  spec.pins.push_back(
      {full_output, output, name, {}, PinDirection::kOutput, false});
  pins.push_back(full_output);
  spec.instances.push_back({std::move(name), std::move(cell), std::move(pins)});
}

void addNet(RawDagSpec& spec, std::string name, std::string driver,
            std::vector<std::string> loads)
{
  for (PinSpec& pin : spec.pins) {
    if (pin.full_name == driver
        || std::ranges::find(loads, pin.full_name) != loads.end()) {
      pin.net_name = name;
    }
  }
  spec.nets.push_back(
      {std::move(name), std::move(driver), std::move(loads)});
}

CellSpec bufferCell(std::string name = "BUF_X1", bool disabled = false,
                    bool parallel = false)
{
  CellSpec cell{std::move(name), {"A"}, {{"A", 0, disabled}}};
  if (parallel) {
    cell.arcs.push_back({"A", 1, disabled});
  }
  return cell;
}

CellSpec mergeCell()
{
  return CellSpec{"MERGE2_X1", {"A", "B"},
                  {{"A", 0, false}, {"B", 1, false}}};
}

RawDagSpec makeSpec(Shape shape)
{
  RawDagSpec spec;
  spec.shape = shape;
  if (shape == Shape::kSingleton) {
    addPort(spec, "in", PinDirection::kInput);
    spec.expected_nodes = 1;
    return spec;
  }

  if (shape == Shape::kChain || shape == Shape::kParallel
      || shape == Shape::kDisabled) {
    addPort(spec, "in", PinDirection::kInput, "n0");
    addPort(spec, "out", PinDirection::kOutput, "n1");
    addInstance(spec, "u0",
                shape == Shape::kParallel
                    ? "PAR_X1"
                    : (shape == Shape::kDisabled ? "DIS_X1" : "BUF_X1"),
                {"A"});
    addNet(spec, "n0", "in", {"u0:A"});
    addNet(spec, "n1", "u0:Y", {"out"});
    spec.cells.push_back(
        shape == Shape::kParallel
            ? bufferCell("PAR_X1", false, true)
            : bufferCell(shape == Shape::kDisabled ? "DIS_X1" : "BUF_X1",
                         shape == Shape::kDisabled));
    spec.expected_nodes = 4;
    spec.expected_arcs = shape == Shape::kParallel ? 4 : 3;
    return spec;
  }

  if (shape == Shape::kDiamond) {
    addPort(spec, "in", PinDirection::kInput, "n0");
    addPort(spec, "out", PinDirection::kOutput, "n3");
    addInstance(spec, "u0", "BUF_X1", {"A"});
    addInstance(spec, "u1", "BUF_X1", {"A"});
    addInstance(spec, "u2", "MERGE2_X1", {"A", "B"});
    addNet(spec, "n0", "in", {"u0:A", "u1:A"});
    addNet(spec, "n1", "u0:Y", {"u2:A"});
    addNet(spec, "n2", "u1:Y", {"u2:B"});
    addNet(spec, "n3", "u2:Y", {"out"});
    spec.cells = {bufferCell(), mergeCell()};
    spec.expected_nodes = 9;
    spec.expected_arcs = 9;
    return spec;
  }

  if (shape == Shape::kMultiRoot) {
    addPort(spec, "in0", PinDirection::kInput, "n0");
    addPort(spec, "in1", PinDirection::kInput, "n1");
    addPort(spec, "out", PinDirection::kOutput, "n2");
    addInstance(spec, "u0", "MERGE2_X1", {"A", "B"});
    addNet(spec, "n0", "in0", {"u0:A"});
    addNet(spec, "n1", "in1", {"u0:B"});
    addNet(spec, "n2", "u0:Y", {"out"});
    spec.cells = {mergeCell()};
    spec.expected_nodes = 6;
    spec.expected_arcs = 5;
    return spec;
  }

  addInstance(spec, "u0", "BUF_X1", {"A"});
  addInstance(spec, "u1", "BUF_X1", {"A"});
  addNet(spec, "n0", "u0:Y", {"u1:A"});
  addNet(spec, "n1", "u1:Y", {"u0:A"});
  spec.cells = {bufferCell()};
  spec.expected_nodes = 4;
  spec.expected_arcs = 4;
  return spec;
}

LogicExpressionTerm expressionTerm(LogicOperationType operation,
                                   std::string port_name = {})
{
  LogicExpressionTerm term;
  term.set_operation_type(operation);
  term.set_port_name(port_name);
  return term;
}

TimingCellPort cellPort(const std::string& name, bool input)
{
  TimingCellPort port;
  port.set_port_name(name);
  port.set_is_input(input);
  port.set_is_output(!input);
  port.set_is_clock(false);
  if (!input) {
    port.get_function_expression().set_term_list(
        {expressionTerm(LogicOperationType::kPort, "A")});
  }
  return port;
}

TimingCell makeTimingCell(const CellSpec& spec)
{
  TimingCell cell;
  cell.set_cell_name(spec.name);
  cell.set_library_name("slow.lib");
  for (const std::string& input : spec.inputs) {
    cell.get_port_map().emplace(input, cellPort(input, true));
  }
  cell.get_port_map().emplace("Y", cellPort("Y", false));
  for (const CellArcSpec& arc_spec : spec.arcs) {
    TimingArc child;
    child.set_library_name("slow.lib");
    child.set_arc_idx(arc_spec.child_index);
    child.set_sense(TimingArcSense::kPositive);
    child.set_trigger_trans_type(TransType::kNone);
    child.set_check_trans_type(TransType::kNone);
    TimingCellArc arc;
    arc.set_source_port(arc_spec.source);
    arc.set_sink_port("Y");
    arc.set_delay(0.05);
    arc.set_delay_min(0.04);
    arc.set_delay_max(0.06);
    arc.set_is_timing_graph_arc(true);
    arc.set_is_clock_arc(false);
    arc.set_is_disable_arc(arc_spec.disabled);
    arc.set_is_clear_preset_arc(false);
    arc.get_timing_arc_list().push_back(std::move(child));
    cell.get_cell_arc_list().push_back(std::move(arc));
  }
  return cell;
}

Database buildRawDatabase(RawDagSpec spec, std::uint64_t seed)
{
  std::uint64_t state = seed;
  xorshiftFisherYates(spec.pins, state);
  xorshiftFisherYates(spec.instances, state);
  xorshiftFisherYates(spec.nets, state);
  xorshiftFisherYates(spec.cells, state);
  for (NetSpec& net : spec.nets) {
    xorshiftFisherYates(net.loads, state);
  }

  Database database;
  database.set_design_name(std::string("property_") + shapeName(spec.shape));
  for (const PinSpec& value : spec.pins) {
    Pin pin;
    pin.set_full_name(value.full_name);
    pin.set_pin_name(value.port_name);
    pin.set_instance_name(value.instance_name);
    pin.set_net_name(value.net_name);
    pin.set_direction(value.direction);
    pin.set_is_port(value.is_port);
    require(database.get_pin_map().emplace(value.full_name, std::move(pin)).second,
            "property pin collision");
  }
  for (const InstanceSpec& value : spec.instances) {
    Instance instance;
    instance.set_instance_name(value.name);
    instance.set_cell_name(value.cell);
    instance.set_pin_name_list(value.pins);
    instance.set_output_pin_name(value.name + ":Y");
    require(database.get_instance_map().emplace(value.name, std::move(instance)).second,
            "property instance collision");
  }
  for (const NetSpec& value : spec.nets) {
    Net net;
    net.set_net_name(value.name);
    std::vector<std::string> pins{value.driver};
    pins.insert(pins.end(), value.loads.begin(), value.loads.end());
    net.set_pin_name_list(pins);
    require(database.get_net_map().emplace(value.name, std::move(net)).second,
            "property net collision");
  }
  for (const CellSpec& value : spec.cells) {
    require(database.get_timing_library().get_cell_map()
                .emplace(value.name, makeTimingCell(value))
                .second,
            "property library cell collision");
  }
  return database;
}

void resetTimingRuntime() noexcept
{
  GraphBuilder::destroyInst();
  try {
    DelayCalculator::initInst();
    STADC.destroy();
  } catch (...) {
  }
  DelayCalculator::destroyInst();
}

class PropertyRuntime final
{
 public:
  PropertyRuntime()
  {
    Logger::initInst();
    DataManager::initInst();
  }
  ~PropertyRuntime()
  {
    resetTimingRuntime();
    DataManager::destroyInst();
    Logger::destroyInst();
  }
};

struct GraphResult
{
  std::vector<std::uint8_t> exact_logical_bytes;
  std::vector<ista::csr::NodeRecord> nodes;
  std::vector<ista::csr::ArcRecord> arcs;
  std::vector<std::uint64_t> out_row_offsets;
  std::vector<ista::csr::ArcId> out_arc_ids;
  std::vector<std::uint64_t> in_row_offsets;
  std::vector<ista::csr::ArcId> in_arc_ids;
  std::vector<ista::csr::NodeId> roots;
  std::vector<ista::csr::NodeId> starts;
  std::vector<ista::csr::NodeId> ends;
  ista::csr::CsrFifoLevelization fifo;
  std::vector<std::string> native_order;
  std::vector<std::pair<std::string, std::int32_t>> native_levels;
  friend bool operator==(const GraphResult&, const GraphResult&) = default;
};
GraphResult analyze(Shape shape, std::uint64_t seed)
{
  resetTimingRuntime();
  const RawDagSpec spec = makeSpec(shape);
  STADM.getDatabase() = buildRawDatabase(spec, seed);
  const std::vector<std::string> raw_fanout_membership
      = shape == Shape::kDiamond
            ? STADM.getDatabase().get_net_map().at("n0").get_pin_name_list()
            : std::vector<std::string>{};
  DelayCalculator::initInst();
  STADC.init();
  GraphBuilder::initInst();
  STAGB.build();
  GraphBuilder::destroyInst();

  const NativeDatabaseValidationResult failure
      = validateNativeDatabase(STADM.getDatabase());
  require(!failure.has_value(),
          std::string(shapeName(shape)) + " seed " + std::to_string(seed)
              + " failed native validation: "
              + (failure ? failure->object + ": " + failure->detail : ""));
  const Database& database = STADM.getDatabase();
  if (shape == Shape::kDiamond) {
    const Net& fanout = database.get_net_map().at("n0");
    require(fanout.get_pin_name_list() == raw_fanout_membership,
            "GraphBuilder changed raw net membership order");
    if (seed == kSeeds.front()) {
      require(raw_fanout_membership.size() == 3
                  && raw_fanout_membership[1] > raw_fanout_membership[2],
              "fixed-seed closure fixture is not non-lexical");
      std::vector<std::string> expected_loads(raw_fanout_membership.begin() + 1,
                                              raw_fanout_membership.end());
      std::ranges::sort(expected_loads);
      require(fanout.get_driver_pin() == raw_fanout_membership.front()
                  && fanout.get_driver_pin_list()
                         == std::vector<std::string>{raw_fanout_membership.front()}
                  && fanout.get_load_pin_list() == expected_loads,
              "GraphBuilder did not derive stable semantic-name net caches");
    }
  }
  require(database.get_timing_point_map().size() == spec.expected_nodes
              && database.get_timing_order_list().size() == spec.expected_nodes
              && database.get_arc_list().size() == spec.expected_arcs,
          std::string(shapeName(shape))
              + " GraphBuilder produced the wrong node/arc/order fields");
  if (shape == Shape::kParallel) {
    const auto parallel = std::ranges::count_if(
        database.get_arc_list(), [](const Arc& arc) {
          return arc.get_source_pin() == "u0:A"
                 && arc.get_sink_pin() == "u0:Y";
        });
    require(parallel == 2,
            "parallel shape lost one same-endpoint cell arc");
  }
  if (shape == Shape::kDisabled) {
    require(std::ranges::any_of(database.get_arc_list(), [](const Arc& arc) {
              return arc.get_is_disable_arc();
            }),
            "disabled shape lost its disabled arc");
  }
  if (shape == Shape::kLoopDisabled) {
    require(std::ranges::count_if(database.get_arc_list(), [](const Arc& arc) {
              return arc.get_is_loop_disable();
            }) == 1,
            "loop shape was not broken by an explicit loop-disabled arc");
  }

  using namespace ista::csr;
  CsrReferenceInputExportResult exported = exportCsrReferenceInputCandidate(
      database, editTestAnalysisContext(), kGraphSessionToken);
  const auto* input = std::get_if<CsrReferenceInput>(&exported);
  require(input != nullptr, std::string(shapeName(shape)) + " CSR export failed");
  require(validateReferenceInputForSession(*input, kGraphSessionToken).empty()
              && input->source_identity.session_token == kGraphSessionToken
              && input->unsupported_reasons.empty(),
          std::string(shapeName(shape)) + " CSR export is not strict supported");
  if (shape == Shape::kDiamond && seed == kSeeds.front()) {
    constexpr TransactionGeneration kBaselineGeneration{0};
    const TimingEditSessionToken session_token{kGraphSessionToken};
    const CommittedEditIdentity identity
        = NativeTimingEditPreparer::captureCurrentIdentity(
            database, kBaselineGeneration, editTestAnalysisContext(), session_token);
    const BaselineSemanticIdentity baseline{
        STADM.getDatabase().get_design_name(), identity.current_fingerprints,
        identity.canonical_pre_state};
    const TimingEditResolverState resolver
        = TimingEditResolverState::bindBaseline(database);
    NativeTimingEditPreparer::requireExactCommittedTraceClosure(
        database, *input, resolver, baseline, std::vector<EditBatch>{});
  }
  const CsrFifoLevelization fifo = replayFifoLevelization(*input);
  require(fifo.complete && fifo.level_nodes.size() == input->nodes.size()
              && fifo.node_to_level.size() == input->nodes.size(),
          std::string(shapeName(shape)) + " CSR FIFO replay is incomplete");

  std::vector<std::size_t> native_ranks(input->arcs.size(), 0);
  std::vector<std::size_t> active_indegree(input->nodes.size(), 0);
  for (std::size_t index = 0; index < input->nodes.size(); ++index) {
    require(input->nodes[index].id == NodeId{index},
            "CSR node ids are not exact dense order");
  }
  for (std::size_t index = 0; index < input->arcs.size(); ++index) {
    const ArcRecord& arc = input->arcs[index];
    require(arc.id == ArcId{index}
                && arc.native_traversal_rank < native_ranks.size()
                && native_ranks[arc.native_traversal_rank]++ == 0,
            "CSR arc ids/native ranks are not exact bijections");
    if (!arc.is_disabled && !arc.is_loop_disabled) {
      ++active_indegree[arc.sink.value()];
      require(fifo.node_to_level[arc.source.value()]
                  < fifo.node_to_level[arc.sink.value()],
              "active CSR arc does not advance FIFO level");
    }
  }
  for (std::size_t node = 0; node < input->nodes.size(); ++node) {
    for (std::size_t offset = input->out_row_offsets[node];
         offset < input->out_row_offsets[node + 1]; ++offset) {
      require(input->arcs[input->out_arc_ids[offset].value()].source
                  == NodeId{node},
              "CSR outgoing row contains a wrong source");
    }
    for (std::size_t offset = input->in_row_offsets[node];
         offset < input->in_row_offsets[node + 1]; ++offset) {
      require(input->arcs[input->in_arc_ids[offset].value()].sink
                  == NodeId{node},
              "CSR incoming row contains a wrong sink");
    }
  }
  std::vector<NodeId> expected_roots;
  for (std::size_t node = 0; node < active_indegree.size(); ++node) {
    if (active_indegree[node] == 0) expected_roots.push_back(NodeId{node});
  }
  require(input->ordered_root_ids == expected_roots,
          "CSR ordered roots differ from active indegree-zero nodes");
  if (shape == Shape::kMultiRoot) {
    require(input->ordered_root_ids.size() == 2,
            "multi-root CSR export did not preserve two roots");
  }

  GraphResult result{canonicalReferenceInputBytes(*input), input->nodes,
                     input->arcs, input->out_row_offsets, input->out_arc_ids,
                     input->in_row_offsets, input->in_arc_ids,
                     input->ordered_root_ids, input->ordered_start_ids,
                     input->ordered_end_ids, fifo,
                     database.get_timing_order_list(), {}};
  for (const auto& [name, point] : database.get_timing_point_map()) {
    result.native_levels.emplace_back(name, point.get_level());
  }
  return result;
}

void testFixedSeedGraphFamiliesAndCycleRejection()
{
  constexpr std::array<Shape, 7> kShapes{
      Shape::kSingleton, Shape::kChain, Shape::kDiamond, Shape::kMultiRoot,
      Shape::kParallel, Shape::kDisabled, Shape::kLoopDisabled};
  for (const Shape shape : kShapes) {
    std::optional<GraphResult> canonical;
    for (const std::uint64_t seed : kSeeds) {
      const GraphResult current = analyze(shape, seed);
      if (!canonical) {
        canonical = current;
      } else {
        require(current == *canonical,
                std::string(shapeName(shape)) + " seed "
                    + std::to_string(seed) + " changed exact native/CSR fields");
      }
    }
  }

  static_cast<void>(analyze(Shape::kLoopDisabled, kSeeds.front()));
  auto loop_arc = std::ranges::find_if(
      STADM.getDatabase().get_arc_list(),
      [](const Arc& arc) { return arc.get_is_loop_disable(); });
  require(loop_arc != STADM.getDatabase().get_arc_list().end(),
          "cycle rejection fixture lacks loop-disabled arc");
  loop_arc->set_is_loop_disable(false);
  const NativeDatabaseValidationResult cycle
      = validateNativeDatabase(STADM.getDatabase());
  require(cycle.has_value()
              && cycle->code == NativeDatabaseValidationCode::kInvalidTimingOrder,
          "enabled native cycle was not rejected by timing-order validation");

  static_cast<void>(analyze(Shape::kChain, kSeeds.front()));
  Database& malformed = STADM.getDatabase();
  const Arc& first = malformed.get_arc_list().front();
  auto& incoming = malformed.get_incoming_arc_list_map().at(first.get_sink_pin());
  const auto index = std::ranges::find(incoming, std::size_t{0});
  require(index != incoming.end(), "malformed adjacency witness is absent");
  incoming.erase(index);
  const NativeDatabaseValidationResult native_rejection
      = validateNativeDatabase(malformed);
  require(native_rejection
              && native_rejection->code
                     == NativeDatabaseValidationCode::kInvalidAdjacencyIndex,
          "malformed adjacency did not return typed native rejection");
  const ista::csr::CsrReferenceInputExportResult csr_rejection
      = ista::csr::exportCsrReferenceInputCandidate(
          malformed, editTestAnalysisContext(), kGraphSessionToken);
  const auto* csr_failure
      = std::get_if<ista::csr::CsrReferenceInputExportFailure>(&csr_rejection);
  require(csr_failure && csr_failure->code == ista::csr::CsrCaptureCode::kMalformedTopology,
          "malformed adjacency did not return typed CSR topology rejection");
}

}  // namespace

int main()
{
  PropertyRuntime runtime;
  testFixedSeedGraphFamiliesAndCycleRejection();
  return 0;
}
