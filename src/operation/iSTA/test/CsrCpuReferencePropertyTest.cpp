// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceSlowOracle.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrReferenceValidator.hpp"

namespace {

std::uint64_t parseSeed(int argc, char* argv[])
{
  if (argc != 3 || std::string(argv[1]) != "--seed")
    throw std::runtime_error("usage: ista_csr_cpu_reference_property_test --seed <integer>");
  std::size_t consumed = 0;
  const std::uint64_t seed = std::stoull(argv[2], &consumed, 0);
  if (consumed != std::string(argv[2]).size())
    throw std::runtime_error("property seed must be a complete integer");
  return seed;
}

std::string replaySummary(const ista::csr::CsrReferenceInput& input)
{
  std::ostringstream output;
  output << std::hexfloat << "canonical_nodes=[";
  for (const auto& node : input.nodes)
    output << node.id.value() << ':' << node.key.full_name << ',';
  output << "] canonical_arcs=[";
  for (const auto& arc : input.arcs)
    output << arc.id.value() << ':' << arc.source.value() << "->" << arc.sink.value() << "/ordinal=" << arc.key.parallel_ordinal
           << "/rank=" << arc.native_traversal_rank << "/disabled=" << arc.is_disabled << "/loop=" << arc.is_loop_disabled << ',';
  output << "] roots=[";
  for (const auto node : input.ordered_root_ids)
    output << node.value() << ',';
  output << "] out_offsets=[";
  for (const auto offset : input.out_row_offsets)
    output << offset << ',';
  output << "] out_ids=[";
  for (const auto arc : input.out_arc_ids)
    output << arc.value() << ',';
  output << "] in_offsets=[";
  for (const auto offset : input.in_row_offsets)
    output << offset << ',';
  output << "] in_ids=[";
  for (const auto arc : input.in_arc_ids)
    output << arc.value() << ',';
  output << "] transfers=[";
  for (const auto& arc : input.arcs)
    output << arc.id.value() << ':' << arc.transfer.delay << '/' << arc.transfer.delay_min << '/' << arc.transfer.delay_max
           << "/children=" << arc.transfer.children.size() << ',';
  output << ']';
  return output.str();
}

}  // namespace

int main(int argc, char* argv[])
{
  using namespace ista::csr;
  using namespace ista::csr::test;
  std::string failure_context = "seed=<unparsed>";
  try {
    const std::uint64_t seed = parseSeed(argc, argv);
    const CsrReferenceInput input = makeCpuReferencePropertyFixture(seed);
    failure_context = "seed=" + std::to_string(seed) + ' ' + replaySummary(input);
    const std::vector<std::uint8_t> before = cpuReferenceCanonicalBytes(input);
    const CsrCpuReferenceBackend backend;
    const CsrComputationResult first = backend.compute(input);
    const CsrComputationResult second = backend.compute(input);
    const std::vector<std::uint8_t> after = cpuReferenceCanonicalBytes(input);
    requireCpuReference(before == after, "property backend mutated canonical input bytes");
    requireExactComputationResult(first, second, "property determinism");
    const CsrComputationResult oracle = computeCsrCpuReferenceSlowOracle(input);
    requireExactComputationResult(first, oracle, "property slow-oracle differential");
    const auto* timing = std::get_if<CsrTimingResult>(&first.outcome);
    const auto* oracle_timing = std::get_if<CsrTimingResult>(&oracle.outcome);
    requireCpuReference(timing != nullptr, "valid supported property graph did not return timing");
    requireCpuReference(oracle_timing != nullptr, "valid supported slow oracle did not return timing");
    requireExactTimingResult(*timing, *oracle_timing, "property direct exact timing differential");
    const auto errors = validateTimingResult(input, *timing);
    requireCpuReference(errors.empty(), "property result failed the external timing-result validator");
  } catch (const std::exception& error) {
    std::cerr << failure_context << ": " << error.what() << '\n';
    return 1;
  }
  return 0;
}
