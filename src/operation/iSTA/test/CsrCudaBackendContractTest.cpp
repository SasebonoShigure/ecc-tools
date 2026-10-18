#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceBackend.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaBackend.hpp"
#include "CsrCudaPacking.hpp"
#include "CsrModelTestMutation.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr::test {

CsrComputationResult computeContractTiming(const CsrReferenceInput& input)
{
  auto actual = CsrCudaBackend{}.computeWithTelemetry(input);
  if constexpr (incregpusta_debug_checks_enabled)
    requireExactComputationResult(actual.computation, CsrCpuReferenceBackend{}.compute(input), "CUDA full all-field differential");
  return std::move(actual.computation);
}

void runPathDomainCases();

void runModelValidationCases()
{
  using namespace cuda_detail;
  const auto input = makeCpuReferenceCellFixture();
  const auto arc = std::ranges::find(input.arcs, ArcKind::kCell, &ArcRecord::kind);
  requireCpuReference(arc != input.arcs.end() && !arc->transfer.children().empty(), "model fixture has no children");
  const auto original = arc->transfer;
  ModelStorage models;
  const auto first = models.internTransfer(original);
  const auto reject = [&](const CellTransferModel& model) {
    bool rejected = false;
    try { (void) models.internTransfer(model); } catch (const std::invalid_argument&) { rejected = true; }
    requireCpuReference(rejected && models.internTransfer(original) == first, "model validation accepted invalid transfer or lost valid owner");
  };
  auto invalid = original;
  invalid.delay = std::numeric_limits<double>::infinity();
  reject(invalid);  // Shared immutable owner cannot validate mutable fallback scalars.
  invalid = original;
  mutateChildren(invalid, [](auto& children) { children[0].delay_tables[0].values.push_back(1.0); });
  reject(invalid);
  auto duplicate = original;
  mutateChildren(duplicate, [](auto& children) { children.push_back(children.front()); });
  const auto check_begin = models.intern(duplicate.childrenOwner());
  reject(duplicate);
  auto equal_duplicate = duplicate;
  equal_duplicate.setChildren(duplicate.children());
  reject(equal_duplicate);
  requireCpuReference(models.intern(equal_duplicate.childrenOwner()) == check_begin,
                      "timing-check model acquired transfer-only unique-index restriction");
  auto replacement = original;
  mutateChildren(replacement, [](auto& children) { children[0].delay_tables[0].values[0] += 0.25; });
  ModelStorage reference;
  (void) reference.internTransfer(original);
  const auto replacement_begin = reference.internTransfer(replacement);
  if constexpr (incregpusta_debug_checks_enabled) {
    std::size_t allocations{};
    {
      ModelStorage counted;
      (void) counted.internTransfer(original);
      ista_test_allocation::armCountOnly();
      (void) counted.internTransfer(replacement);
      allocations = ista_test_allocation::disable();
    }
    requireCpuReference(allocations > 0, "new model allocation path was not exercised");
    for (std::size_t ordinal = 0; ordinal < allocations; ++ordinal) {
      ModelStorage retry;
      (void) retry.internTransfer(original);
      const auto old_children = retry.children.size();
      const auto old_tables = retry.tables;
      bool failed = false;
      ista_test_allocation::armFailure(ordinal);
      try { (void) retry.internTransfer(replacement); } catch (const std::bad_alloc&) { failed = true; }
      catch (...) { (void) ista_test_allocation::disable(); throw; }
      const bool injected = ista_test_allocation::failureInjected();
      (void) ista_test_allocation::disable();
      requireCpuReference(failed && injected && retry.children.size() == old_children && retry.tables == old_tables,
                          "model allocation failure published partial immutable data");
      requireCpuReference(retry.internTransfer(original) == first && retry.internTransfer(replacement) == replacement_begin
                              && retry.children.size() == reference.children.size() && retry.tables == reference.tables,
                          "model allocation failure prevented exact retry");
    }
  }
}

void runPackingCases()
{
  using namespace cuda_detail;
  runPathDomainCases();
  runModelValidationCases();
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
        const auto begin = packed.paths.tags.begin() + packed.paths.rows[r].begin;
        const auto end = packed.paths.tags.begin() + packed.paths.rows[r].end;
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
