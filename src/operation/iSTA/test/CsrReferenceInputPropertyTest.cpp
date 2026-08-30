// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <variant>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceInputRawProjection.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {

void require(bool condition, const char* message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::uint64_t parseSeed(int argc, char** argv)
{
  require(argc == 3 && std::string_view(argv[1]) == "--seed", "expected --seed VALUE");
  std::string_view text(argv[2]);
  int base = 10;
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
    base = 16;
  }
  std::uint64_t seed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), seed, base);
  require(result.ec == std::errc{} && result.ptr == text.data() + text.size(), "invalid property seed");
  return seed;
}

}  // namespace

int main(int argc, char** argv)
{
  using namespace ista::csr;
  using namespace ista::csr::test;

  const std::uint64_t seed = parseSeed(argc, argv);
  auto native = makeReferencePropertyFixture(seed);
  const auto raw_before = projectCsrReferenceInputRaw(native.database());
  const auto first = exportCsrReferenceInputCandidate(native.database(), referenceInputTestContext(), seed == 0 ? 1 : seed);
  const auto second = exportCsrReferenceInputCandidate(native.database(), referenceInputTestContext(), seed == 0 ? 1 : seed);
  require(first.index() == second.index(), "property export outcome is nondeterministic");
  require(std::holds_alternative<CsrReferenceInput>(first), "property export must produce a candidate");

  const auto& lhs = std::get<CsrReferenceInput>(first);
  const auto& rhs = std::get<CsrReferenceInput>(second);
  const std::uint64_t token = seed == 0 ? 1 : seed;
  require(validateReferenceInputForSession(lhs, token).empty(), "property candidate is not strict-valid");
  require(lhs.source_identity == rhs.source_identity, "property identity is nondeterministic");
  require(canonicalReferenceInputBytes(lhs) == canonicalReferenceInputBytes(rhs), "property logical bytes are nondeterministic");
  require(canonicalIndependentReferenceInputBytes(lhs) == canonicalIndependentReferenceInputBytes(rhs), "property full-envelope bytes are nondeterministic");
  require(raw_before == projectCsrReferenceInputRaw(native.database()), "read-only export changed raw native input");
  require(!lhs.nodes.empty(), "successful property candidate must be nonempty");
  require(lhs.out_row_offsets.size() == lhs.nodes.size() + 1, "property outgoing CSR shape mismatch");
  require(lhs.in_row_offsets.size() == lhs.nodes.size() + 1, "property incoming CSR shape mismatch");
  return 0;
}
