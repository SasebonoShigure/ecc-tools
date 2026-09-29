#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaBackend.hpp"
#include "CsrCudaPacking.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr::test {

CsrComputationResult computeContractTiming(const CsrReferenceInput& input)
{
  auto actual = CsrCudaBackend{}.computeWithTelemetry(input);
  if constexpr (incregpusta_debug_checks_enabled)
    requireExactComputationResult(actual.computation, CsrCpuReferenceBackend{}.compute(input), "CUDA full all-field differential");
  return std::move(actual.computation);
}

void runPackingCases()
{
  using namespace cuda_detail;
  const std::vector<ArcBinding> arcs{
      {0, 2, true, true, EdgeStorageKind::kBaseOut, 0b1001},
      {1, 2, true, true, EdgeStorageKind::kBaseOut, 0b0110},
      {2, 3, true, true, EdgeStorageKind::kBaseOut, 0b1111},
      {4, 5, true, true, EdgeStorageKind::kBaseOut, 0b1001},
      {0, 2, true, true, EdgeStorageKind::kAuxOut, 0b1001}};
  const std::vector<PathSeed> seeds{{0, 9, {}}, {9, 2, {}}, {15, 7, {}}};
  const auto domain = buildPathDomain(arcs, buildPathAdjacency(6, arcs), seeds);
  const auto row = [&](Index index) {
    return std::vector<Index>(domain.tags.begin() + domain.offsets[index], domain.tags.begin() + domain.offsets[index + 1]);
  };
  requireCpuReference(row(16) == std::vector<Index>{2, 9} && row(17).empty()
                          && row(24) == std::vector<Index>{2, 9} && row(25) == std::vector<Index>{2, 9},
                      "sparse paths lost ordered tags or transition identity");
  requireCpuReference(row(22) == std::vector<Index>{7} && row(30) == std::vector<Index>{7}
                          && row(31) == std::vector<Index>{7} && row(32).empty() && row(40).empty(),
                      "sparse paths changed analysis/source identity or allocated an unseeded domain");
  auto removed = arcs;
  removed[1].active = false;
  const auto smaller = buildPathDomain(removed, buildPathAdjacency(6, removed), seeds);
  requireCpuReference(smaller.tags.size() < domain.tags.size(), "inactive arcs retained unreachable path payload");
  removed[1].active = true;
  removed.push_back({3, 2, true, true, EdgeStorageKind::kAuxOut, 0b1001});
  const auto cyclic = buildPathDomain(removed, buildPathAdjacency(6, removed), seeds);
  requireCpuReference(cyclic.offsets.back() == cyclic.tags.size(), "structural cycle did not terminate");
  PathDomain updated;
  const std::vector<Index> changed{2};
  updatePathDomain(removed, buildPathAdjacency(6, removed), seeds, domain, changed, updated);
  requireCpuReference(updated == cyclic, "incremental cyclic path closure differs from fresh construction");
  // Drop the only seeded entrance to a structural cycle. Its own stale tags
  // must not become new seeds, including register rows with empty PathState.
  auto current = removed;
  current[0].active = current[1].active = current[4].active = false;
  updatePathDomain(current, buildPathAdjacency(6, current), seeds, cyclic, changed, updated);
  requireCpuReference(updated == buildPathDomain(current, buildPathAdjacency(6, current), seeds)
                          && updated.tags.size() < cyclic.tags.size(),
                      "removed sources retained self-sustaining tags in a dirty cycle");
  auto previous = updated;
  current[0].active = true;
  current[0].path_transitions = 0b0110;
  updatePathDomain(current, buildPathAdjacency(6, current), seeds, previous, changed, updated);
  requireCpuReference(updated == buildPathDomain(current, buildPathAdjacency(6, current), seeds),
                      "negative transition update lost path identity");
  previous = updated;
  current[0].path_transitions = 0b1111;
  updatePathDomain(current, buildPathAdjacency(6, current), seeds, previous, changed, updated);
  requireCpuReference(updated == buildPathDomain(current, buildPathAdjacency(6, current), seeds),
                      "non-unate update lost transition identity");
  previous = updated;
  current[0].path_transitions = 0b1001;
  updatePathDomain(current, buildPathAdjacency(7, current), seeds, previous, changed, updated);
  requireCpuReference(updated == buildPathDomain(current, buildPathAdjacency(7, current), seeds),
                      "narrowed transitions or isolated appended node changed path identities");
  previous = updated;
  current[4].active = true;
  updatePathDomain(current, buildPathAdjacency(7, current), seeds, previous, changed, updated);
  requireCpuReference(updated == previous, "parallel duplicate path changed a tag set");
  previous = updated;
  current[0].active = false;
  updatePathDomain(current, buildPathAdjacency(7, current), seeds, previous, changed, updated);
  requireCpuReference(updated == previous, "removal of one parallel source removed a surviving tag");
  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    const auto input = seed == 0 ? makeCpuReferenceCellFixture() : makeCpuReferencePropertyFixture(seed);
    ModelStorage models;
    const auto packed = pack(input, models);
    const auto computed = CsrCpuReferenceBackend{}.compute(input);
    const auto* timing = std::get_if<CsrTimingResult>(&computed.outcome);
    requireCpuReference(timing != nullptr, "sparse packing CPU oracle failed");
    for (std::size_t id = 0; id < input.arcs.size(); ++id) {
      const auto& arc = input.arcs[id];
      const auto mask = pathTransitions(arc.kind, arc.transfer, arc.eligibility.data_arrival);
      for (unsigned lane = 0; lane < 4; ++lane)
        requireCpuReference(!arc.eligibility.data_arrival
                                || bool(mask & (1U << lane)) == bool(timing->arcs.input_output_delays.presence[id * 8 + lane]),
                            "path-domain transition mask differs from CPU timing semantics");
    }
    for (std::size_t r = 0; r + 1 < timing->paths.row_offsets.size(); ++r)
      for (auto slot = timing->paths.row_offsets[r]; slot < timing->paths.row_offsets[r + 1]; ++slot) {
        const auto begin = packed.paths.tags.begin() + packed.paths.offsets[r];
        const auto end = packed.paths.tags.begin() + packed.paths.offsets[r + 1];
        requireCpuReference(std::binary_search(begin, end, timing->paths.clock_tags.values[slot].value()),
                            "computed path was omitted from the structural domain");
      }
  }
  auto shared = makeCpuReferencePropertyFixture(2);
  const auto model = shared.arcs[1].transfer;
  for (std::size_t id = 1; id < shared.arcs.size(); ++id) {
    shared.arcs[id].transfer = model;
    shared.referenced_library_cells.front().cell_arcs[id - 1].transfer = model;
  }
  refreshCpuReferenceIdentity(shared);
  ModelStorage shared_models;
  const auto storage = pack(shared, shared_models);
  requireCpuReference(shared_models.children.size() == model.children().size()
                          && storage.child_state_count == model.children().size() * (shared.arcs.size() - 1),
                      "identical models were duplicated or instance state was aliased");
  for (std::size_t id = 2; id < storage.arcs.size(); ++id)
    requireCpuReference(storage.arcs[id].child_begin == storage.arcs[1].child_begin
                            && storage.arcs[id].child_state_begin != storage.arcs[1].child_state_begin,
                        "shared models lost independent child state");
  ModelStorage models;
  const auto first = models.intern(model.childrenOwner());
  CellTransferModel independent = model;
  independent.setChildren(model.children());
  requireCpuReference(independent.childrenOwner() != model.childrenOwner()
                          && models.intern(independent.childrenOwner()) == first,
                      "independent equal public model failed value interning");
  auto collision = model.children();
  collision[0].delay_tables[0].values[1] += 0.25;
  const auto collision_owner = std::make_shared<const std::vector<ChildTimingArcModel>>(std::move(collision));
  const auto second = models.intern(collision_owner);
  requireCpuReference(first != second && models.intern(model.childrenOwner()) == first && models.intern(collision_owner) == second,
                      "signature/hash collision merged distinct Liberty models");
  auto positive_zero = model.children(), negative_zero = model.children();
  positive_zero[0].delay_tables[0].values[1] = 0.0;
  negative_zero[0].delay_tables[0].values[1] = -0.0;
  requireCpuReference(models.intern(std::make_shared<const std::vector<ChildTimingArcModel>>(std::move(positive_zero)))
                          != models.intern(std::make_shared<const std::vector<ChildTimingArcModel>>(std::move(negative_zero))),
                      "model interning discarded the sign of a timing-table zero");
  auto duplicate_seeds = makeCpuReferenceNetFixture();
  duplicate_seeds.clock_tags.push_back("second");
  const auto append_seed = [&](const std::string& tag, double arrival) {
    auto seed = duplicate_seeds.start_seeds.front();
    seed.clock_tag = tag;
    for (auto& analysis : seed.arrival)
      for (auto& value : analysis)
        value = arrival;
    duplicate_seeds.ordered_start_ids.push_back(seed.seed_node);
    duplicate_seeds.start_seeds.push_back(std::move(seed));
  };
  append_seed("second", 0.5);
  append_seed(duplicate_seeds.start_seeds.front().clock_tag, 0.75);
  const auto folded = pack(duplicate_seeds, models);
  requireCpuReference(folded.path_seeds.size() == 8 && folded.node_seeds.front().state.arrival == 0.75,
                      "duplicate seed folding lost a tag or scalar last write");
  for (const auto& seed : folded.path_seeds)
    requireCpuReference(seed.state.present && seed.state.arrival == (seed.tag == 0 ? 0.75 : 0.5),
                        "duplicate seed folding changed last-write path values");
  auto many_tags = makeCpuReferenceNetFixture();
  for (unsigned tag = 0; tag < 256; ++tag)
    many_tags.clock_tags.push_back("unused-" + std::to_string(tag));
  const auto packed = pack(many_tags, models);
  requireCpuReference(packed.paths.tags.size() == pack(makeCpuReferenceNetFixture(), models).paths.tags.size(),
                      "unused clock tags expanded path payload");
}

void runCudaCases()
{
  runPackingCases();
  runWholeResultLiteralContract();
  runCpuReferenceArcContracts();
  runCpuReferencePropagationContracts();
  runComputedNumericFailureContracts();
  const auto cycle = computeContractTiming(makeCpuReferenceCycleFixture());
  requireCpuReference(cycle.outcome == CsrComputationResult{CsrComputationCode::kCycle}.outcome, "CUDA must reject cycle");
  auto malformed = makeCpuReferenceNetFixture();
  malformed.out_row_offsets.pop_back();
  const auto invalid = computeContractTiming(malformed);
  requireCpuReference(invalid.outcome == CsrComputationResult{CsrComputationCode::kInvariantViolation}.outcome, "CUDA must reject malformed CSR");
  for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 20260901ULL, 0xdeadbeefULL})
    for (std::uint64_t sample = 0; sample < 16; ++sample) {
      const auto input = makeCpuReferencePropertyFixture(seed + sample);
      const auto first = computeContractTiming(input);
      requireCpuReference(std::holds_alternative<CsrTimingResult>(first.outcome), "CUDA property fixture must succeed");
      if constexpr (incregpusta_debug_checks_enabled)
        requireExactComputationResult(computeContractTiming(input), first, "CUDA cold/warm repeat");
    }
}

void runPerformance()
{
  if constexpr (incregpusta_debug_checks_enabled)
    throw std::invalid_argument("CUDA performance requires ECC_INCREGPUSTA_DEBUG_CHECKS=OFF");
  CsrCudaBackend backend;
  const auto input = makeCpuReferenceCellFixture();
  (void) backend.computeWithTelemetry(input);
  for (int sample = 0; sample < 3; ++sample) {
    const auto output = backend.computeWithTelemetry(input);
    if (!std::holds_alternative<CsrTimingResult>(output.computation.outcome))
      throw std::runtime_error("CUDA performance workload failed");
    const auto& t = output.telemetry;
    std::cout << "{\"sample\":" << sample << ",\"debug_checks_enabled\":false,\"correctness_comparison\":\"not_run\","
              << "\"end_to_end_ms\":" << t.end_to_end_ms << ",\"h2d_bytes\":" << t.h2d_bytes << ",\"d2h_bytes\":" << t.d2h_bytes
              << ",\"allocations\":" << t.allocations << ",\"explicit_kernel_launches\":" << t.kernel_launches << ",\"device_algorithm_calls\":" << t.device_algorithm_calls
              << ",\"levelization_decision_bytes\":" << t.levelization_decision_bytes << "}\n";
  }
}

}  // namespace ista::csr::test

int main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--packing-only") {
      ista::csr::test::runPackingCases();
      return 0;
    }
    ista::csr::CsrCudaBackend::requireDevice();
    if (argc == 2 && std::string(argv[1]) == "--performance")
      ista::csr::test::runPerformance();
    else if (argc == 1)
      ista::csr::test::runCudaCases();
    else
      throw std::invalid_argument("usage: ista_csr_cuda_contract_test [--performance | --packing-only]");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
