// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrReferenceFingerprintPayload.hpp"
#include "CsrReferenceInputTestSupport.hpp"
#include "CsrReferenceValidator.hpp"

namespace {

void require(bool condition, const char* message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool equalPayloads(const ista::csr::FingerprintPayloads& lhs, const ista::csr::FingerprintPayloads& rhs)
{
  return lhs.topology == rhs.topology && lhs.clock_constraint == rhs.clock_constraint && lhs.library == rhs.library && lhs.corner == rhs.corner
         && lhs.parasitic == rhs.parasitic && lhs.timing_input == rhs.timing_input;
}

void requireCommittedIdentity(const ista::csr::CsrSnapshotIdentity& identity)
{
  require(identity.session_token != 0, "identity token must be nonzero");
  require(identity.graph_generation == 0, "identity generation must be zero");
  require(ista::csr::isLowercaseSha256(identity.topology_fingerprint), "topology fingerprint syntax");
  require(ista::csr::isLowercaseSha256(identity.clock_constraint_fingerprint), "clock fingerprint syntax");
  require(ista::csr::isLowercaseSha256(identity.library_fingerprint), "library fingerprint syntax");
  require(ista::csr::isLowercaseSha256(identity.corner_fingerprint), "corner fingerprint syntax");
  require(ista::csr::isLowercaseSha256(identity.parasitic_fingerprint), "parasitic fingerprint syntax");
  require(ista::csr::isLowercaseSha256(identity.timing_input_fingerprint), "timing-input fingerprint syntax");
}

}  // namespace

int main()
{
  using namespace ista::csr;
  using namespace ista::csr::test;

  const CsrReferenceInput first = makeReferenceInputTestFixture(1);
  const CsrReferenceInput second = makeReferenceInputTestFixture(2);
  // These are production reference payloads, not generic analysis-context bytes.
  const std::array<std::pair<StaAnalysisContext, const char*>, 3> corners{{
      {{"__direct__", "__unspecified_temperature__", "__no_rc_corner__"},
       "762ed637a7b3f10653e1212cf54480f9d8b319108418663f301a9b45b5e97dcf"},
      {{"slow", "25", "__no_rc_corner__"},
       "3e1ae07c69c21623632f49b62cac0604e6b62d5acabdd2ba5c992a08ea2d9de0"},
      {{"慢", "25", "__no_rc_corner__"},
       "71ebe214003ff803fcf733046f836fa7e5b9cf37f2a94de04f1ca42d6cbd2c89"},
  }};
  for (const auto& [context, expected] : corners) {
    CsrReferenceInput input = first;
    input.analysis_context = context;
    require(makeReferenceCornerFingerprint(input) == expected, "production corner known answer differs");
    require(makeReferenceInputIdentity(input, 1, 0).corner_fingerprint == expected,
            "production snapshot identity differs from its corner known answer");
  }

  require(validateReferenceInputForSession(first, 1).empty(), "fixture must be strict-valid");
  requireCommittedIdentity(first.source_identity);

  const FingerprintPayloads first_payloads = buildReferenceFingerprintPayloads(first);
  const FingerprintPayloads second_payloads = buildReferenceFingerprintPayloads(second);
  require(!first_payloads.topology.empty(), "topology payload must carry its empty marker");
  require(!first_payloads.clock_constraint.empty(), "clock payload must carry its empty marker");
  require(!first_payloads.library.empty(), "library payload must carry its empty marker");
  require(!first_payloads.corner.empty(), "corner payload must carry its empty marker");
  require(!first_payloads.parasitic.empty(), "parasitic payload must carry its empty marker");
  require(!first_payloads.timing_input.empty(), "timing payload must carry its empty marker");
  require(equalPayloads(first_payloads, second_payloads), "payload builder must ignore source identity");
  require(canonicalReferenceInputBytes(first) == canonicalReferenceInputBytes(second), "production logical bytes must ignore source identity");
  require(canonicalIndependentReferenceInputBytes(first) != canonicalIndependentReferenceInputBytes(second),
          "test envelope must include complete source identity");

  CsrReferenceInput rebound = first;
  rebound.source_identity.session_token = 2;
  require(validateReferenceInput(rebound).empty(), "self validation must accept a nonzero token replacement");
  require(!validateReferenceInputForSession(rebound, 1).empty(), "bound validation must reject the old token");
  require(validateReferenceInputForSession(rebound, 2).empty(), "bound validation must accept the replacement token");

  CsrReferenceInput tampered = first;
  tampered.source_identity.topology_fingerprint[0] = tampered.source_identity.topology_fingerprint[0] == '0' ? '1' : '0';
  require(!validateReferenceInput(tampered).empty(), "tampered component hash must be rejected");

  CsrReferenceInput changed_corner = first;
  changed_corner.analysis_context.temperature = "26";
  changed_corner.source_identity = makeReferenceInputIdentity(changed_corner, 1, 0);
  require(validateReferenceInputForSession(changed_corner, 1).empty(), "atomic corner mutation must remain strict-valid");
  require(first.source_identity.topology_fingerprint == changed_corner.source_identity.topology_fingerprint, "corner mutation changed topology");
  require(first.source_identity.clock_constraint_fingerprint == changed_corner.source_identity.clock_constraint_fingerprint,
          "corner mutation changed clock constraint");
  require(first.source_identity.library_fingerprint == changed_corner.source_identity.library_fingerprint, "corner mutation changed library");
  require(first.source_identity.parasitic_fingerprint == changed_corner.source_identity.parasitic_fingerprint, "corner mutation changed parasitic");
  require(first.source_identity.corner_fingerprint != changed_corner.source_identity.corner_fingerprint, "corner mutation did not change corner");
  require(first.source_identity.timing_input_fingerprint != changed_corner.source_identity.timing_input_fingerprint,
          "corner mutation did not change composite");

  return 0;
}
