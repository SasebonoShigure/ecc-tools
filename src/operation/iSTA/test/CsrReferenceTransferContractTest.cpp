// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include "CsrModelTestMutation.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Arc.hpp"
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "Database.hpp"
#include "Instance.hpp"
#include "Net.hpp"
#include "ParasiticNet.hpp"
#include "ParasiticNode.hpp"
#include "ParasiticResistor.hpp"
#include "Pin.hpp"
#include "TimingArc.hpp"
#include "TimingCell.hpp"
#include "TimingCellArc.hpp"
#include "TimingCellPort.hpp"
#include "TimingGraphCsrAdapter.hpp"
#include "TimingPoint.hpp"
#include "TimingPortConstraint.hpp"
#include "TimingTable.hpp"

void testSharedParasiticBindings();

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string hex(const std::vector<std::uint8_t>& bytes)
{
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (std::uint8_t value : bytes)
    stream << std::setw(2) << static_cast<unsigned>(value);
  return stream.str();
}

std::vector<std::uint8_t> hexBytes(std::string_view text)
{
  require(text.size() % 2 == 0, "odd raw KAT hex");
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    require(false, "non-lowercase raw KAT hex");
    return 0;
  };
  std::vector<std::uint8_t> result;
  for (std::size_t offset = 0; offset < text.size(); offset += 2)
    result.push_back(static_cast<std::uint8_t>((nibble(text[offset]) << 4) | nibble(text[offset + 1])));
  return result;
}

CsrReferenceInputRawProjection complexRawKatProjection()
{
  CsrReferenceInputRawProjection projection;
  RawInstance instance;
  instance.map_key = "u";
  instance.cell_name = "CELL";
  instance.sequential = true;
  instance.output_pin_name = "u:Q";
  instance.ordered_pin_names = {"u:X"};
  projection.instances.push_back(std::move(instance));
  projection.clock_constraints.push_back({"c", "e", {"ghost", "ghost"}, -2.0, 0.0, -1.0, false});

  RawLibraryArcChild child;
  child.library_name = "slow";
  child.index = 7;
  child.sense = RawTimingSense::kNegativeUnate;
  child.trigger_transition = RawTransition::kFall;
  child.check_transition = RawTransition::kRise;
  child.sdf_condition = "EN";
  child.time_scale = 2.0;
  child.capacitance_scale = 4.0;
  child.slew_derate = 0.5;
  child.slew_lower_thresholds = {0.125, 0.25};
  child.slew_upper_thresholds = {0.75, 0.875};
  child.input_thresholds = {0.25, 0.5};
  child.output_thresholds = {0.5, 0.75};
  child.delay_tables[0] = {true, RawTableVariable::kNone, RawTableVariable::kNone, {}, {}, {}};

  RawLibraryCellArc arc;
  arc.source_port = "A";
  arc.sink_port = "Y";
  arc.constant_delay = 0.25;
  arc.delay_min = 0.125;
  arc.delay_max = 0.5;
  arc.ordered_children.push_back(std::move(child));

  RawLibraryCellPayload payload;
  payload.sequential = true;
  payload.output_pin_name = "Y";
  payload.capacitance_unit = "PF";
  payload.time_unit = "NS";
  payload.slew_derate = 0.5;
  payload.slew_lower_thresholds = {0.125, 0.25};
  payload.slew_upper_thresholds = {0.75, 0.875};
  payload.input_thresholds = {0.25, 0.5};
  payload.output_thresholds = {0.5, 0.75};
  payload.cell_arcs.push_back(std::move(arc));
  projection.referenced_library_cells.push_back({"slow", "CELL", std::move(payload)});

  projection.parasitics
      = RawParasitics{"fixture.spef",
                      "PF",
                      "OHM",
                      {{"z_net", "z_emb", 1.5, {{"z2", "z_two", 0.25, 2.0, -2.0}, {"z1", "z_one", 0.125, 1.0, -1.0}}, {{"z2", "z1", 8.0}, {"z1", "z2", 4.0}}},
                       {"a_net", "a_emb", 0.0, {{"a2", "a_two", 0.0, 4.0, -4.0}, {"a1", "a_one", 0.0, 3.0, -3.0}}, {{"a2", "a1", 2.0}, {"a1", "a2", 1.0}}}}};
  projection.parasitics->input_port_slew_derate = 0.5;
  projection.parasitics->input_port_lower = {0.125, 0.25};
  projection.parasitics->input_port_upper = {0.75, 0.875};
  return projection;
}

constexpr std::string_view kComplexRawKatHex
    =         "0000000000000022697374612d6373722d7265666572656e63652d696e7075742d7261772f332e302e3000000000000000000000000000000001000000000000000175"
    "000000000000000443454c4c010000000000000000000000000000000000000000000000000003753a5100000000000000010000000000000003753a58000000000000"
    "000000000000000000000000000000000000010000000000000001630000000000000001650000000000000002000000000000000567686f7374000000000000000567"
    "686f737400c00000000000000000000000000000000000bff0000000000000000000000000000000000000000000000000000000000000000000000000000001000000"
    "0000000004736c6f77000000000000000443454c4c01010000000000000000000000000000000000000000000000000001590000000000000002504600000000000000"
    "024e53003fe0000000000000003fc0000000000000003fd0000000000000003fe8000000000000003fec000000000000003fd0000000000000003fe000000000000000"
    "3fe0000000000000003fe800000000000000000000000000000000000000000001000000000000000141000000000000000159003fd0000000000000003fc000000000"
    "0000003fe000000000000000000000000000000000010000000000000004736c6f77000000070201000000000000000002454e00400000000000000000401000000000"
    "0000003fe0000000000000003fc0000000000000003fd0000000000000003fe8000000000000003fec000000000000003fd0000000000000003fe0000000000000003f"
    "e0000000000000003fe80000000000000100000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000000c6669"
    "78747572652e737065660000000000000002504600000000000000034f484d003fe0000000000000003fc0000000000000003fd0000000000000003fe8000000000000"
    "003fec000000000000000000000000000200000000000000057a5f6e657400000000000000057a5f656d62003ff8000000000000000000000000000200000000000000"
    "027a3200000000000000057a5f74776f003fd000000000000000400000000000000000c00000000000000000000000000000027a3100000000000000057a5f6f6e6500"
    "3fc0000000000000003ff000000000000000bff0000000000000000000000000000200000000000000027a3200000000000000027a3100402000000000000000000000"
    "000000027a3100000000000000027a320040100000000000000000000000000005615f6e65740000000000000005615f656d6200000000000000000000000000000000"
    "02000000000000000261320000000000000005615f74776f00000000000000000000401000000000000000c01000000000000000000000000000026131000000000000"
    "0005615f6f6e6500000000000000000000400800000000000000c008000000000000000000000000000200000000000000026132000000000000000261310040000000"
    "000000000000000000000002613100000000000000026132003ff0000000000000";
class Reader
{
 public:
  explicit Reader(const std::vector<std::uint8_t>& bytes) : _bytes(bytes) {}
  std::uint8_t u8()
  {
    require(_offset < _bytes.size(), "raw KAT reader overrun");
    return _bytes[_offset++];
  }
  std::uint64_t u64()
  {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
      value = (value << 8) | u8();
    return value;
  }
  std::int32_t i32()
  {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index)
      value = (value << 8) | u8();
    return std::bit_cast<std::int32_t>(value);
  }
  double finite()
  {
    require(u8() == 0, "raw KAT finite tag");
    return std::bit_cast<double>(u64());
  }
  std::string string()
  {
    const std::size_t size = static_cast<std::size_t>(u64());
    require(size <= _bytes.size() - _offset, "raw KAT string overrun");
    std::string value(_bytes.begin() + static_cast<std::ptrdiff_t>(_offset), _bytes.begin() + static_cast<std::ptrdiff_t>(_offset + size));
    _offset += size;
    return value;
  }
  bool done() const { return _offset == _bytes.size(); }

 private:
  const std::vector<std::uint8_t>& _bytes;
  std::size_t _offset{0};
};

void decodeComplexRawLiteral(const std::vector<std::uint8_t>& bytes)
{
  Reader reader(bytes);
  const auto count = [&](std::uint64_t expected, const char* field) { require(reader.u64() == expected, std::string{"raw KAT count: "} + field); };
  require(reader.string() == "ista-csr-reference-input-raw/3.0.0", "raw KAT domain");
  count(0, "nodes");
  count(1, "instances");
  require(reader.string() == "u" && reader.string() == "CELL", "raw KAT instance names");
  require(reader.u8() == 1 && reader.u8() == 0 && reader.u8() == 0, "raw KAT instance roles");
  require(reader.string().empty() && reader.string().empty() && reader.string() == "u:Q", "raw KAT instance pins");
  count(1, "ordered instance pins");
  require(reader.string() == "u:X" && reader.u8() == 0, "raw KAT missing selected C2Q");
  count(0, "arcs");
  count(0, "nets");
  count(1, "clocks");
  require(reader.string() == "c" && reader.string() == "e", "raw KAT clock identity");
  count(2, "clock sources");
  require(reader.string() == "ghost" && reader.string() == "ghost", "raw KAT duplicate unresolved sources");
  require(reader.finite() == -2.0 && reader.finite() == 0.0 && reader.finite() == -1.0 && reader.u8() == 0, "raw KAT negative/zero clock values");
  count(0, "ports");
  count(0, "cases");
  count(0, "checks");
  count(1, "libraries");
  require(reader.string() == "slow" && reader.string() == "CELL" && reader.u8() == 1, "raw KAT library identity");
  require(reader.u8() == 1 && reader.u8() == 0 && reader.u8() == 0, "raw KAT cell roles");
  require(reader.string().empty() && reader.string().empty() && reader.string() == "Y", "raw KAT cell role pins");
  require(reader.string() == "PF" && reader.string() == "NS", "raw KAT library units");
  require(reader.finite() == 0.5, "raw KAT cell derate");
  for (double expected : {0.125, 0.25, 0.75, 0.875, 0.25, 0.5, 0.5, 0.75})
    require(reader.finite() == expected, "raw KAT cell threshold");
  count(0, "library ports");
  count(1, "cell arcs");
  require(reader.string() == "A" && reader.string() == "Y", "raw KAT transfer ports");
  require(reader.finite() == 0.25 && reader.finite() == 0.125 && reader.finite() == 0.5, "raw KAT transfer delays");
  require(reader.u8() == 0 && reader.u8() == 0 && reader.u8() == 0, "raw KAT transfer flags");
  count(1, "children");
  require(reader.string() == "slow" && reader.i32() == 7, "raw KAT child identity");
  require(reader.u8() == 2 && reader.u8() == 1 && reader.u8() == 0 && reader.string() == "EN", "raw KAT child semantics");
  require(reader.finite() == 2.0 && reader.finite() == 4.0 && reader.finite() == 0.5, "raw KAT child scales");
  for (double expected : {0.125, 0.25, 0.75, 0.875, 0.25, 0.5, 0.5, 0.75})
    require(reader.finite() == expected, "raw KAT child threshold");
  require(reader.u8() == 1 && reader.u8() == 0 && reader.u8() == 0, "raw KAT present-empty table signature");
  count(0, "table axis1");
  count(0, "table axis2");
  count(0, "table values");
  for (int table = 0; table < 5; ++table)
    require(reader.u8() == 0, "raw KAT missing table");
  count(0, "library checks");
  require(reader.u8() == 1, "raw KAT parasitic presence");
  require(reader.string() == "fixture.spef" && reader.string() == "PF" && reader.string() == "OHM", "raw KAT parasitic header");
  for (double expected : {0.5, 0.125, 0.25, 0.75, 0.875})
    require(reader.finite() == expected, "raw KAT input-port threshold");
  count(2, "parasitic nets");
  const auto net = [&](std::string_view key, std::string_view embedded, double total, std::array<std::string_view, 2> node_keys,
                       std::array<std::string_view, 2> node_names, std::array<double, 6> node_values, std::array<std::string_view, 4> resistor_ends,
                       std::array<double, 2> resistances) {
    require(reader.string() == key && reader.string() == embedded && reader.finite() == total, "raw KAT parasitic net identity");
    count(2, "parasitic nodes");
    for (std::size_t index = 0; index < 2; ++index) {
      require(reader.string() == node_keys[index] && reader.string() == node_names[index], "raw KAT parasitic node order");
      for (std::size_t value = 0; value < 3; ++value)
        require(reader.finite() == node_values[index * 3 + value], "raw KAT parasitic node value");
    }
    count(2, "parasitic resistors");
    for (std::size_t index = 0; index < 2; ++index)
      require(reader.string() == resistor_ends[index * 2] && reader.string() == resistor_ends[index * 2 + 1] && reader.finite() == resistances[index],
              "raw KAT parasitic resistor order");
  };
  net("z_net", "z_emb", 1.5, {"z2", "z1"}, {"z_two", "z_one"}, {0.25, 2.0, -2.0, 0.125, 1.0, -1.0}, {"z2", "z1", "z1", "z2"}, {8.0, 4.0});
  net("a_net", "a_emb", 0.0, {"a2", "a1"}, {"a_two", "a_one"}, {0.0, 4.0, -4.0, 0.0, 3.0, -3.0}, {"a2", "a1", "a1", "a2"}, {2.0, 1.0});
  require(reader.done(), "raw KAT trailing bytes");
}

void testRawKatsAndMutations()
{
  const CsrReferenceInputRawProjection empty;
  const auto empty_bytes = canonicalIndependentReferenceInputRawBytes(empty);
  constexpr std::string_view kEmptyHex
      =     "0000000000000022697374612d6373722d7265666572656e63652d696e7075742d7261772f332e302e3000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
  require(empty_bytes.size() == 115 && hex(empty_bytes) == kEmptyHex, "empty raw KAT");

  const auto literal = hexBytes(kComplexRawKatHex);
  const auto complex = complexRawKatProjection();
  require(literal.size() == 1172, "complex raw KAT byte count");
  require(canonicalIndependentReferenceInputRawBytes(complex) == literal, "complex raw KAT literal mismatch");
  decodeComplexRawLiteral(literal);

  const auto baseline = canonicalIndependentReferenceInputRawBytes(complex);
  const auto changed = [&](const char* field, auto mutate) {
    auto value = complex;
    mutate(value);
    require(canonicalIndependentReferenceInputRawBytes(value) != baseline, std::string{"raw field omitted: "} + field);
  };
  changed("instance map key", [](auto& value) { value.instances[0].map_key += "x"; });
  changed("instance cell", [](auto& value) { value.instances[0].cell_name += "x"; });
  changed("instance roles", [](auto& value) { value.instances[0].gating = true; });
  changed("instance role pins", [](auto& value) { value.instances[0].clock_pin_name = "u:CK"; });
  changed("instance pin order", [](auto& value) { value.instances[0].ordered_pin_names.push_back("u:Y"); });
  changed("selected C2Q presence", [](auto& value) { value.instances[0].selected_clock_arc = RawSelectedClockArc{"u:X", "u:Q", {RawTransition::kFall}}; });
  changed("clock map/embedded", [](auto& value) { value.clock_constraints[0].embedded_name += "x"; });
  changed("clock ordered duplicate", [](auto& value) {
    std::ranges::reverse(value.clock_constraints[0].ordered_sources);
    value.clock_constraints[0].ordered_sources[0] += "x";
  });
  changed("clock period", [](auto& value) { value.clock_constraints[0].period = -3.0; });
  changed("clock rise", [](auto& value) { value.clock_constraints[0].rise_edge = 0.25; });
  changed("clock fall", [](auto& value) { value.clock_constraints[0].fall_edge = -0.5; });
  changed("clock propagated", [](auto& value) { value.clock_constraints[0].propagated = true; });
  changed("library identity", [](auto& value) { value.referenced_library_cells[0].library_name += "x"; });
  changed("cell roles", [](auto& value) { value.referenced_library_cells[0].payload->macro = true; });
  changed("cell role pins", [](auto& value) { value.referenced_library_cells[0].payload->data_pin_name = "D"; });
  changed("capacitance unit", [](auto& value) { value.referenced_library_cells[0].payload->capacitance_unit = "FF"; });
  changed("time unit", [](auto& value) { value.referenced_library_cells[0].payload->time_unit = "PS"; });
  changed("cell derate", [](auto& value) { value.referenced_library_cells[0].payload->slew_derate = 0.75; });
  changed("cell four thresholds", [](auto& value) {
    auto& payload = *value.referenced_library_cells[0].payload;
    payload.slew_lower_thresholds[0] = 0.1;
    payload.slew_upper_thresholds[1] = 0.9;
    payload.input_thresholds[0] = 0.4;
    payload.output_thresholds[1] = 0.8;
  });
  changed("transfer delays", [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].delay_max = 0.75; });
  changed("transfer flags", [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].is_clear_or_preset = true; });
  changed("child library/index", [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].index = 8; });
  changed("child sense", [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].sense = RawTimingSense::kNonUnate; });
  changed("child trigger",
          [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].trigger_transition = RawTransition::kRise; });
  changed("child check",
          [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].check_transition = RawTransition::kFall; });
  changed("child condition", [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].sdf_condition += "X"; });
  changed("child scales", [](auto& value) {
    auto& child = value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0];
    child.time_scale = 3.0;
    child.capacitance_scale = 5.0;
    child.slew_derate = 0.75;
  });
  changed("child four thresholds", [](auto& value) {
    auto& child = value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0];
    child.slew_lower_thresholds[1] = 0.3;
    child.slew_upper_thresholds[0] = 0.8;
    child.input_thresholds[1] = 0.6;
    child.output_thresholds[0] = 0.6;
  });
  changed("table presence", [](auto& value) { value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].delay_tables[0].present = false; });
  changed("table variables", [](auto& value) {
    auto& table = value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].delay_tables[0];
    table.variable1 = RawTableVariable::kInputTransition;
    table.variable2 = RawTableVariable::kOutputCapacitance;
  });
  changed("table axes/values", [](auto& value) {
    auto& table = value.referenced_library_cells[0].payload->cell_arcs[0].ordered_children[0].delay_tables[0];
    table.axis1 = {0.1};
    table.axis2 = {0.2};
    table.values = {0.3};
  });
  changed("parasitic header units", [](auto& value) { value.parasitics->resistance_unit = "KOHM"; });
  changed("input-port derate", [](auto& value) { value.parasitics->input_port_slew_derate = 0.75; });
  for (std::size_t transition = 0; transition < 2; ++transition) {
    changed("input-port lower", [&](auto& value) { value.parasitics->input_port_lower[transition] += 0.01; });
    changed("input-port upper", [&](auto& value) { value.parasitics->input_port_upper[transition] += 0.01; });
  }
  changed("parasitic net identity/cap", [](auto& value) { value.parasitics->nets[0].total_capacitance = 2.0; });
  changed("parasitic net order", [](auto& value) { std::ranges::reverse(value.parasitics->nets); });
  changed("parasitic node fields", [](auto& value) {
    auto& node = value.parasitics->nets[0].nodes[0];
    node.map_key += "x";
    node.embedded_name += "x";
    node.capacitance = 0.5;
    node.x = 5.0;
    node.y = -5.0;
  });
  changed("parasitic node order", [](auto& value) { std::ranges::reverse(value.parasitics->nets[0].nodes); });
  changed("parasitic resistor fields", [](auto& value) {
    auto& resistor = value.parasitics->nets[0].resistors[0];
    resistor.source += "x";
    resistor.sink += "x";
    resistor.resistance = 9.0;
  });
  changed("parasitic resistor order", [](auto& value) { std::ranges::reverse(value.parasitics->nets[0].resistors); });
}

CsrReferenceInput exportInput(CsrReferenceInputNativeFixture& fixture, std::uint64_t token, StaAnalysisContext context = referenceInputTestContext())
{
  auto result = exportCsrReferenceInputCandidate(fixture.database(), context, token);
  if (const auto* failure = std::get_if<CsrReferenceInputExportFailure>(&result); failure != nullptr)
    require(false,
            "native transfer export failed at token " + std::to_string(token) + " with code " + std::to_string(static_cast<std::uint8_t>(failure->code)));
  CsrReferenceInput input = std::get<CsrReferenceInput>(std::move(result));
  require(validateReferenceInputForSession(input, token).empty(), "native transfer export is not strict-valid");
  return input;
}

CellTransferModel& c2qGraphTransfer(CsrReferenceInput& input)
{
  auto found = std::ranges::find(input.arcs, "CK", [](const ArcRecord& arc) { return arc.key.base.library_source_port; });
  require(found != input.arcs.end(), "missing graph C2Q transfer");
  return found->transfer;
}

template <typename Input>
auto& c2qLibraryTransfer(Input& input)
{
  require(!input.referenced_library_cells.empty(), "missing library closure");
  auto& arcs = input.referenced_library_cells[0].cell_arcs;
  auto found = std::ranges::find(arcs, "CK", &LibraryCellArcModel::source_port);
  require(found != arcs.end(), "missing library C2Q transfer");
  return found->transfer;
}

std::uint8_t changedMask(const CsrReferenceInput& before, const CsrReferenceInput& after)
{
  const auto lhs = makeReferenceInputIdentity(before, 1, 0);
  const auto rhs = makeReferenceInputIdentity(after, 1, 0);
  std::uint8_t mask = 0;
  mask |= lhs.topology_fingerprint != rhs.topology_fingerprint ? 1 : 0;
  mask |= lhs.clock_constraint_fingerprint != rhs.clock_constraint_fingerprint ? 2 : 0;
  mask |= lhs.library_fingerprint != rhs.library_fingerprint ? 4 : 0;
  mask |= lhs.corner_fingerprint != rhs.corner_fingerprint ? 8 : 0;
  mask |= lhs.parasitic_fingerprint != rhs.parasitic_fingerprint ? 16 : 0;
  mask |= lhs.timing_input_fingerprint != rhs.timing_input_fingerprint ? 32 : 0;
  return mask;
}

void testTransfersTablesAndChangedSets()
{
  auto fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  CsrReferenceInput baseline = exportInput(fixture, 21);
  auto& graph = c2qGraphTransfer(baseline);
  auto& library = c2qLibraryTransfer(baseline);
  require(graph == library, "graph/library C2Q transfer mismatch");
  require(graph.delay == 0.12 && graph.delay_min == 0.08 && graph.delay_max == 0.18 && graph.is_clock_arc, "C2Q scalar transfer mismatch");
  require(graph.children().size() == 1, "C2Q child count");
  const auto& child = graph.children()[0];
  require(child.signature.library_name == "slow.lib" && child.signature.arc_index == 11 && child.signature.sense == TimingSense::kPositiveUnate
              && child.signature.trigger_transition == Transition::kFall && child.signature.check_transition == Transition::kFall
              && child.signature.sdf_condition == "ENABLE",
          "C2Q child semantic fields");
  require(child.time_unit_scale == 1.0 && child.capacitance_unit_scale == 1.0 && child.slew_derate == 0.9, "C2Q child scales/derate");
  require(child.slew_lower_threshold_pct == std::array{0.3, 0.3} && child.slew_upper_threshold_pct == std::array{0.7, 0.7}
              && child.input_threshold_pct == std::array{0.5, 0.5} && child.output_threshold_pct == std::array{0.5, 0.5},
          "C2Q four threshold families");
  require(child.delay_tables[0].present && child.delay_tables[0].axes.empty() && child.delay_tables[0].values.empty(), "present-empty table collapsed");
  require(!child.delay_tables[1].present, "missing table fabricated");
  require(child.slew_tables[1].present && child.slew_tables[1].axes.empty() && child.slew_tables[1].values == std::vector{0.07}, "0D table mismatch");
  const auto& data_table = baseline.referenced_library_cells[0].cell_arcs[1].transfer.children()[0].delay_tables[0];
  require(data_table.present && data_table.axes.size() == 2 && data_table.values.size() == 4, "2D table mismatch");
  const auto& check = baseline.referenced_library_cells[0].timing_checks[0];
  require(check.kind == TimingCheckKind::kSetup && check.check_time == 0.15 && check.clock_transition == Transition::kRise, "timing check scalar mismatch");
  require(check.children()[0].check_tables[0].present && check.children()[0].check_tables[0].axes.size() == 1, "1D timing-check table mismatch");
  for (const auto& arc : baseline.arcs)
    if (arc.kind == ArcKind::kNet)
      require(arc.transfer == [] { CellTransferModel model; model.delay = model.delay_min = model.delay_max = 0.0; return model; }(), "net transfer is not exact zero");

  const auto verify = [&](std::uint8_t expected, const char* name, auto mutate) {
    CsrReferenceInput changed = baseline;
    mutate(changed);
    require(changedMask(baseline, changed) == expected, std::string{"wrong component changed-set: "} + name);
  };
  verify(4 | 32, "constant/min/max", [](auto& input) {
    c2qGraphTransfer(input).delay = c2qLibraryTransfer(input).delay = 0.2;
    c2qGraphTransfer(input).delay_min = c2qLibraryTransfer(input).delay_min = 0.1;
    c2qGraphTransfer(input).delay_max = c2qLibraryTransfer(input).delay_max = 0.3;
  });
  verify(4 | 32, "clear/preset", [](auto& input) { c2qGraphTransfer(input).is_clear_or_preset = c2qLibraryTransfer(input).is_clear_or_preset = true; });
  verify(4 | 32, "scales/derate/thresholds", [](auto& input) {
    for (CellTransferModel* transfer : {&c2qGraphTransfer(input), &c2qLibraryTransfer(input)}) {
      ista::csr::test::mutateChildren(*transfer, [&](auto& children) {
      auto& value = children[0];
      value.time_unit_scale = 2.0;
      value.capacitance_unit_scale = 3.0;
      value.slew_derate = 0.8;
      value.slew_lower_threshold_pct[0] = 0.2;
      value.slew_upper_threshold_pct[1] = 0.8;
      value.input_threshold_pct[0] = 0.4;
      value.output_threshold_pct[1] = 0.6;
    });
    }
  });
  verify(4 | 32, "load-cell thresholds", [](auto& input) {
    auto& cell = input.referenced_library_cells[0];
    cell.slew_derate = 0.6;
    cell.slew_lower_threshold_pct[0] = 0.2;
    cell.slew_upper_threshold_pct[1] = 0.8;
    cell.input_threshold_pct[0] = 0.4;
  });
  verify(4 | 32, "table axes/values", [](auto& input) {
    for (CellTransferModel* transfer : {&c2qGraphTransfer(input), &c2qLibraryTransfer(input)}) {
      ista::csr::test::mutateChildren(*transfer, [&](auto& children) {
      auto& table = children[0].delay_tables[0];
      table.axes = {{0.1}};
      table.values = {0.2};
    });
    }
  });
  verify(1 | 4 | 32, "signature sense/trigger/check/condition/table", [](auto& input) {
    auto mutate = [](LibraryArcSignature& value) {
      value.library_name = "fast.lib";
      value.arc_index = 99;
      value.sense = TimingSense::kNegativeUnate;
      value.trigger_transition = Transition::kRise;
      value.check_transition = Transition::kRise;
      value.sdf_condition = "ALT";
      value.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kNone};
    };
    ista::csr::test::mutateChildren(c2qGraphTransfer(input), [&](auto& children) { mutate(children[0].signature); });
    ista::csr::test::mutateChildren(c2qLibraryTransfer(input), [&](auto& children) { mutate(children[0].signature); });
    for (auto& arc : input.arcs)
      if (arc.key.base.library_source_port == "CK")
        mutate(arc.key.base.library_arcs[0]);
    for (auto& entry : input.arc_ids_by_key)
      if (entry.key.base.library_source_port == "CK")
        mutate(entry.key.base.library_arcs[0]);
  });
  verify(4 | 32, "timing check kind/time/transition/child", [](auto& input) {
    auto& value = input.referenced_library_cells[0].timing_checks[0];
    value.kind = TimingCheckKind::kHold;
    value.check_time = 0.25;
    value.clock_transition = Transition::kFall;
    ista::csr::test::mutateChildren(value, [&](auto& children) { children[0].check_tables[0].values[0] = 0.25; });
  });
  verify(32, "native Liberty arc identity", [](auto& input) {
    for (auto& arc : input.arcs)
      if (arc.native_cell_arc_ordinal) { ++*arc.native_cell_arc_ordinal; break; }
  });
  verify(8 | 32, "corner", [](auto& input) { input.analysis_context.liberty_corner += "x"; });

  auto owner_fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  auto& native_cells = owner_fixture.database().get_timing_library().get_cell_map();
  auto other = native_cells.at("DFF_X1");
  other.set_cell_name("OTHER_DFF");
  native_cells.emplace("OTHER_DFF", std::move(other));
  detail::NativeLibraryModels model_store(owner_fixture.database());
  static_cast<void>(model_store.cell("OTHER_DFF"));
  const auto& native_arc = native_cells.at("DFF_X1").get_cell_arc_list().front();
  require(model_store.arcOrdinal("DFF_X1", native_arc) == 0, "native Liberty ordinal lost");
  bool wrong_owner_rejected = false;
  try {
    static_cast<void>(model_store.arcOrdinal("OTHER_DFF", native_arc));
  } catch (const std::exception&) {
    wrong_owner_rejected = true;
  }
  require(wrong_owner_rejected, "equal model in another cell stole native arc ownership");

  auto threshold_fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  auto& native_load = threshold_fixture.database().get_timing_library().get_cell_map().at("DFF_X1");
  native_load.set_input_threshold_pct_rise(35.0);
  native_load.set_slew_lower_threshold_pct_fall(0.1);
  native_load.set_slew_upper_threshold_pct_fall(90.0);
  native_load.set_slew_derate_from_library(0.6);
  const auto threshold_input = exportInput(threshold_fixture, 23);
  const auto& load_cell = threshold_input.referenced_library_cells[0];
  require(load_cell.input_threshold_pct[0] == 35.0 * 0.01 && load_cell.slew_lower_threshold_pct[1] == 0.1
              && load_cell.slew_upper_threshold_pct[1] == 0.9 && load_cell.slew_derate == 0.6,
          "load-cell threshold export differs from native normalized values");
  for (int fault = 0; fault < 3; ++fault) {
    auto bad = threshold_input;
    auto& cell = bad.referenced_library_cells[0];
    if (fault == 0) cell.input_threshold_pct[0] = std::numeric_limits<double>::quiet_NaN();
    if (fault == 1) cell.slew_lower_threshold_pct[1] = -0.1;
    if (fault == 2) cell.slew_derate = std::numeric_limits<double>::infinity();
    require(!validateReferenceInputIntrinsic(bad).empty(), "invalid load-cell threshold accepted");
  }
  auto empty_child_fixture = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  empty_child_fixture.database().get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list()[0].get_timing_arc_list().clear();
  const CsrReferenceInput empty_child = exportInput(empty_child_fixture, 22);
  require(c2qLibraryTransfer(empty_child).children().empty() && c2qLibraryTransfer(empty_child).delay == 0.12,
          "empty child did not preserve transfer fallback scalars");
}

ista::Pin nativePin(std::string full, std::string local, std::string instance, ista::PinDirection direction)
{
  ista::Pin pin;
  pin.set_full_name(full);
  pin.set_pin_name(local);
  pin.set_instance_name(instance);
  pin.set_direction(direction);
  return pin;
}

ista::TimingCellPort nativePort(std::string name, bool input, bool output)
{
  ista::TimingCellPort port;
  port.set_port_name(name);
  port.set_is_input(input);
  port.set_is_output(output);
  return port;
}

CsrReferenceInputNativeFixture fallbackFixture(bool present_cell)
{
  CsrReferenceInputNativeFixture fixture;
  auto& database = fixture.database();
  database.get_pin_map().emplace("u:A", nativePin("u:A", "A", "u", ista::PinDirection::kInput));
  database.get_pin_map().emplace("u:Y", nativePin("u:Y", "Y", "u", ista::PinDirection::kOutput));
  database.get_timing_point_map().emplace("u:A", ista::TimingPoint{});
  database.get_timing_point_map().emplace("u:Y", ista::TimingPoint{});
  ista::Instance instance;
  instance.set_instance_name("u");
  instance.set_cell_name(present_cell ? "EMPTY" : "MISSING");
  instance.set_pin_name_list({"u:A", "u:Y"});
  if (present_cell)
    instance.set_output_pin_name("u:Y");
  database.get_instance_map().emplace("u", std::move(instance));
  if (present_cell) {
    ista::TimingCell cell;
    cell.set_cell_name("EMPTY");
    cell.set_library_name("slow.lib");
    cell.get_port_map().emplace("A", nativePort("A", true, false));
    cell.get_port_map().emplace("Y", nativePort("Y", false, true));
    database.get_timing_library().get_cell_map().emplace("EMPTY", std::move(cell));
  }
  ista::Arc arc;
  arc.set_source_pin("u:A");
  arc.set_sink_pin("u:Y");
  arc.set_owner_name("u");
  arc.set_type(ista::ArcType::kCell);
  arc.set_timing_cell_arc(nullptr);
  database.get_arc_list().push_back(std::move(arc));
  database.get_outgoing_arc_list_map()["u:A"] = {0};
  database.get_incoming_arc_list_map()["u:Y"] = {0};
  database.get_start_point_list() = {"u:A"};
  database.get_end_point_list() = {"u:Y"};
  return fixture;
}

void addParasitic(ista::Database& database, bool with_net)
{
  auto& library = database.get_parasitic_library();
  library.set_spef_file_path("ordered.spef");
  library.set_capacitive_unit("PF");
  library.set_resistance_unit("OHM");
  if (!with_net)
    return;
  ista::ParasiticNode z2;
  z2.set_node_name("z_two");
  z2.set_capacitance(0.2);
  z2.set_x(2.0);
  z2.set_y(-2.0);
  ista::ParasiticNode z1;
  z1.set_node_name("z_one");
  z1.set_capacitance(0.1);
  ista::ParasiticResistor first;
  first.set_source_node("z2");
  first.set_sink_node("z1");
  first.set_resistance(8.0);
  ista::ParasiticResistor second;
  second.set_source_node("z1");
  second.set_sink_node("z2");
  second.set_resistance(4.0);
  ista::ParasiticNet net;
  net.set_net_name("z_emb");
  net.set_lumped_capacitance(0.3);
  net.get_node_map().emplace("z2", std::move(z2));
  net.get_node_map().emplace("z1", std::move(z1));
  net.get_resistor_list() = {first, second};
  library.get_net_map().emplace("z_net", std::move(net));
}

bool allLoads(const CsrReferenceInput& input, bool present)
{
  return std::ranges::all_of(input.arcs, [&](const ArcRecord& arc) { return arc.output_load.has_value() == present; });
}

void testFallbackParasiticsAndLoads()
{
  for (bool present : {false, true}) {
    auto fixture = fallbackFixture(present);
    CsrReferenceInput input = exportInput(fixture, present ? 31 : 30);
    require(input.referenced_library_cells[0].present == present, "missing/present-empty cell closure");
    require(input.arcs[0].transfer == CellTransferModel{}, "unbound/no-cell-arcs unit fallback");
  }

  auto plain = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  require(allLoads(exportInput(plain, 32), true), "nonparasitic loads are not all-present");
  auto metadata = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  addParasitic(metadata.database(), false);
  require(allLoads(exportInput(metadata, 33), true), "empty parasitic metadata removed loads");
  auto parasitic = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  addParasitic(parasitic.database(), true);
  StaAnalysisContext context = referenceInputTestContext();
  context.rc_corner = "rc";
  CsrReferenceInput with_net = exportInput(parasitic, 34, context);
  require(allLoads(with_net, true), "unrelated RC net removed no-RC loads");
  require(with_net.parasitics.nets[0].nodes[0].map_key == "z1" && with_net.parasitics.nets[0].nodes[1].map_key == "z2"
              && with_net.parasitics.nets[0].resistors[0].source == "z2" && with_net.parasitics.nets[0].resistors[1].source == "z1",
          "ordered parasitic nodes/resistors changed");
  CsrReferenceInput changed = with_net;
  changed.parasitics.nets[0].lumped_capacitance += 1.0;
  require(changedMask(with_net, changed) == (16 | 32), "parasitic changed-set");
  const auto raw_before = canonicalIndependentReferenceInputRawBytes(projectCsrReferenceInputRaw(parasitic.database()));
  parasitic.database().get_parasitic_library().set_spef_file_path("renamed.spef");
  const auto raw_after = canonicalIndependentReferenceInputRawBytes(projectCsrReferenceInputRaw(parasitic.database()));
  CsrReferenceInput renamed = exportInput(parasitic, 34, context);
  require(raw_before != raw_after && changedMask(with_net, renamed) == 0, "SPEF source-path exclusion boundary");

  auto diamond = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kDiamond);
  const auto zero = exportInput(diamond, 35);
  require(allLoads(zero, true), "true-zero load presence");
  for (const auto& arc : zero.arcs)
    for (const auto& row : *arc.output_load)
      for (double value : row)
        require(value == 0.0, "true-zero load value");
  addParasitic(diamond.database(), true);
  require(allLoads(exportInput(diamond, 36, context), true), "unrelated RC removed true-zero loads");
  const std::string output_net = "out_net";
  auto rc = parasitic.database().get_parasitic_library().get_net_map().extract("z_net");
  rc.key() = output_net;
  rc.mapped().set_net_name(output_net);
  parasitic.database().get_parasitic_library().get_net_map().insert(std::move(rc));
  const auto mixed = exportInput(parasitic, 37, context);
  bool rc_seen = false, plain_seen = false;
  for (const auto& arc : mixed.arcs) {
    const bool has_rc = mixed.nodes[arc.sink.value()].net_name == output_net;
    require(arc.output_load.has_value() != has_rc, "load presence did not follow its net");
    rc_seen |= has_rc;
    plain_seen |= !has_rc;
  }
  require(rc_seen && plain_seen, "mixed RC fixture did not exercise both paths");
  auto wrong_value = mixed;
  const auto with_load = std::ranges::find_if(wrong_value.arcs, [](const ArcRecord& arc) { return arc.output_load.has_value(); });
  (*with_load->output_load)[0][0] += 1.0;
  const auto value_errors = validateReferenceInputIntrinsic(wrong_value);
  require(std::ranges::any_of(value_errors, [](const CsrContractViolation& error) { return error.field == "arcs.output_load"; }),
          "unrelated incorrect load accepted in a mixed RC design");
  auto wrong = mixed;
  const auto plain_arc = std::ranges::find_if(wrong.arcs, [](const ArcRecord& arc) { return arc.output_load.has_value(); });
  plain_arc->output_load.reset();
  const auto errors = validateReferenceInputIntrinsic(wrong);
  require(std::ranges::any_of(errors, [](const CsrContractViolation& error) { return error.field == "arcs.output_load"; }),
          "unrelated absent load accepted");
}

void setScaleAndLoad(CsrReferenceInputNativeFixture& fixture, double scale, double load)
{
  auto& database = fixture.database();
  database.get_timing_library().get_cell_map().at("DFF_X1").get_cell_arc_list()[0].get_timing_arc_list()[0].set_cap_unit_scale(scale);
  auto& constraint = database.get_timing_constraint().get_port_constraint_map().at("out");
  constraint.set_load(load);
  constraint.set_has_load(true);
}

void requireNumericFailure(CsrReferenceInputNativeFixture& fixture, const char* field)
{
  const auto result = exportCsrReferenceInputCandidate(fixture.database(), referenceInputTestContext(), 40);
  require(std::holds_alternative<CsrReferenceInputExportFailure>(result)
              && std::get<CsrReferenceInputExportFailure>(result).code == CsrCaptureCode::kIllegalNumeric,
          std::string{"missing illegal-numeric boundary: "} + field);
}

void testScaleBoundaries()
{
  constexpr double kScaleEpsilon = 1.0e-6;
  const double maximum = static_cast<double>(std::numeric_limits<int>::max());
  const double rounded_below = 1.0 + kScaleEpsilon;
  require(std::abs(rounded_below - 1.0) < kScaleEpsilon, "rounded scale must stay below threshold");
  auto below = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  setScaleAndLoad(below, rounded_below, maximum);
  static_cast<void>(exportInput(below, 41));
  const double first_at_or_above = std::nextafter(rounded_below, std::numeric_limits<double>::infinity());
  require(std::abs(first_at_or_above - 1.0) >= kScaleEpsilon, "next scale must enter conversion branch");
  auto exact = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  setScaleAndLoad(exact, first_at_or_above, maximum);
  requireNumericFailure(exact, "first binary64 conversion entry");
  auto ceiling_ok = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  setScaleAndLoad(ceiling_ok, 2.0, maximum / 2.0);
  static_cast<void>(exportInput(ceiling_ok, 42));

  auto ceiling_overflow = makeReferenceNativeFixture(ReferenceNativeFixtureCase::kChain);
  setScaleAndLoad(ceiling_overflow, 2.0, maximum / 2.0 + 0.25);
  requireNumericFailure(ceiling_overflow, "ceil INT_MAX overflow");
}

}  // namespace

int main()
{
  testRawKatsAndMutations();
  testTransfersTablesAndChangedSets();
  testFallbackParasiticsAndLoads();
  testSharedParasiticBindings();
  testScaleBoundaries();
  return 0;
}
