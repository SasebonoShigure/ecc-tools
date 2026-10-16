// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <iostream>
#include "BufferTraceReplayInput.hpp"
#include "DualCsrTargetedArtifactSupport.hpp"
#include "NativeFullTransactionChecks.hpp"

int main(int argc, char** argv)
{
  try {
    if (argc != 2)
      throw std::invalid_argument("expected one frozen trace path");
    const auto program = ista::buffer_trace::test::readReplayInput(argv[1], false);
    ista::timing_dual_csr::test::DualCsrTargetedArtifactSupport artifacts(program);
    ista::test::checkNativeLoaderFailure(artifacts.artifacts());
    ista::timing_edit::oracle::ProductionFreshSessionLoader::runNative(artifacts.artifacts(), [&](const auto& view) {
      ista::test::requireTimingOnlyFiles(artifacts.artifacts().run_directory);
      ista::test::checkNativeFullTransactions(view, program);
    });
    const auto temporary_root = artifacts.artifacts().run_directory.parent_path();
    artifacts.close();
    if (std::filesystem::exists(temporary_root))
      throw std::runtime_error("explicit artifact cleanup left the owned directory");
    artifacts.close();
    std::cout << "native full transactions and loader cleanup passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
