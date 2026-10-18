#include "CsrCudaPacking.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ista::csr::cuda_detail {
namespace {
Index rowCount(std::size_t nodes)
{
  if (nodes > std::numeric_limits<Index>::max() / 8)
    throw std::length_error("CUDA path row capacity overflow");
  return nodes * 8;
}
template<class T> void reserveGrowth(std::vector<T>& values, std::size_t count)
{
  if (count <= values.capacity()) return;
  const auto extra = values.capacity() / 2 + 1;
  const auto grown = extra <= values.max_size() - values.capacity() ? values.capacity() + extra : values.max_size();
  values.reserve(std::max(count, grown));
}
std::span<const Index> rowTags(const PathDomain& domain, PathRow row)
{
  return std::span<const Index>(domain.tags).subspan(row.begin, row.end - row.begin);
}
}  // namespace

PathDomain buildPathDomain(std::span<const ArcBinding> arcs, std::span<const PathAdjacency> adjacency, std::span<const PathSeed> seeds)
{
  const auto rows = rowCount(adjacency.size());
  std::vector<std::vector<Index>> tags(rows);
  std::vector<std::pair<Index, Index>> pending;
  const auto add = [&](Index row, Index tag) {
    if (row >= rows)
      throw std::invalid_argument("CUDA path seed row out of range");
    auto& values = tags[row];
    const auto found = std::ranges::lower_bound(values, tag);
    if (found == values.end() || *found != tag) {
      values.insert(found, tag);
      pending.emplace_back(row, tag);
    }
  };
  for (const auto& seed : seeds)
    add(seed.row, seed.tag);
  for (std::size_t head = 0; head < pending.size(); ++head) {
    const auto [row, tag] = pending[head];
    for (auto id : adjacency[row / 8].outgoing) {
      const auto& arc = arcs[id];
      if (!arc.active || !arc.path_transitions)
        continue;
      for (Index output = 0; output < 2; ++output)
        if (arc.path_transitions & (1U << ((row % 2) * 2 + output)))
          add(arc.sink * 8 + (row % 8 / 2) * 2 + output, tag);
    }
  }
  PathDomain result;
  result.rows.resize(rows);
  result.tags.reserve(pending.size());
  for (Index row = 0; row < rows; ++row) {
    const auto& values = tags[row];
    if (values.empty()) continue;
    const Index begin = result.tags.size();
    result.tags.insert(result.tags.end(), values.begin(), values.end());
    result.rows[row] = {begin, result.tags.size()};
    result.segments.push_back({row, result.rows[row]});
  }
  result.live_tags = result.tags.size();
  return result;
}

std::vector<PathAdjacency> buildPathAdjacency(Index nodes, std::span<const ArcBinding> arcs)
{
  std::vector<PathAdjacency> result(nodes);
  for (Index id = 0; id < arcs.size(); ++id) {
    const auto& arc = arcs[id];
    if (arc.source >= nodes || arc.sink >= nodes)
      throw std::invalid_argument("CUDA path-domain endpoint out of range");
    result[arc.source].outgoing.push_back(id);
    result[arc.sink].incoming.push_back(id);
  }
  return result;
}

void updatePathDomain(std::span<const ArcBinding> arcs, std::span<const PathAdjacency> adjacency,
                      std::span<const PathSeed> seeds, std::span<const Index> changed_nodes,
                      PathDomain& domain, PathDomainUndo& undo)
{
  const auto row_count = rowCount(adjacency.size());
  if (domain.rows.size() > row_count)
    throw std::invalid_argument("CUDA incremental path-domain storage mismatch");
  // Only affected structural rows own temporary tag payloads. Clear their old
  // identities first so an edge removal can remove the last route to a tag.
  std::map<Index, std::array<std::vector<Index>, 8>> dirty;
  std::vector<Index> nodes;
  const auto mark = [&](Index node) {
    if (node >= adjacency.size())
      throw std::invalid_argument("CUDA changed path-domain node out of range");
    if (dirty.try_emplace(node).second)
      nodes.push_back(node);
  };
  for (Index node : changed_nodes)
    mark(node);
  for (std::size_t head = 0; head < nodes.size(); ++head)
    for (Index id : adjacency[nodes[head]].outgoing) {
      const auto& arc = arcs[id];
      if (arc.active && arc.path_transitions)
        mark(arc.sink);
    }

  std::vector<std::pair<Index, Index>> pending;
  const auto add = [&](Index row, Index tag) {
    auto& values = dirty.at(row / 8)[row % 8];
    const auto found = std::ranges::lower_bound(values, tag);
    if (found == values.end() || *found != tag) {
      values.insert(found, tag);
      pending.emplace_back(row, tag);
    }
  };
  for (const auto& seed : seeds)
    if (dirty.contains(seed.row / 8))
      add(seed.row, seed.tag);
  // Entry identities from unchanged predecessors survive; identities carried
  // solely around a now-unseeded dirty cycle do not.
  for (Index node : nodes)
    for (Index id : adjacency[node].incoming) {
      const auto& arc = arcs[id];
      if (!arc.active || !arc.path_transitions || dirty.contains(arc.source))
        continue;
      for (Index lane = 0; lane < 8; ++lane) {
        const Index source = arc.source * 8 + lane;
        if (source >= domain.rows.size())
          continue;
        for (Index output = 0; output < 2; ++output)
          if (arc.path_transitions & (1U << ((lane % 2) * 2 + output)))
            for (Index slot = domain.rows[source].begin; slot < domain.rows[source].end; ++slot)
              add(node * 8 + (lane / 2) * 2 + output, domain.tags[slot]);
      }
    }
  for (std::size_t head = 0; head < pending.size(); ++head) {
    const auto [row, tag] = pending[head];
    for (Index id : adjacency[row / 8].outgoing) {
      const auto& arc = arcs[id];
      if (!arc.active || !arc.path_transitions)
        continue;
      for (Index output = 0; output < 2; ++output)
        if (arc.path_transitions & (1U << ((row % 2) * 2 + output)))
          add(arc.sink * 8 + (row % 8 / 2) * 2 + output, tag);
    }
  }
  std::map<Index, const std::vector<Index>*> changed;
  Index live = domain.live_tags, appended = 0;
  for (const auto& [node, lanes] : dirty)
    for (Index lane = 0; lane < 8; ++lane) {
      const Index row = node * 8 + lane;
      const auto old = row < domain.rows.size() ? domain.rows[row] : PathRow{};
      if (std::ranges::equal(rowTags(domain, old), lanes[lane])) continue;
      changed.emplace(row, &lanes[lane]);
      const auto count = lanes[lane].size();
      if (count > std::numeric_limits<Index>::max() - appended)
        throw std::length_error("CUDA path payload capacity overflow");
      appended += count;
      live -= old.end - old.begin;
    }
  if (appended > std::numeric_limits<Index>::max() - domain.tags.size())
    throw std::length_error("CUDA path payload capacity overflow");
  live += appended;
  const auto physical = domain.tags.size() + appended;
  undo.clear();
  undo.row_count = domain.rows.size();
  undo.tag_count = domain.tags.size();
  undo.segment_count = domain.segments.size();
  undo.live_tags = domain.live_tags;
  undo.compacted = physical - live > live;
  const Index base = undo.compacted ? 0 : domain.tags.size();
  const auto append = [&](Index row, std::span<const Index> values) {
    const auto before = row < domain.rows.size() ? domain.rows[row] : PathRow{};
    PathRow after;
    if (!values.empty()) {
      after.begin = base + undo.tags.size();
      undo.tags.insert(undo.tags.end(), values.begin(), values.end());
      after.end = base + undo.tags.size();
      undo.segments.push_back({row, after});
    }
    if (before != after) undo.rows.push_back({row, before, after});
  };
  // Compact live segments only. Empty logical rows do not turn a sparse
  // payload cleanup into a full N*8 scan or metadata copy.
  if (undo.compacted)
    for (const auto& segment : domain.segments)
      if (domain.rows[segment.row] == segment.range && !changed.contains(segment.row))
        append(segment.row, rowTags(domain, segment.range));
  for (const auto& [row, values] : changed) append(row, *values);

  // Every allocation precedes mutation; inserts below copy only trivial data.
  reserveGrowth(domain.rows, row_count);
  if (!undo.compacted) {
    reserveGrowth(domain.tags, physical);
    if (undo.segments.size() > domain.segments.max_size() - domain.segments.size())
      throw std::length_error("CUDA path segment capacity overflow");
    reserveGrowth(domain.segments, domain.segments.size() + undo.segments.size());
  }
  domain.rows.resize(row_count);
  if (undo.compacted) {
    domain.tags.swap(undo.tags);
    domain.segments.swap(undo.segments);
  } else {
    domain.tags.insert(domain.tags.end(), undo.tags.begin(), undo.tags.end());
    domain.segments.insert(domain.segments.end(), undo.segments.begin(), undo.segments.end());
  }
  for (const auto& row : undo.rows) domain.rows[row.row] = row.after;
  domain.live_tags = live;
  undo.applied = true;
}

void PathDomainUndo::restore(PathDomain& domain) noexcept
{
  if (!applied) return;
  for (const auto& row : rows)
    if (row.row < row_count) domain.rows[row.row] = row.before;
  domain.rows.resize(row_count);
  if (compacted) {
    domain.tags.swap(tags);
    domain.segments.swap(segments);
  } else {
    domain.tags.resize(tag_count);
    domain.segments.resize(segment_count);
  }
  domain.live_tags = live_tags;
  applied = false;
}
void PathDomainUndo::clear() noexcept
{
  rows.clear();
  tags.clear();
  segments.clear();
  applied = compacted = false;
}
}  // namespace ista::csr::cuda_detail
