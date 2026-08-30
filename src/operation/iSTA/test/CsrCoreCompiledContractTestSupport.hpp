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

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceValidator.hpp"

namespace ista::csr::test {

struct CsrCoreCompiledContract
{
  std::string_view case_id;
  std::string_view baseline;
  std::string_view recipe_op;
  std::string_view target;
  std::string_view operand;
  std::string_view action_token;
  std::string_view expected_validity;
  std::string_view expected_field;
  std::string_view expected_category;
  std::string_view expected_message;
};

inline std::array<std::string_view, 10> columns(const CsrCoreCompiledContract& contract)
{
  return {contract.case_id,           contract.baseline,       contract.recipe_op,         contract.target,          contract.operand, contract.action_token,
          contract.expected_validity, contract.expected_field, contract.expected_category, contract.expected_message};
}

inline void requirePrintableAscii(std::string_view value, std::string_view field)
{
  if (value.empty())
    throw std::runtime_error(std::string(field) + " must not be empty");
  for (char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte > 0x7e)
      throw std::runtime_error(std::string(field) + " must contain printable ASCII only");
  }
}

template <std::size_t Size>
void validateCoreCompiledContracts(const std::array<CsrCoreCompiledContract, Size>& contracts)
{
  for (std::size_t index = 0; index < contracts.size(); ++index) {
    const auto descriptor = columns(contracts[index]);
    for (std::size_t column = 0; column < descriptor.size(); ++column)
      requirePrintableAscii(descriptor[column], "compiled-contract column");
    if (contracts[index].expected_validity != "valid" && contracts[index].expected_validity != "invalid")
      throw std::runtime_error("compiled-contract validity must be valid or invalid");
    if (contracts[index].action_token != "-")
      throw std::runtime_error("core identity/FIFO cases must not carry observation actions");
    for (std::size_t previous = 0; previous < index; ++previous)
      if (contracts[previous].case_id == contracts[index].case_id)
        throw std::runtime_error("compiled-contract case_id must be unique within executable");
  }
}

template <std::size_t Size>
void emitCoreCompiledContracts(const std::array<CsrCoreCompiledContract, Size>& contracts)
{
  validateCoreCompiledContracts(contracts);
  for (const auto& contract : contracts) {
    const auto descriptor = columns(contract);
    for (std::size_t column = 0; column < descriptor.size(); ++column) {
      if (column != 0)
        std::cout.put('\t');
      std::cout << descriptor[column];
    }
    std::cout.put('\n');
  }
}

template <std::size_t Size>
bool handleCoreCompiledContractCli(int argc, char* argv[], const std::array<CsrCoreCompiledContract, Size>& contracts)
{
  validateCoreCompiledContracts(contracts);
  if (argc == 1)
    return false;
  if (argc == 2 && std::string_view(argv[1]) == "--list-case-contracts") {
    emitCoreCompiledContracts(contracts);
    return true;
  }
  throw std::runtime_error("usage: executable [--list-case-contracts]");
}

inline void requireCoreCompiledOutcome(const CsrCoreCompiledContract& contract, const std::vector<CsrContractViolation>& errors)
{
  if (contract.expected_validity == "valid") {
    if (contract.expected_field != "-" || contract.expected_category != "-" || contract.expected_message != "-")
      throw std::runtime_error(std::string(contract.case_id) + " valid descriptor must use dash expected fields");
    if (!errors.empty())
      throw std::runtime_error(std::string(contract.case_id) + " unexpectedly rejected at " + errors.front().field + ": " + errors.front().message);
    return;
  }
  if (contract.expected_category != "-")
    throw std::runtime_error(std::string(contract.case_id) + " core C++ descriptor category must be dash");
  if (errors.size() != 1)
    throw std::runtime_error(std::string(contract.case_id) + " must produce exactly one violation");
  if (errors.front().field != contract.expected_field || errors.front().message != contract.expected_message)
    throw std::runtime_error(std::string(contract.case_id) + " violation differs from its compiled descriptor");
}

}  // namespace ista::csr::test
