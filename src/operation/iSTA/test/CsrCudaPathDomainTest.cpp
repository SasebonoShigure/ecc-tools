#include <algorithm>
#include <array>
#include <numeric>

#include "AllocationFailureSupport.hpp"
#include "CsrCpuReferenceTestFixture.hpp"
#include "CsrCudaPacking.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr::test {
using namespace cuda_detail;
namespace {
std::span<const Index> rowTags(const PathDomain& domain, Index row)
{
  const auto range = domain.rows[row];
  return std::span<const Index>(domain.tags).subspan(range.begin, range.end - range.begin);
}
bool samePaths(const PathDomain& a, const PathDomain& b)
{
  if (a.rows.size() != b.rows.size() || a.live_tags != b.live_tags) return false;
  for (Index row = 0; row < a.rows.size(); ++row)
    if (!std::ranges::equal(rowTags(a, row), rowTags(b, row))) return false;
  return true;
}
void sparseAppendAndCompact()
{
  // Many empty rows must not inflate the payload or its compact transaction.
  const std::vector<ArcBinding> arcs{{0, 9999, true, true, EdgeStorageKind::kBaseOut, 0b1001}};
  const auto adjacency = buildPathAdjacency(10000, arcs);
  std::vector<PathSeed> seeds{{0, 1, {}}};
  auto domain = buildPathDomain(arcs, adjacency, seeds);
  const std::array<Index, 1> changed{0};
  PathDomainUndo undo;
  const auto* rows = domain.rows.data();
  const auto rows_capacity = domain.rows.capacity();
  for (Index batch = 0; batch < 64; ++batch) {
    const auto before = domain;
    seeds.front().tag = batch + 2;
    updatePathDomain(arcs, adjacency, seeds, changed, domain, undo);
    const auto fresh = buildPathDomain(arcs, adjacency, seeds);
    requireCpuReference(samePaths(domain, fresh) && domain.tags.size() <= 2 * domain.live_tags,
                        "long sparse path edits leaked identities or unbounded dead payload");
    requireCpuReference(domain.rows.data() == rows && domain.rows.capacity() == rows_capacity
                            && undo.rows.size() == 2 && domain.segments.size() <= domain.tags.size(),
                        "sparse path update copied all rows or recorded empty segments");
    if (batch % 3 == 1) {
      undo.restore(domain);
      requireCpuReference(samePaths(domain, before) && domain.rows == before.rows && domain.tags == before.tags,
                          "path compact/append rollback did not restore actual slots");
      undo.clear();
      updatePathDomain(arcs, adjacency, seeds, changed, domain, undo);
      requireCpuReference(samePaths(domain, fresh), "path update after rollback differs from fresh");
    }
    undo.clear();
    const auto unchanged_rows = domain.rows;
    const auto unchanged_tags = domain.tags;
    updatePathDomain(arcs, adjacency, seeds, changed, domain, undo);
    requireCpuReference(undo.rows.empty() && domain.rows == unchanged_rows && domain.tags == unchanged_tags,
                        "equal tag sets moved or appended path payload");
    undo.clear();
  }
  const auto before = domain;
  seeds.clear();
  updatePathDomain(arcs, adjacency, seeds, changed, domain, undo);
  requireCpuReference(domain.live_tags == 0 && domain.tags.empty() && domain.segments.empty(),
                      "last path removal retained dead payload");
  undo.restore(domain);
  requireCpuReference(samePaths(domain, before) && domain.tags == before.tags,
                      "empty-domain rollback lost tags");
}
void allocationRollback()
{
  if constexpr (!incregpusta_debug_checks_enabled) return;
  const std::vector<ArcBinding> arcs{{0, 1, true, true, EdgeStorageKind::kBaseOut, 0b1001}};
  const auto adjacency = buildPathAdjacency(2, arcs);
  const std::array<Index, 1> changed{0};
  const std::vector<PathSeed> first{{0, 1, {}}}, second{{0, 2, {}}}, third{{0, 3, {}}};
  auto domain = buildPathDomain(arcs, adjacency, first);
  PathDomainUndo undo;
  updatePathDomain(arcs, adjacency, second, changed, domain, undo);
  undo.clear();
  const auto before = domain;
  auto measured = before;
  PathDomainUndo measured_undo;
  ista_test_allocation::armCountOnly();
  updatePathDomain(arcs, adjacency, third, changed, measured, measured_undo);
  const auto count = ista_test_allocation::disable();
  requireCpuReference(measured_undo.compacted, "allocation fixture did not compact");
  for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
    auto trial = before;
    PathDomainUndo trial_undo;
    bool failed = false;
    ista_test_allocation::armFailure(ordinal);
    try { updatePathDomain(arcs, adjacency, third, changed, trial, trial_undo); }
    catch (const std::bad_alloc&) { failed = true; }
    catch (...) { static_cast<void>(ista_test_allocation::disable()); throw; }
    static_cast<void>(ista_test_allocation::disable());
    trial_undo.restore(trial);
    requireCpuReference(failed && samePaths(trial, before) && trial.rows == before.rows && trial.tags == before.tags,
                        "path preparation allocation failure mutated canonical storage");
    trial_undo.clear();
    updatePathDomain(arcs, adjacency, third, changed, trial, trial_undo);
    requireCpuReference(samePaths(trial, measured), "path preparation did not recover after allocation failure");
  }
}
}  // namespace

void runPathDomainCases()
{
  const std::vector<ArcBinding> arcs{
      {0, 2, true, true, EdgeStorageKind::kBaseOut, 0b1001},
      {1, 2, true, true, EdgeStorageKind::kBaseOut, 0b0110},
      {2, 3, true, true, EdgeStorageKind::kBaseOut, 0b1111},
      {4, 5, true, true, EdgeStorageKind::kBaseOut, 0b1001},
      {0, 2, true, true, EdgeStorageKind::kAuxOut, 0b1001}};
  const std::vector<PathSeed> seeds{{0, 9, {}}, {9, 2, {}}, {15, 7, {}}};
  const auto domain = buildPathDomain(arcs, buildPathAdjacency(6, arcs), seeds);
  const auto row = [&](Index index) {
    const auto values = rowTags(domain, index);
    return std::vector<Index>(values.begin(), values.end());
  };
  requireCpuReference(row(16) == std::vector<Index>{2, 9} && row(17).empty()
                          && row(24) == std::vector<Index>{2, 9} && row(25) == std::vector<Index>{2, 9},
                      "sparse paths lost ordered tags or transition identity");
  requireCpuReference(row(22) == std::vector<Index>{7} && row(30) == std::vector<Index>{7}
                          && row(31) == std::vector<Index>{7} && row(32).empty() && row(40).empty(),
                      "sparse paths changed analysis/source identity or allocated an unseeded domain");
  auto current = arcs;
  current[1].active = false;
  const auto smaller = buildPathDomain(current, buildPathAdjacency(6, current), seeds);
  requireCpuReference(smaller.live_tags < domain.live_tags, "inactive arcs retained unreachable path payload");
  current[1].active = true;
  current.push_back({3, 2, true, true, EdgeStorageKind::kAuxOut, 0b1001});
  auto updated = domain;
  PathDomainUndo undo;
  const std::vector<Index> changed{2};
  const auto update = [&](Index nodes) {
    updatePathDomain(current, buildPathAdjacency(nodes, current), seeds, changed, updated, undo);
    requireCpuReference(samePaths(updated, buildPathDomain(current, buildPathAdjacency(nodes, current), seeds)),
                        "incremental path identities differ from fresh closure");
    undo.clear();
  };
  update(6);
  const auto cyclic_tags = updated.live_tags;
  current[0].active = current[1].active = current[4].active = false;
  update(6);
  requireCpuReference(updated.live_tags < cyclic_tags, "dirty cycle retained self-sustaining tags");
  current[0].active = true;
  for (const auto transitions : {0b0110, 0b1111, 0b1001}) {
    current[0].path_transitions = transitions;
    update(7);
  }
  const auto previous = updated;
  current[4].active = true;
  update(7);
  requireCpuReference(samePaths(updated, previous), "parallel duplicate path changed a tag set");
  current[0].active = false;
  update(7);
  requireCpuReference(samePaths(updated, previous), "removing one parallel source removed a surviving tag");
  sparseAppendAndCompact();
  allocationRollback();
}
}  // namespace ista::csr::test
