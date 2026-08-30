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
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "CsrGraphEncoding.hpp"
#include "CsrGraphValidator.hpp"
#include "CsrNativeGraphFixture.hpp"
#include "CsrNativeGraphTestSupport.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

const CsrGraphSnapshot& requireSnapshot(const CsrGraphExportResult& result)
{
  const auto* snapshot = std::get_if<CsrGraphSnapshot>(&result);
  require(snapshot != nullptr, "export returned a typed failure");
  return *snapshot;
}

const NativeGraphProjection& requireProjection(const NativeGraphProjectionResult& result)
{
  const auto* projection = std::get_if<NativeGraphProjection>(&result);
  require(projection != nullptr, "decode returned a typed failure");
  return *projection;
}

std::string replayGraph(std::uint64_t seed, const NativeGraphProjection& projection)
{
  std::ostringstream stream;
  stream << "seed=" << seed << " nodes=[";
  for (const auto& node : projection.nodes) {
    stream << node.key.full_name << ',';
  }
  stream << "] arcs=[";
  for (const auto& arc : projection.arcs) {
    stream << arc.key.base.source.full_name << "->" << arc.key.base.sink.full_name << "#" << arc.key.parallel_ordinal << "@" << arc.native_traversal_rank
           << ":d" << arc.is_disabled << ":l" << arc.is_loop_disabled << ',';
  }
  stream << "] out_rows=[";
  for (const auto& row : projection.base_out) {
    stream << row.node.full_name << ':';
    for (const auto& arc : row.ordered_arcs) {
      stream << arc.parallel_ordinal << ',';
    }
    stream << ';';
  }
  stream << ']';
  return stream.str();
}

void verifyTranspose(const CsrGraphSnapshot& snapshot)
{
  require(snapshot.out_row_offsets.size() == snapshot.nodes.size() + 1, "out offset shape is wrong");
  require(snapshot.in_row_offsets.size() == snapshot.nodes.size() + 1, "in offset shape is wrong");
  std::vector<std::size_t> out_occurrences(snapshot.arcs.size(), 0);
  std::vector<std::size_t> in_occurrences(snapshot.arcs.size(), 0);
  for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) {
    for (std::uint64_t position = snapshot.out_row_offsets[node]; position < snapshot.out_row_offsets[node + 1]; ++position) {
      const ArcId id = snapshot.out_arc_ids.at(static_cast<std::size_t>(position));
      require(snapshot.arcs.at(id.value()).source == NodeId{node}, "out arc is in the wrong row");
      ++out_occurrences.at(id.value());
    }
    for (std::uint64_t position = snapshot.in_row_offsets[node]; position < snapshot.in_row_offsets[node + 1]; ++position) {
      const ArcId id = snapshot.in_arc_ids.at(static_cast<std::size_t>(position));
      require(snapshot.arcs.at(id.value()).sink == NodeId{node}, "in arc is in the wrong row");
      ++in_occurrences.at(id.value());
    }
  }
  require(std::ranges::all_of(out_occurrences, [](std::size_t count) { return count == 1; }), "out CSR does not contain each arc exactly once");
  require(std::ranges::all_of(in_occurrences, [](std::size_t count) { return count == 1; }), "in CSR does not contain each arc exactly once");
}

void verifyIndependentOrdinals(const CsrGraphSnapshot& snapshot)
{
  std::vector<const CsrGraphArcRecord*> native;
  native.reserve(snapshot.arcs.size());
  for (const auto& arc : snapshot.arcs) {
    native.push_back(&arc);
  }
  std::ranges::sort(native, {}, &CsrGraphArcRecord::native_traversal_rank);
  for (std::size_t index = 0; index < native.size(); ++index) {
    std::uint64_t expected = 0;
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (native[prior]->key.base == native[index]->key.base) {
        ++expected;
      }
    }
    require(native[index]->key.parallel_ordinal == expected, "parallel ordinal does not equal the independent prior-base count");
  }
}

void verifyForcedCoverage(const CsrGraphSnapshot& snapshot, const NativeGraphProjection& projection)
{
  require(snapshot.ordered_root_ids.size() >= 2, "fixture did not force multi-root coverage");
  require(std::ranges::any_of(snapshot.arcs, [](const auto& arc) { return arc.is_disabled; }), "fixture did not force disabled overlay coverage");
  require(std::ranges::any_of(snapshot.arcs, [](const auto& arc) { return arc.is_loop_disabled; }), "fixture did not force loop-disabled overlay coverage");
  require(std::ranges::any_of(snapshot.arcs, [](const auto& arc) { return arc.key.parallel_ordinal > 0; }), "fixture did not force parallel coverage");
  require(std::ranges::any_of(projection.base_out, [](const auto& row) { return row.ordered_arcs.empty(); }),
          "fixture did not force an absent native adjacency key/empty normalized row");
  require(std::ranges::any_of(snapshot.arcs, [](const auto& arc) { return arc.id.value() != arc.native_traversal_rank; }),
          "fixture did not force raw traversal/canonical ArcId disagreement");
  require(snapshot.nodes.front().key.full_name.starts_with('z'), "fixture did not force native map/canonical NodeId disagreement");
}

void verifySeed(std::uint64_t seed)
{
  auto fixture = makePropertyGraphFixture(seed);
  const NativeGraphProjection direct_before = captureDirectNativeGraphProjection(fixture.database());
  const std::string replay = replayGraph(seed, direct_before);
  try {
    const auto before_bytes = canonicalIndependentNativeGraphProjectionBytes(direct_before);
    const CsrGraphExportResult first_result = exportCsrGraphSnapshot(fixture.database());
    const CsrGraphSnapshot& first = requireSnapshot(first_result);
    require(validateCsrGraphSnapshot(first).empty(), "exported property snapshot is invalid");
    const NativeGraphProjectionResult decoded_result = decodeNativeGraphProjection(first);
    const NativeGraphProjection& decoded = requireProjection(decoded_result);
    require(decoded == direct_before, "property direct/decode round-trip differs");
    require(canonicalNativeGraphProjectionBytes(decoded) == before_bytes, "property production/independent encoding differs");

    verifyTranspose(first);
    verifyIndependentOrdinals(first);
    verifyForcedCoverage(first, decoded);

    const NativeGraphProjection direct_after = captureDirectNativeGraphProjection(fixture.database());
    require(direct_after == direct_before, "property export mutated Database");
    require(canonicalIndependentNativeGraphProjectionBytes(direct_after) == before_bytes, "property raw bytes changed after export");

    const CsrGraphExportResult second_result = exportCsrGraphSnapshot(fixture.database());
    const CsrGraphSnapshot& second = requireSnapshot(second_result);
    require(second == first, "property repeated export is nondeterministic");
    const CsrNativeGraphOracle native = readNativeGraphOracle(fixture.database());
    const IndependentFifoOracle fifo = runIndependentFifoOracle(fixture.database());
    require(!fifo.has_active_cycle && fifo.oracle == native, "property native order/levels differ from independent FIFO");
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(error.what()) + " replay={" + replay + "}");
  }
}

std::uint64_t parseSeed(const char* text)
{
  std::size_t consumed = 0;
  const std::string value(text);
  const std::uint64_t seed = std::stoull(value, &consumed, 0);
  require(consumed == value.size(), "seed contains trailing characters");
  return seed;
}

}  // namespace

int main(int argc, char* argv[])
{
  try {
    if (argc == 3 && std::string(argv[1]) == "--seed") {
      const std::uint64_t seed = parseSeed(argv[2]);
      verifySeed(seed);
      return 0;
    }
    require(argc == 1, "usage: CsrGraphPropertyTest [--seed integer]");
    constexpr std::array<std::uint64_t, 5> kSeeds{1, 7, 42, 20260820, 0xdeadbeefULL};
    for (std::uint64_t seed : kSeeds) {
      verifySeed(seed);
    }
  } catch (const std::exception& error) {
    std::cerr << "CsrGraphPropertyTest: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
