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

#include "CsrRealDifferentialProbeSupport.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "CsrFingerprint.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "DataManager.hpp"
#include "STAInterface.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#include "idm.h"
#pragma GCC diagnostic pop

namespace ista::csr::real_diff {
namespace {
namespace fs = std::filesystem;
constexpr std::array<std::string_view, 14> kFlags{
    "--descriptor-manifest",  "--tech-lef",     "--cell-lef",          "--liberty",   "--verilog", "--sdc",        "--top", "--case-name",
    "--expected-clocks-json", "--repeat-index", "--input-case-sha256", "--run-nonce", "--run-dir", "--result-temp"};
constexpr std::array<std::string_view, 5> kInputLabels{"tech_lef", "cell_lef", "liberty", "verilog", "sdc"};
[[noreturn]] void fail(std::string_view message)
{
  throw std::runtime_error(std::string(message));
}
[[noreturn]] void failErrno(std::string_view action)
{
  const int error = errno;
  throw std::runtime_error(std::string(action) + ": " + std::strerror(error));
}
bool lowerHex(std::string_view value, std::size_t size)
{
  return value.size() == size
         && std::all_of(value.begin(), value.end(), [](char item) { return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f'); });
}
std::string text(const Json& value, std::string_view label, bool allow_empty = false)
{
  if (!value.is_string()) {
    fail(std::string(label) + " must be a string");
  }
  std::string result = value.get<std::string>();
  if (!allow_empty && result.empty()) {
    fail(std::string(label) + " must be nonempty");
  }
  return result;
}
std::uint64_t positiveU64(const Json& value, std::string_view label)
{
  if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0)) {
    fail(std::string(label) + " must be a U64");
  }
  const std::uint64_t result = value.get<std::uint64_t>();
  if (result == 0) {
    fail(std::string(label) + " must be positive");
  }
  return result;
}
std::vector<std::string> stringArray(const Json& value, std::string_view label)
{
  if (!value.is_array()) {
    fail(std::string(label) + " must be an array");
  }
  std::vector<std::string> result;
  for (const Json& item : value) {
    result.push_back(text(item, label));
  }
  return result;
}
fs::path canonicalRegular(const fs::path& path, std::string_view label)
{
  if (!path.is_absolute() || fs::symlink_status(path).type() != fs::file_type::regular) {
    fail(std::string(label) + " must be an absolute non-symlink regular file");
  }
  const fs::path canonical = fs::canonical(path);
  if (path.lexically_normal() != canonical) {
    fail(std::string(label) + " must already be a canonical path");
  }
  return canonical;
}
void requirePrivateDirectory(const fs::path& path)
{
  if (!path.is_absolute() || fs::symlink_status(path).type() != fs::file_type::directory || fs::canonical(path) != path.lexically_normal()) {
    fail("run directory must be an absolute canonical non-symlink directory");
  }
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    failErrno("lstat run directory");
  }
  if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0777) != 0700) {
    fail("run directory ownership or mode differs");
  }
}
std::uint64_t decimalU64(std::string_view value)
{
  if (value.empty() || (value.size() > 1 && value.front() == '0')) {
    fail("repeat index is not canonical decimal U64");
  }
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    fail("repeat index is not canonical decimal U64");
  }
  return result;
}
fs::path inputPath(const ProbeOptions& options, std::string_view label)
{
  if (label == "tech_lef") {
    return options.tech_lef;
  }
  if (label == "cell_lef") {
    return options.cell_lef;
  }
  if (label == "liberty") {
    return options.liberty;
  }
  if (label == "verilog") {
    return options.verilog;
  }
  return options.sdc;
}
std::string bindInput(ProbeOptions& options, std::string_view label, const fs::path& expected)
{
  const fs::path supplied = inputPath(options, label);
  if (supplied != expected) {
    fail(std::string(label) + " path differs from descriptor manifest");
  }
  std::string digest = sha256File(expected);
  options.inputs.push_back({std::string(label), expected, digest});
  return digest;
}
void requireArcSelector(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"instance", "cell", "source_pin", "sink_pin", "owner_name", "library_source_port", "library_sink_port"}, label);
  for (std::string_view key : {"instance", "cell", "source_pin", "sink_pin", "owner_name", "library_source_port", "library_sink_port"}) {
    (void) text(value.at(std::string(key)), label);
  }
}
void requireNodeSelector(const Json& value, std::string_view label)
{
  requireExactKeys(value, {"kind", "full_name", "instance_name", "pin_name"}, label);
  const std::string kind = text(value.at("kind"), label);
  const std::string full = text(value.at("full_name"), label);
  const std::string instance = text(value.at("instance_name"), label, true);
  const std::string pin = text(value.at("pin_name"), label, true);
  if ((kind == "port" && (!instance.empty() || !pin.empty())) || (kind == "instance_pin" && (instance.empty() || pin.empty() || full != instance + ":" + pin))
      || (kind != "port" && kind != "instance_pin")) {
    fail(std::string(label) + " is not a canonical NodeKey descriptor");
  }
}
void appendArcSelector(CanonicalEncoder& encoder, const Json& value)
{
  for (std::string_view key : {"instance", "cell", "source_pin", "sink_pin", "owner_name", "library_source_port", "library_sink_port"}) {
    encoder.appendString(value.at(std::string(key)).get_ref<const std::string&>());
  }
}
void appendNodeSelector(CanonicalEncoder& encoder, const Json& value)
{
  for (std::string_view key : {"kind", "full_name", "instance_name", "pin_name"}) {
    encoder.appendString(value.at(std::string(key)).get_ref<const std::string&>());
  }
}
void requireClockArgument(ProbeOptions& options, const Json& expected)
{
  const std::vector<std::string> clocks = stringArray(expected, "expected_clocks");
  if (clocks.empty() || !std::is_sorted(clocks.begin(), clocks.end()) || std::adjacent_find(clocks.begin(), clocks.end()) != clocks.end()) {
    fail("expected clocks are not strict byte-sorted");
  }
  if (clocks != options.expected_clocks || expected.dump() != options.expected_clocks_json) {
    fail("expected clock argv differs from descriptor");
  }
}
std::string baselineFingerprint(const Json& descriptor, std::vector<std::pair<std::string, std::string>> inputs)
{
  auto key_bytes = [](const std::string& value) {
    CanonicalEncoder encoder;
    encoder.appendString(value);
    return encoder.bytes();
  };
  std::sort(inputs.begin(), inputs.end(), [&](const auto& lhs, const auto& rhs) { return key_bytes(lhs.first) < key_bytes(rhs.first); });
  CanonicalEncoder encoder;
  encoder.appendDomain("ista-csr-projection-input-case");
  for (std::string_view key : {"name", "top", "verilog", "sdc", "expected_clock"}) {
    encoder.appendString(descriptor.at(std::string(key)).get_ref<const std::string&>());
  }
  for (const auto& [path, digest] : inputs) {
    encoder.appendString(path);
    encoder.appendString(digest);
  }
  return csr::sha256Hex(encoder.bytes());
}
void validateBaselineManifest(ProbeOptions& options)
{
  const Json& root = options.descriptor;
  requireExactKeys(
      root, {"schema_version", "description", "source_baseline", "time_unit", "semantics", "tolerances", "fresh_process_repeats", "golden_directory", "cases"},
      "baseline manifest");
  requireExactKeys(root.at("source_baseline"), {"parent_branch", "parent_commit", "ecc_tools_branch", "ecc_tools_base_commit"}, "baseline source");
  requireExactKeys(
      root.at("semantics"),
      {"analysis_types", "transitions", "path_sources", "node_key", "arc_key", "missing_value", "infinite_value", "root_level", "propagation_scope"},
      "baseline semantics");
  requireExactKeys(root.at("tolerances"), {"absolute", "relative"}, "baseline tolerances");
  requireExactKeys(root.at("fresh_process_repeats"), {"ctest", "local"}, "baseline repeats");
  if (root.at("schema_version") != 1 || root.at("time_unit") != "ns" || !root.at("cases").is_array() || root.at("cases").size() != 2) {
    fail("baseline manifest contract differs");
  }
  const Json* selected = nullptr;
  for (const Json& item : root.at("cases")) {
    requireExactKeys(item, {"name", "top", "verilog", "sdc", "expected_clock", "required_artifacts"}, "baseline case");
    if (!item.at("required_artifacts").is_array()) {
      fail("baseline required_artifacts must be an array");
    }
    for (const Json& artifact : item.at("required_artifacts")) {
      (void) text(artifact, "required artifact");
    }
    if (text(item.at("name"), "baseline case name") == options.case_name) {
      selected = &item;
    }
  }
  if (selected == nullptr || options.case_name == "supported_branches") {
    fail("baseline manifest lacks selected case");
  }
  const Json& item = *selected;
  if (text(item.at("top"), "baseline top") != options.top || text(item.at("expected_clock"), "baseline clock") != "vclk") {
    fail("baseline descriptor differs from argv");
  }
  requireClockArgument(options, Json::array({"vclk"}));
  const std::array<std::pair<std::string_view, std::string>, 5> relatives{{
      {"tech_lef", "fixtures/stage0_tech.lef"},
      {"cell_lef", "fixtures/stage0_cells.lef"},
      {"liberty", "fixtures/stage0.lib"},
      {"verilog", text(item.at("verilog"), "baseline verilog")},
      {"sdc", text(item.at("sdc"), "baseline sdc")},
  }};
  std::vector<std::pair<std::string, std::string>> input_hashes;
  for (const auto& [label, relative] : relatives) {
    const fs::path path = canonicalRegular(options.descriptor_manifest.parent_path() / relative, label);
    input_hashes.emplace_back(relative, bindInput(options, label, path));
  }
  if (baselineFingerprint(item, std::move(input_hashes)) != options.input_case_sha256) {
    fail("baseline input-case SHA differs");
  }
  options.descriptor = {{"name", options.case_name}, {"top", options.top}, {"expected_clocks", options.expected_clocks}, {"extended", false}};
}
std::string extendedFingerprint(const Json& descriptor, const std::vector<ProbeInput>& inputs)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("real-differential-input-case");
  encoder.appendString(descriptor.at("name").get_ref<const std::string&>());
  encoder.appendString(descriptor.at("top").get_ref<const std::string&>());
  encoder.appendU64(descriptor.at("expected_clocks").size());
  for (const Json& clock : descriptor.at("expected_clocks")) {
    encoder.appendString(clock.get_ref<const std::string&>());
  }
  for (const ProbeInput& input : inputs) {
    const std::string relative = descriptor.at("inputs").at(input.label).at("path").get<std::string>();
    encoder.appendString(input.label);
    encoder.appendString(relative);
    encoder.appendString(input.sha256);
  }
  for (std::string_view key : {"non_unate_anchor", "disabled_anchor", "parallel_anchor"}) {
    appendArcSelector(encoder, descriptor.at(std::string(key)));
  }
  const Json& loop = descriptor.at("loop_disabled_witness");
  for (std::string_view key : {"source", "sink", "owner_name"}) {
    encoder.appendString(loop.at(std::string(key)).get_ref<const std::string&>());
  }
  encoder.appendU64(loop.at("minimum_count").get<std::uint64_t>());
  encoder.appendU64(descriptor.at("data_roots").size());
  for (const Json& root : descriptor.at("data_roots")) {
    appendNodeSelector(encoder, root);
  }
  encoder.appendU64(descriptor.at("clock_tags").size());
  for (const Json& tag : descriptor.at("clock_tags")) {
    encoder.appendString(tag.get_ref<const std::string&>());
  }
  const Json& near = descriptor.at("near_tie");
  appendNodeSelector(encoder, near.at("join_node"));
  appendArcSelector(encoder, near.at("row_first_arc"));
  appendArcSelector(encoder, near.at("row_second_arc"));
  for (std::string_view key : {"analysis", "path_source", "transition", "clock_tag"}) {
    encoder.appendString(near.at(std::string(key)).get_ref<const std::string&>());
  }
  appendNodeSelector(encoder, near.at("secondary_root"));
  encoder.appendString(near.at("secondary_clock_tag").get_ref<const std::string&>());
  encoder.appendString("3e112e0be826d695");
  encoder.appendString("3eb0c6f7a0b5ed8d");
  return csr::sha256Hex(encoder.bytes());
}
void validateExtendedManifest(ProbeOptions& options)
{
  Json& root = options.descriptor;
  requireExactKeys(root, {"schema", "domain", "baseline_manifest", "tolerances", "extended_cases", "coverage_claims"},
                   "extended manifest");
  const Json& tolerances = root.at("tolerances");
  requireExactKeys(tolerances, {"absolute", "relative"}, "extended tolerances");
  const Json expected_claims
      = {{"native_backed",
          Json::array({"chain", "diamond", "non_unate", "parallel", "disabled", "loop_disabled", "multi_root", "clock_tag", "near_tie", "post_scatter_rerun"})},
         {"synthetic_only", Json::array({"parasitic_net", "sequential_instance", "sequential_launch", "timing_check", "crpr", "propagated_clock",
                                         "clock_tree_fanout", "clock_gating_or_divider", "conditional_timing_arc", "case_analysis"})},
         {"native_fatal", Json::array({"clock_tree_overlap", "multi_driver"})},
         {"not_claimed", Json::array({"sequential_native_parity", "crpr_native_parity", "mutation", "generation_gt_zero", "cuda", "dual_csr", "speedup"})}};
  requireExactKeys(root.at("coverage_claims"), {"native_backed", "synthetic_only", "native_fatal", "not_claimed"}, "coverage claims");
  const fs::path baseline
      = canonicalRegular((options.descriptor_manifest.parent_path() / "../baseline/manifest.json").lexically_normal(), "referenced baseline manifest");
  if (root.at("schema") != "ista-csr-real-differential-manifest/1.0.0" || root.at("domain") != "ista-csr-real-differential-input-case/1.0.0"
      || root.at("baseline_manifest") != "../baseline/manifest.json" || root.at("coverage_claims") != expected_claims || !tolerances.at("absolute").is_number()
      || !tolerances.at("relative").is_number() || std::bit_cast<std::uint64_t>(tolerances.at("absolute").get<double>()) != 0x3e112e0be826d695ULL
      || std::bit_cast<std::uint64_t>(tolerances.at("relative").get<double>()) != 0x3e112e0be826d695ULL || !root.at("extended_cases").is_array()
      || root.at("extended_cases").size() != 1) {
    fail("extended manifest frozen identity differs");
  }
  Json descriptor = root.at("extended_cases").at(0);
  requireExactKeys(descriptor,
                   {"name", "top", "expected_clocks", "inputs", "non_unate_anchor", "disabled_anchor", "parallel_anchor", "loop_disabled_witness", "data_roots",
                    "clock_tags", "near_tie"},
                   "extended descriptor");
  if (descriptor.at("name") != "supported_branches" || descriptor.at("top") != "supported_branches" || options.case_name != "supported_branches"
      || options.top != "supported_branches" || descriptor.at("expected_clocks") != Json::array({"vclk_a", "vclk_b"})) {
    fail("extended name/top differs");
  }
  requireClockArgument(options, descriptor.at("expected_clocks"));
  requireExactKeys(descriptor.at("inputs"), {"tech_lef", "cell_lef", "liberty", "verilog", "sdc"}, "extended inputs");
  constexpr std::array<std::string_view, 5> relative_paths{"../baseline/fixtures/stage0_tech.lef", "fixtures/supported_branches_cells.lef",
                                                           "fixtures/supported_branches.lib", "fixtures/supported_branches.v",
                                                           "fixtures/supported_branches.sdc"};
  for (std::size_t index = 0; index < kInputLabels.size(); ++index) {
    const std::string_view label = kInputLabels[index];
    const Json& input = descriptor.at("inputs").at(std::string(label));
    requireExactKeys(input, {"path"}, "extended input");
    const std::string relative = text(input.at("path"), "extended relative input");
    const fs::path candidate = canonicalRegular((options.descriptor_manifest.parent_path() / relative).lexically_normal(), label);
    if (relative != relative_paths[index] || (index == 0 && candidate != baseline.parent_path() / "fixtures/stage0_tech.lef")) {
      fail("extended input frozen path differs");
    }
    bindInput(options, label, candidate);
  }
  constexpr std::array<std::string_view, 3> anchor_keys{"non_unate_anchor", "disabled_anchor", "parallel_anchor"};
  constexpr std::array<std::string_view, 3> anchor_instances{"u_nonunate", "u_disabled", "u_parallel"};
  constexpr std::array<std::string_view, 3> anchor_cells{"NONUNATE2_X1", "DISABLE_X1", "PARALLEL_X1"};
  for (std::size_t index = 0; index < anchor_keys.size(); ++index) {
    const Json& anchor = descriptor.at(std::string(anchor_keys[index]));
    requireArcSelector(anchor, anchor_keys[index]);
    if (anchor.at("instance") != anchor_instances[index] || anchor.at("cell") != anchor_cells[index] || anchor.at("owner_name") != anchor_instances[index]
        || anchor.at("source_pin") != "A" || anchor.at("sink_pin") != "Y" || anchor.at("library_source_port") != "A" || anchor.at("library_sink_port") != "Y") {
      fail("extended anchor branch identity differs");
    }
  }
  const Json& loop = descriptor.at("loop_disabled_witness");
  requireExactKeys(loop, {"source", "sink", "owner_name", "minimum_count"}, "loop witness");
  for (std::string_view key : {"source", "sink", "owner_name"}) {
    (void) text(loop.at(std::string(key)), "loop witness");
  }
  (void) positiveU64(loop.at("minimum_count"), "loop minimum");
  if (loop != Json{{"source", "u_loop_b:Y"}, {"sink", "u_loop_a:A"}, {"owner_name", "loop_b"}, {"minimum_count", 1}}) {
    fail("loop-disabled frozen witness differs");
  }
  if (!descriptor.at("data_roots").is_array() || descriptor.at("data_roots").size() != 2) {
    fail("extended data roots differ");
  }
  for (const Json& node : descriptor.at("data_roots")) {
    requireNodeSelector(node, "data root");
  }
  if (descriptor.at("data_roots")
      != Json::array({{{"kind", "port"}, {"full_name", "data_a"}, {"instance_name", ""}, {"pin_name", ""}},
                      {{"kind", "port"}, {"full_name", "data_b"}, {"instance_name", ""}, {"pin_name", ""}}})) {
    fail("extended data root identities differ");
  }
  if (stringArray(descriptor.at("clock_tags"), "clock tags") != options.expected_clocks) {
    fail("clock tags differ from expected clocks");
  }
  const Json& near = descriptor.at("near_tie");
  requireExactKeys(near,
                   {"join_node", "row_first_arc", "row_second_arc", "analysis", "path_source", "transition", "clock_tag", "secondary_root",
                    "secondary_clock_tag", "lower_exclusive", "upper_exclusive"},
                   "near tie");
  requireNodeSelector(near.at("join_node"), "near join");
  requireNodeSelector(near.at("secondary_root"), "near secondary root");
  requireArcSelector(near.at("row_first_arc"), "near first Arc");
  requireArcSelector(near.at("row_second_arc"), "near second Arc");
  for (std::string_view key : {"analysis", "path_source", "transition", "clock_tag", "secondary_clock_tag"}) {
    (void) text(near.at(std::string(key)), "near tie string");
  }
  const auto near_row = [](const Json& row, std::string_view source) {
    return row.at("instance") == "u_join" && row.at("cell") == "JOIN2_X1" && row.at("source_pin") == source && row.at("sink_pin") == "Y"
           && row.at("owner_name") == "u_join" && row.at("library_source_port") == source && row.at("library_sink_port") == "Y";
  };
  if (near.at("join_node") != Json{{"kind", "instance_pin"}, {"full_name", "u_join:Y"}, {"instance_name", "u_join"}, {"pin_name", "Y"}}
      || !near_row(near.at("row_first_arc"), "A") || !near_row(near.at("row_second_arc"), "B") || near.at("analysis") != "max"
      || near.at("path_source") != "input" || near.at("transition") != "rise" || near.at("clock_tag") != descriptor.at("clock_tags").at(0)
      || near.at("secondary_root") != descriptor.at("data_roots").at(1) || near.at("secondary_clock_tag") != descriptor.at("clock_tags").at(1)) {
    fail("near-tie branch identity or lane relation differs");
  }
  if (!near.at("lower_exclusive").is_number() || !near.at("upper_exclusive").is_number()
      || std::bit_cast<std::uint64_t>(near.at("lower_exclusive").get<double>()) != 0x3e112e0be826d695ULL
      || std::bit_cast<std::uint64_t>(near.at("upper_exclusive").get<double>()) != 0x3eb0c6f7a0b5ed8dULL) {
    fail("near-tie binary64 bounds differ");
  }
  if (extendedFingerprint(descriptor, options.inputs) != options.input_case_sha256) {
    fail("extended input-case SHA differs");
  }
  options.descriptor = std::move(descriptor);
}
void validateManifest(ProbeOptions& options)
{
  const std::vector<std::uint8_t> bytes = readFileBytes(options.descriptor_manifest);
  options.descriptor = parseStrictJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "descriptor manifest");
  if (options.case_name == "supported_branches") {
    validateExtendedManifest(options);
  } else {
    validateBaselineManifest(options);
  }
}
}  // namespace
ProbeOptions parseProbeOptions(int argc, char* argv[], std::string_view lane)
{
  if ((lane != "oracle" && lane != "reference") || argc != 1 + static_cast<int>(2 * kFlags.size())) {
    fail("probe lane or argv cardinality differs");
  }
  ProbeOptions options;
  options.lane = std::string(lane);
  options.executable = canonicalRegular(fs::path(argv[0]), "executable");
  options.canonical_argv.push_back(options.executable.string());
  std::array<std::string, kFlags.size()> values;
  for (std::size_t index = 0; index < kFlags.size(); ++index) {
    if (argv[1 + 2 * index] == nullptr || argv[2 + 2 * index] == nullptr || argv[1 + 2 * index] != kFlags[index]) {
      fail("probe argv flag order differs");
    }
    values[index] = argv[2 + 2 * index];
    options.canonical_argv.emplace_back(kFlags[index]);
    options.canonical_argv.push_back(values[index]);
  }
  options.descriptor_manifest = canonicalRegular(values[0], "descriptor manifest");
  options.tech_lef = canonicalRegular(values[1], "technology LEF");
  options.cell_lef = canonicalRegular(values[2], "cell LEF");
  options.liberty = canonicalRegular(values[3], "Liberty");
  options.verilog = canonicalRegular(values[4], "Verilog");
  options.sdc = canonicalRegular(values[5], "SDC");
  options.top = values[6];
  options.case_name = values[7];
  options.expected_clocks_json = values[8];
  const Json clocks = parseStrictJson(options.expected_clocks_json, "expected clocks argv");
  options.expected_clocks = stringArray(clocks, "expected clocks argv");
  options.repeat_index = decimalU64(values[9]);
  options.input_case_sha256 = values[10];
  options.run_nonce = values[11];
  options.run_dir = fs::path(values[12]).lexically_normal();
  options.result_temp_name = values[13];
  if (options.top.empty() || !lowerHex(options.input_case_sha256, 64) || !lowerHex(options.run_nonce, 32)) {
    fail("top, input-case SHA, or nonce is malformed");
  }
  requirePrivateDirectory(options.run_dir);
  if (fs::canonical(fs::current_path()) != options.run_dir) {
    fail("probe cwd must equal lane-private run directory");
  }
  const std::string expected_temp = "." + options.lane + "-result-" + options.run_nonce + ".tmp";
  if (options.result_temp_name != expected_temp || fs::path(options.result_temp_name).filename() != options.result_temp_name) {
    fail("result temp basename differs");
  }
  options.result_temp = options.run_dir / options.result_temp_name;
  std::error_code error;
  if (fs::symlink_status(options.result_temp, error).type() != fs::file_type::not_found || (error && error != std::errc::no_such_file_or_directory)) {
    fail("result temp already exists or cannot be inspected");
  }
  validateManifest(options);
  return options;
}
Json makeArgvReceipt(const ProbeOptions& options)
{
  Json inputs = Json::array();
  for (const ProbeInput& input : options.inputs) {
    inputs.push_back({{"label", input.label}, {"path", input.path.string()}});
  }
  return {{"argv", options.canonical_argv},
          {"cwd", fs::canonical(fs::current_path()).string()},
          {"descriptor_manifest", {{"path", options.descriptor_manifest.string()}}},
          {"executable", options.executable.string()},
          {"resolved_inputs", std::move(inputs)},
          {"run_dir", options.run_dir.string()},
          {"result_temp", options.result_temp.string()}};
}
const StaAnalysisContext& probeAnalysisContext()
{
  static const StaAnalysisContext context{"slow", "25", "__no_rc_corner__"};
  return context;
}
std::map<std::string, std::any> makeProbeConfig(const ProbeOptions& options)
{
  return {{"-temp_directory_path", (options.run_dir / "sta").string()},
          {"-thread_number", std::int32_t{1}},
          {"-output_timing_reports", std::int32_t{1}},
          {"-output_timing_features", std::int32_t{1}},
          {"-timing_path_limit", std::int32_t{20}},
          {"-max_paths", std::int32_t{20}},
          {"-nworst", std::int32_t{20}},
          {"-timing_corner", std::string{}}};
}
void loadProbeDesign(const ProbeOptions& options)
{
  if (!dmInst->readLef(std::vector<std::string>{options.tech_lef.string()}, true) || !dmInst->readLef(std::vector<std::string>{options.cell_lef.string()})) {
    fail("failed to read LEF");
  }
  dmInst->get_config().set_sdc_path(options.sdc.string());
  if (!dmInst->readVerilog(options.verilog.string(), options.top) || !dmInst->readLib(std::vector<std::string>{options.liberty.string()})) {
    fail("failed to read Verilog or Liberty");
  }
}
std::vector<std::uint8_t> readFileBytes(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail("failed to open input file");
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) {
    fail("failed to size input file");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
    fail("failed to read input file");
  }
  return bytes;
}
std::string sha256File(const fs::path& path)
{
  return csr::sha256Hex(readFileBytes(path));
}
std::string bytesToHex(const std::vector<std::uint8_t>& bytes)
{
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(2 * bytes.size());
  for (std::uint8_t byte : bytes) {
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}
std::vector<std::uint8_t> hexToBytes(std::string_view bytes, std::string_view label)
{
  if (bytes.size() % 2 != 0 || !lowerHex(bytes, bytes.size())) {
    fail(std::string(label) + " is not lowercase even hex");
  }
  std::vector<std::uint8_t> result;
  result.reserve(bytes.size() / 2);
  auto nibble = [](char value) -> std::uint8_t { return value <= '9' ? value - '0' : value - 'a' + 10; };
  for (std::size_t index = 0; index < bytes.size(); index += 2) {
    result.push_back(static_cast<std::uint8_t>((nibble(bytes[index]) << 4U) | nibble(bytes[index + 1])));
  }
  return result;
}
void writeProbeResultExclusive(const ProbeOptions& options, const Json& result)
{
  int directory = ::open(options.run_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    failErrno("open run directory");
  }
  int output = -1;
  bool created = false;
  try {
    struct stat directory_status{};
    if (::fstat(directory, &directory_status) != 0 || !S_ISDIR(directory_status.st_mode) || directory_status.st_uid != ::geteuid()
        || (directory_status.st_mode & 0777) != 0700) {
      fail("opened run directory identity differs");
    }
    output = ::openat(directory, options.result_temp_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
      failErrno("exclusive create result temp");
    }
    created = true;
    if (::fchmod(output, 0600) != 0) {
      failErrno("fchmod result temp");
    }
    const std::string bytes = result.dump() + "\n";
    std::size_t written = 0;
    while (written < bytes.size()) {
      const ssize_t count = ::write(output, bytes.data() + written, bytes.size() - written);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        failErrno("write result temp");
      }
      written += static_cast<std::size_t>(count);
    }
    struct stat status{};
    if (::fsync(output) != 0 || ::fstat(output, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0777) != 0600
        || status.st_nlink != 1) {
      fail("result temp fsync or inode contract differs");
    }
    if (::close(output) != 0) {
      output = -1;
      failErrno("close result temp");
    }
    output = -1;
    if (::fsync(directory) != 0) {
      failErrno("fsync run directory");
    }
    if (::close(directory) != 0) {
      directory = -1;
      failErrno("close run directory");
    }
  } catch (...) {
    if (output >= 0) {
      (void) ::close(output);
    }
    if (created) {
      (void) ::unlinkat(directory, options.result_temp_name.c_str(), 0);
      (void) ::fsync(directory);
    }
    if (directory >= 0) {
      (void) ::close(directory);
    }
    throw;
  }
}
void destroyStaInterfaceAfterFailure() noexcept
{
  try {
    STAInterface::destroyInst();
  } catch (...) {
  }
}
}  // namespace ista::csr::real_diff
