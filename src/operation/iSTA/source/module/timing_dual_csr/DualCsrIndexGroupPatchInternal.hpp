// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ista::timing_dual_csr::detail {
template <typename T>
struct GroupEdit
{
  std::size_t position;
  std::size_t after_position;
  bool existed;
  bool replacement_present;
  T value;
};

template <typename T, typename Less, typename Key>
void prepareGroupEdit(const std::vector<T>& groups, T replacement, Less less, Key key,
                      std::vector<GroupEdit<T>>& edits)
{
  const auto& wanted = replacement.*key;
  const auto found = std::ranges::lower_bound(groups, wanted, less, key);
  const bool existed = found != groups.end() && !less(wanted, (*found).*key) && !less((*found).*key, wanted);
  const bool present = !replacement.active_arc_ids.empty();
  if (existed || present)
    edits.push_back({static_cast<std::size_t>(found - groups.begin()), 0, existed, present, std::move(replacement)});
}

template <typename T>
void reserveGroupEdits(std::vector<T>& groups, std::vector<GroupEdit<T>>& edits)
{
  std::ranges::stable_sort(edits, {}, &GroupEdit<T>::position);
  std::size_t inserted = 0, removed = 0;
  for (auto& edit : edits) {
    edit.after_position = edit.position + inserted - removed;
    inserted += !edit.existed;
    removed += !edit.replacement_present;
  }
  const auto additions = std::ranges::count_if(edits, [](const auto& value) { return !value.existed; });
  if (static_cast<std::size_t>(additions) > groups.max_size() - groups.size())
    throw std::length_error("Dual-CSR index group capacity exhausted");
  const auto required = groups.size() + additions;
  if (required > groups.capacity())
    groups.reserve(std::max(required, groups.capacity() > groups.max_size() / 2
                                         ? groups.max_size() : std::max<std::size_t>(1, groups.capacity() * 2)));
}

template <typename T>
void applyGroups(std::vector<T>& groups, std::vector<GroupEdit<T>>& edits) noexcept
{
  static_assert(std::is_nothrow_default_constructible_v<T>
                && std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
  std::size_t read = 0, write = 0, additions = 0;
  // Compact removals once, keeping only touched old values for rollback.
  for (auto& edit : edits) {
    if (!edit.existed) {
      ++additions;
      continue;
    }
    std::swap(groups[edit.position], edit.value);
    if (!edit.replacement_present) {
      const auto count = edit.position - read;
      if (write != read)
        std::move(groups.begin() + read, groups.begin() + edit.position, groups.begin() + write);
      write += count;
      read = edit.position + 1;
    }
  }
  if (write != read)
    std::move(groups.begin() + read, groups.end(), groups.begin() + write);
  const auto compact_size = write + groups.size() - read;
  groups.resize(compact_size);
  groups.resize(compact_size + additions);
  read = compact_size;
  write = groups.size();
  // Expand from the back so each surviving group moves at most once per pass.
  for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
    if (it->existed) continue;
    const auto count = write - it->after_position - 1;
    if (count != 0)
      std::move_backward(groups.begin() + read - count, groups.begin() + read, groups.begin() + write);
    read -= count;
    groups[it->after_position] = std::move(it->value);
    write = it->after_position;
  }
  for (auto& edit : edits) {
    std::swap(edit.position, edit.after_position);
    std::swap(edit.existed, edit.replacement_present);
  }
}

template <typename T>
void undoGroups(std::vector<T>& groups, std::vector<GroupEdit<T>>& edits) noexcept
{
  applyGroups(groups, edits);
}

}  // namespace ista::timing_dual_csr::detail
