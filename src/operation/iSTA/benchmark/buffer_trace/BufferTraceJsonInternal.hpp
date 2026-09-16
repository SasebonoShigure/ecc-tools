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

#include <algorithm>
#include <stdexcept>
#include <utility>
#include "BufferTraceTypes.hpp"
#include "json.hpp"

namespace ista::buffer_trace::json_detail {
using Json = nlohmann::json;
class WireError : public std::runtime_error
{
 public:
  WireError(BufferTraceContractCode code, std::string field, std::string message)
      : std::runtime_error(std::move(message)), code(code), field(std::move(field))
  {
  }
  BufferTraceContractCode code;
  std::string field;
};
inline void exactObject(const Json& value, std::string_view field, std::initializer_list<std::string_view> required,
                 std::initializer_list<std::string_view> optional = {})
{
  if (!value.is_object())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected object");
  for (const auto key : required)
    if (!value.contains(std::string(key)))
      throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field) + "." + std::string(key), "required field is absent");
  for (auto item = value.begin(); item != value.end(); ++item)
    if (std::find(required.begin(), required.end(), item.key()) == required.end()
        && std::find(optional.begin(), optional.end(), item.key()) == optional.end())
      throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field) + "." + item.key(), "unknown field");
}
inline std::string text(const Json& value, std::string_view field)
{
  if (!value.is_string())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected string");
  return value.get<std::string>();
}
template <typename T, typename Parse>
std::vector<T> vectorOf(const Json& value, std::string_view field, Parse parse)
{
  if (!value.is_array())
    throw WireError(BufferTraceContractCode::kInvalidValue, std::string(field), "expected array");
  std::vector<T> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index)
    result.push_back(parse(value[index], std::string(field) + "[" + std::to_string(index) + "]"));
  return result;
}
SourceArtifactRefs parseSources(const Json& sources);
Json sourcesToJson(const SourceArtifactRefs& sources);
}  // namespace ista::buffer_trace::json_detail
