// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrParasiticModel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ista::csr {
namespace {

constexpr double kNativeEpsilon = 1e-6;

double finite(double value)
{
  if (!std::isfinite(value))
    throw std::overflow_error("non-finite RC reduction");
  return value;
}

void validateTree(std::span<const CsrRcTreeNode> tree, std::span<const std::size_t> terminals)
{
  if (tree.empty() || terminals.empty() || terminals.front() != 0)
    throw std::invalid_argument("RC tree requires a driver terminal");
  if (tree.front().parent != 0 || tree.front().resistance_kohm != 0.0)
    throw std::invalid_argument("invalid RC root");
  for (std::size_t i = 0; i < tree.size(); ++i) {
    const auto& node = tree[i];
    if ((i != 0 && node.parent >= i) || !std::isfinite(node.resistance_kohm)
        || node.resistance_kohm < 0.0 || !std::isfinite(node.capacitance_pf)
        || node.capacitance_pf < 0.0)
      throw std::invalid_argument("invalid RC tree node");
  }
  for (std::size_t i = 0; i < terminals.size(); ++i)
    if (terminals[i] >= tree.size() || (i != 0 && terminals[i] <= terminals[i - 1]))
      throw std::invalid_argument("invalid RC terminal order");
}

CsrArnoldiModel reduceArnoldi(std::span<const CsrRcTreeNode> tree, std::span<const std::size_t> terminals)
{
  CsrArnoldiModel model;
  for (const auto& node : tree)
    model.total_capacitance = finite(model.total_capacitance + node.capacitance_pf);
  if (model.total_capacitance <= 0.0)
    return model;
  model.sqrt_total_capacitance = std::sqrt(model.total_capacitance);
  const auto size = tree.size();
  const int limit = static_cast<int>(std::min(size, static_cast<std::size_t>(kCsrArnoldiOrder)));
  std::vector<double> basis(size, 1.0 / model.sqrt_total_capacitance);
  std::vector<double> previous(size, 0.0), response(size, 0.0), current(size, 0.0);
  model.projection.resize(terminals.size());
  for (int order = 0; order < limit; ++order) {
    model.order = order + 1;
    for (std::size_t t = 0; t < terminals.size(); ++t)
      model.projection[t][order] = basis[terminals[t]];
    std::fill(current.begin(), current.end(), 0.0);
    for (std::size_t i = size - 1; i > 0; --i) {
      current[i] = finite(current[i] + tree[i].capacitance_pf * basis[i]);
      current[tree[i].parent] = finite(current[tree[i].parent] + current[i]);
    }
    current[0] = finite(current[0] + tree[0].capacitance_pf * basis[0]);
    response[0] = 0.0;
    for (std::size_t i = 1; i < size; ++i)
      response[i] = finite(response[tree[i].parent] + tree[i].resistance_kohm * current[i]);
    double diagonal = 0.0;
    for (std::size_t i = 1; i < size; ++i)
      diagonal = finite(diagonal + basis[i] * tree[i].capacitance_pf * response[i]);
    model.diagonal[order] = diagonal;
    if (order + 1 == limit || diagonal < kNativeEpsilon)
      break;
    for (std::size_t i = 0; i < size; ++i) {
      response[i] = finite(response[i] - diagonal * basis[i]);
      if (order != 0)
        response[i] = finite(response[i] - model.off_diagonal[order - 1] * previous[i]);
    }
    double squared_norm = 0.0;
    for (std::size_t i = 0; i < size; ++i)
      squared_norm = finite(squared_norm + tree[i].capacitance_pf * response[i] * response[i]);
    if (squared_norm < kNativeEpsilon * kNativeEpsilon)
      break;
    model.off_diagonal[order] = std::sqrt(squared_norm);
    previous.swap(basis);
    for (std::size_t i = 0; i < size; ++i)
      basis[i] = finite(response[i] / model.off_diagonal[order]);
  }
  return model;
}

CsrDmpLoadModel reduceLoad(double m1, double m2, double m3)
{
  CsrDmpLoadModel model;
  if (m1 == 0.0)
    return model;
  double p1 = m3 == 0.0 ? 0.0 : -m2 / m3;
  const double denominator = m2 == 0.0 || m3 == 0.0 ? 0.0 : m1 / m2 - m2 / m3;
  double p2 = 0.0;
  if (p1 > 0.0 && denominator != 0.0 && m2 != 0.0)
    p2 = p1 * (1.0 / m1 - m1 / m2) / denominator;
  if (!(p1 > 0.0) || !(p2 > 0.0) || p1 == p2 || denominator == 0.0
      || !std::isfinite(p1) || !std::isfinite(p2)) {
    p1 = -1.0 / m1;
    if (p1 > 0.0 && std::isfinite(p1)) {
      model.order = 1;
      model.poles[0] = p1;
      model.residues[0] = 1.0;
    }
    return model;
  }
  double r1 = p1 * p1 * (1.0 + m1 * p2) / (p1 - p2);
  double r2 = -p2 * p2 * (1.0 + m1 * p1) / (p1 - p2);
  if (!std::isfinite(r1) || !std::isfinite(r2))
    return model;
  if (r1 < 0.0 && r2 > 0.0) {
    std::swap(p1, p2);
    std::swap(r1, r2);
  }
  model.order = 2;
  model.poles = {p1, p2};
  model.residues = {r1, r2};
  return model;
}

CsrDmpModel reduceDmp(std::span<const CsrRcTreeNode> tree, std::span<const std::size_t> terminals)
{
  const auto size = tree.size();
  std::vector<double> y1(size), y2(size, 0.0), y3(size, 0.0);
  for (std::size_t i = 0; i < size; ++i)
    y1[i] = tree[i].capacitance_pf;
  for (std::size_t i = size - 1; i > 0; --i) {
    const auto parent = tree[i].parent;
    const double r = tree[i].resistance_kohm;
    y1[parent] = finite(y1[parent] + y1[i]);
    y2[parent] = finite(y2[parent] + (y2[i] - r * y1[i] * y1[i]));
    y3[parent] = finite(y3[parent] + (y3[i] - 2.0 * r * y1[i] * y2[i] + r * r * y1[i] * y1[i] * y1[i]));
  }
  CsrDmpModel model;
  if (y2[0] == 0.0 && y3[0] == 0.0) {
    model.load_capacitance = y1[0];
  } else {
    if (y3[0] == 0.0 || y2[0] == 0.0)
      return model;
    model.load_capacitance = y2[0] * y2[0] / y3[0];
    model.driver_capacitance = y1[0] - model.load_capacitance;
    model.resistance = -y3[0] * y3[0] / (y2[0] * y2[0] * y2[0]);
  }
  if (!std::isfinite(model.driver_capacitance) || !std::isfinite(model.load_capacitance)
      || !std::isfinite(model.resistance) || model.driver_capacitance < 0.0
      || model.load_capacitance < 0.0 || model.resistance < 0.0
      || model.driver_capacitance + model.load_capacitance <= 0.0)
    return CsrDmpModel{};
  model.valid = true;
  std::array<std::vector<double>, 4> moments;
  for (auto& moment : moments)
    moment.resize(size, 0.0);
  std::vector<double> current(size);
  for (int order = 1; order < 4; ++order) {
    for (std::size_t i = 0; i < size; ++i)
      current[i] = finite(tree[i].capacitance_pf * (order == 1 ? 1.0 : moments[order - 1][i]));
    for (std::size_t i = size - 1; i > 0; --i)
      current[tree[i].parent] = finite(current[tree[i].parent] + current[i]);
    for (std::size_t i = 1; i < size; ++i)
      moments[order][i] = finite(moments[order][tree[i].parent] - tree[i].resistance_kohm * current[i]);
  }
  model.loads.resize(terminals.size());
  for (std::size_t t = 1; t < terminals.size(); ++t) {
    const auto i = terminals[t];
    model.loads[t] = reduceLoad(moments[1][i], moments[2][i], moments[3][i]);
  }
  return model;
}

std::vector<CsrRcFallbackLoad> reduceFallback(
    std::span<const CsrRcTreeNode> tree, std::span<const std::size_t> terminals, double& root_load)
{
  const auto size = tree.size();
  std::vector<double> load(size), delay(size, 0.0), load_delay(size), beta(size, 0.0);
  for (std::size_t i = 0; i < size; ++i)
    load[i] = tree[i].capacitance_pf;
  // Child DFS subtrees are accumulated in native adjacency order. Do not
  // reverse siblings when replacing the recursive native traversal.
  std::vector<std::vector<std::size_t>> children(size);
  for (std::size_t i = 1; i < size; ++i)
    children[tree[i].parent].push_back(i);
  for (std::size_t i = size; i-- > 0;)
    for (auto child : children[i])
      load[i] += load[child];
  root_load = finite(load.front());
  for (std::size_t i = 1; i < size; ++i)
    delay[i] = delay[tree[i].parent] + tree[i].resistance_kohm * load[i];
  for (std::size_t i = size; i-- > 0;) {
    load_delay[i] = tree[i].capacitance_pf * delay[i];
    for (auto child : children[i])
      load_delay[i] += load_delay[child];
  }
  for (std::size_t i = 1; i < size; ++i)
    beta[i] = beta[tree[i].parent] + tree[i].resistance_kohm * load_delay[i];
  std::vector<CsrRcFallbackLoad> result;
  result.reserve(terminals.size());
  for (auto node : terminals) {
    const double impulse = 2.0 * beta[node] - std::pow(delay[node], 2);
    const bool valid = std::isfinite(delay[node]) && std::isfinite(impulse);
    result.push_back({valid ? delay[node] : 0.0, valid ? std::max(0.0, impulse) : 0.0, valid});
  }
  return result;
}

}  // namespace

CsrParasiticModel buildCsrParasiticModel(std::span<const CsrRcTreeNode> tree, std::span<const std::size_t> terminals)
{
  validateTree(tree, terminals);
  CsrParasiticModel model;
  model.arnoldi = reduceArnoldi(tree, terminals);
  model.fallback = reduceFallback(tree, terminals, model.root_load);
  // DMP is a fallback. Its numeric failure cannot invalidate Arnoldi.
  try {
    model.dmp = reduceDmp(tree, terminals);
  } catch (const std::overflow_error&) {
    model.dmp = {};
  }
  return model;
}

}  // namespace ista::csr
