// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "TimingEditTypes.hpp"
#include "json.hpp"

// Mechanical value encoding shared by the trace and replay wire formats.
// Their outer parsers retain duplicate-key and semantic validation.
namespace ista::timing_edit::json_detail {
class WireError : public std::runtime_error
{
 public:
  WireError(std::string field, std::string message) : std::runtime_error(message), field(std::move(field)) {}
  std::string field;
};
[[nodiscard]] BaselineSemanticIdentity baseline(const nlohmann::json&, std::string_view field);
[[nodiscard]] BufferInsertionMutation edit(const nlohmann::json&, std::string_view field);
[[nodiscard]] StableNodeRef nodeRef(const nlohmann::json&, std::string_view field);
[[nodiscard]] StableNetRef netRef(const nlohmann::json&, std::string_view field);
[[nodiscard]] LibraryCellRef libraryCell(const nlohmann::json&, std::string_view field);
[[nodiscard]] nlohmann::json toJson(const BaselineSemanticIdentity&);
[[nodiscard]] nlohmann::json toJson(const BufferInsertionMutation&);
[[nodiscard]] nlohmann::json toJson(const StableNodeRef&);
[[nodiscard]] nlohmann::json toJson(const StableNetRef&);
[[nodiscard]] nlohmann::json toJson(const LibraryCellRef&);
}  // namespace ista::timing_edit::json_detail
