// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "NativeTimingEditPreparer.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit::test {

class NativeEditFixture final
{
 public:
  NativeEditFixture();
  ~NativeEditFixture();
  NativeEditFixture(NativeEditFixture&&) noexcept;
  NativeEditFixture& operator=(NativeEditFixture&&) noexcept;

  NativeEditFixture(const NativeEditFixture&) = delete;
  NativeEditFixture& operator=(const NativeEditFixture&) = delete;

  [[nodiscard]] Database& database();
  [[nodiscard]] const Database& database() const;

 private:
  std::unique_ptr<Database> _database;
};

[[nodiscard]] NativeEditFixture makeNativeEditFixture(bool parallel_baseline_arcs = false);
[[nodiscard]] csr::StaAnalysisContext editTestAnalysisContext();
[[nodiscard]] LibraryCellRef nativeLibraryCellRef(const Database& database, const std::string& cell_name);
[[nodiscard]] BaselineSemanticIdentity baselineIdentity(Database& database, TransactionGeneration generation,
                                                        TimingEditSessionToken session_token);
[[nodiscard]] CreationToken editTestCreationToken(std::uint64_t batch_ordinal = 1, std::uint64_t accepted_ordinal = 0);
[[nodiscard]] DesignEditPlan bufferInsertionPlan(const Database& database, const BaselineSemanticIdentity& baseline,
                                                 TransactionGeneration generation, CreationToken creation_token);

}  // namespace ista::timing_edit::test
