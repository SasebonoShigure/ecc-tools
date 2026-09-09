// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#pragma once
#include "TimingEditResolver.hpp"
namespace ista::timing_edit::detail {
[[nodiscard]] std::uint64_t nextNativeInstanceId(const Database& database);
void applyNativeBufferInsertion(Database& database, const BufferInsertionMutation& edit, const TimingEditResolverState& resolver, std::uint64_t instance_id);
}  // namespace ista::timing_edit::detail
