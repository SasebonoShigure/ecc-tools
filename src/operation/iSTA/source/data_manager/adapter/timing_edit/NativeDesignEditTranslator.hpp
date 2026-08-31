// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <vector>

#include "TimingEditTypes.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit {

class TimingEditResolverState;

struct NativeTranslationResult
{
  EditDisposition disposition{EditDisposition::kSupported};
  std::vector<std::string> reasons;

  [[nodiscard]] bool applied() const noexcept { return disposition == EditDisposition::kSupported; }
  friend bool operator==(const NativeTranslationResult&, const NativeTranslationResult&) = default;
};

class NativeDesignEditTranslator final
{
 public:
  // Applies only normalized semantic edits to raw Instance/Pin/Net inputs. The
  // target remains detached; graph arcs are exclusively rebuilt by GraphBuilder.
  [[nodiscard]] static NativeTranslationResult applyNormalizedBatch(Database& database, const PreparedBatch& batch,
                                                                    const TimingEditResolverState& staged_resolver);
};

}  // namespace ista::timing_edit
