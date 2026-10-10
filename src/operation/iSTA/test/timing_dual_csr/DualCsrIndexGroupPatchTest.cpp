// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <iostream>
#include <stdexcept>
#include "DualCsrIndexGroupPatchInternal.hpp"

using namespace ista::timing_dual_csr::detail;
struct Group {
  int key{};
  std::vector<int> active_arc_ids;
  friend bool operator==(const Group&, const Group&) = default;
};
void require(bool value) { if (!value) throw std::runtime_error("group patch differs from reference or rollback"); }
int main() {
  // All keep/remove/replace/insert combinations, including several inserts at one old position.
  for (unsigned pattern = 0; pattern != 65536; ++pattern) {
    unsigned choices = pattern;
    std::vector<Group> groups, expected;
    std::vector<GroupEdit<Group>> edits;
    for (int key = 0; key != 8; ++key) {
      const unsigned action = choices & 3U; choices >>= 2;
      if (action != 0) groups.push_back({key, {key}});
      if (action != 1) expected.push_back({key, action == 3 ? std::vector<int>{key} : std::vector<int>{key, 99}});
    }
    const auto before = groups;
    for (int key = 0; key != 8; ++key) {
      const unsigned action = (pattern >> (key * 2)) & 3U;
      if (action == 3) continue;
      prepareGroupEdit(groups, Group{key, action == 1 ? std::vector<int>{} : std::vector<int>{key,99}},
                       std::less<>{}, &Group::key, edits);
    }
    reserveGroupEdits(groups, edits);
    const auto capacity = groups.capacity();
    applyGroups(groups, edits);
    require(groups == expected && groups.capacity() == capacity);
    undoGroups(groups, edits);
    require(groups == before && groups.capacity() == capacity);
  }
  std::cout << "all batch insertion/removal/replacement and rollback combinations passed\n";
}
