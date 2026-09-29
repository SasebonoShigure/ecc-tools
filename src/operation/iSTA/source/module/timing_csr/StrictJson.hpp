// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "json.hpp"

namespace ista::json_detail {
class DuplicateJsonKey : public std::invalid_argument
{
 public:
  explicit DuplicateJsonKey(const std::string& key) : std::invalid_argument("duplicate object key: " + key) {}
};

// The DOM callback parser scans the parent array at every object end, making
// arrays of objects quadratic. Check keys with SAX, then use the ordinary DOM
// parser. Only the keys of currently open objects are retained by this pass.
struct DuplicateKeySax
{
  using Json = nlohmann::json;
  std::vector<std::set<std::string>> objects;
  bool null() { return true; }
  bool boolean(bool) { return true; }
  bool number_integer(Json::number_integer_t) { return true; }
  bool number_unsigned(Json::number_unsigned_t) { return true; }
  bool number_float(Json::number_float_t, const std::string&) { return true; }
  bool string(std::string&) { return true; }
  bool binary(Json::binary_t&) { return true; }
  bool start_array(std::size_t) { return true; }
  bool end_array() { return true; }
  bool start_object(std::size_t) { objects.emplace_back(); return true; }
  bool end_object() { objects.pop_back(); return true; }
  bool key(std::string& value)
  {
    if (!objects.back().insert(value).second) throw DuplicateJsonKey(value);
    return true;
  }
  bool parse_error(std::size_t, const std::string&, const Json::exception&) { return false; }
};

inline nlohmann::json parseUniqueKeyJson(std::string_view bytes, bool ignore_comments = false)
{
  using Json = nlohmann::json;
  DuplicateKeySax keys;
  // The DOM parse below supplies the original syntax exception if SAX rejects.
  static_cast<void>(Json::sax_parse(bytes.begin(), bytes.end(), &keys, Json::input_format_t::json, true, ignore_comments));
  return Json::parse(bytes.begin(), bytes.end(), nullptr, true, ignore_comments);
}
}  // namespace ista::json_detail
