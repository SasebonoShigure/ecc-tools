// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <fstream>
#include <iostream>
#include "TimingWorkloadBenchmark.hpp"
int main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string_view(argv[1]) == "build-info") {
      std::cout << ista::timing_dual_csr::test::incregpustaBuildInfo().dump() << '\n';
      return 0;
    }
    ista::test::benchmarkRequire(!ista::incregpusta_debug_checks_enabled, "benchmark requires ECC_INCREGPUSTA_DEBUG_CHECKS=OFF");
    ista::test::benchmarkRequire(argc == 14 && std::string_view(argv[1]) == "benchmark", "expected benchmark and six key/value options");
    if (ista::timing_dual_csr::test::incregpustaBuildInfo().at("build_config") != "Release")
      return 77;
    std::map<std::string, std::string> options;
    for (int i = 2; i < argc; i += 2)
      ista::test::benchmarkRequire(options.emplace(argv[i], argv[i + 1]).second, "duplicate option");
    auto result = ista::test::runTimingWorkloadBenchmark(options);
    std::ofstream output(options.at("--output"));
    output << result.dump() << '\n';
    output.close();
    ista::test::benchmarkRequire(bool(output), "output write failed");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
