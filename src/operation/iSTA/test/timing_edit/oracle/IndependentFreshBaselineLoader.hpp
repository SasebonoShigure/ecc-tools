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
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "CsrReferencePrimitives.hpp"

namespace ista {
class Database;
}

namespace ista::csr {
struct CsrReferenceInput;
}

namespace ista::timing_edit::oracle {

struct FreshBaselineArtifacts
{
  std::filesystem::path tech_lef;
  std::vector<std::filesystem::path> cell_lefs;
  std::vector<std::filesystem::path> liberties;
  std::filesystem::path verilog;
  std::filesystem::path sdc;
  std::filesystem::path run_directory;
  std::string top;
  csr::StaAnalysisContext analysis_context;
};

// Mechanical source ordering only; targeted and oracle execution stay separate.
struct FreshArtifactFile
{
  std::string role;
  std::filesystem::path path;
  std::string logical_name;
};

inline std::vector<FreshArtifactFile> freshArtifactFiles(const FreshBaselineArtifacts& artifacts)
{
  std::vector<FreshArtifactFile> files{{"tech_lef", artifacts.tech_lef, artifacts.tech_lef.filename().string()}};
  const auto append = [&](const auto& paths, const std::string& role) {
    for (std::size_t index = 0; index < paths.size(); ++index)
      files.push_back({role, paths[index], std::to_string(index) + "-" + paths[index].filename().string()});
  };
  append(artifacts.cell_lefs, "cell_lef");
  append(artifacts.liberties, "liberty");
  files.push_back({"verilog", artifacts.verilog, artifacts.verilog.filename().string()});
  files.push_back({"sdc", artifacts.sdc, artifacts.sdc.filename().string()});
  return files;
}

inline std::vector<std::string> freshArtifactPathStrings(const std::vector<std::filesystem::path>& paths)
{
  std::vector<std::string> result;
  result.reserve(paths.size());
  for (const auto& path : paths)
    result.push_back(path.string());
  return result;
}

struct FreshArtifactEvidence
{
  std::string role;
  std::string logical_name;
  std::string content_sha256;

  friend bool operator==(const FreshArtifactEvidence&, const FreshArtifactEvidence&) = default;
};

struct FreshBaselineView
{
  Database& database;
  const csr::CsrReferenceInput& reference_input;
  std::uint64_t session_token;
  const std::vector<FreshArtifactEvidence>& artifact_evidence;
};

using FreshBaselineWork = std::function<void(const FreshBaselineView&)>;

// This is the oracle target's only interface/source/IDM/CSR-adapter layer. It
// loads immutable source artifacts into a new process-local session, runs the
// authoritative native CPU/full baseline, and lends the guarded Database to a
// callback. It deliberately has no edit vocabulary.
class IndependentFreshBaselineLoader final
{
 public:
  static void run(const FreshBaselineArtifacts& artifacts, const FreshBaselineWork& work);
};

}  // namespace ista::timing_edit::oracle
