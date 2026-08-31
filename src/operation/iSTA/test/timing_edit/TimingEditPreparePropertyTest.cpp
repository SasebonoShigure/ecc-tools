// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "Database.hpp"
#include "Net.hpp"
#include "TimingEditNativeFixture.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditValidation.hpp"

namespace {

using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;

constexpr TimingEditSessionToken kSessionToken{919};

void require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

BaselineSemanticIdentity baselineFrom(Database& database,
                                      const CommittedEditIdentity& current)
{
  return {database.get_design_name(), current.current_fingerprints,
          current.canonical_pre_state};
}

TimingEditPrepareContext prepareContext(const BaselineSemanticIdentity& baseline,
                                        const CommittedEditIdentity& current)
{
  return {kSessionToken, baseline, current, editTestAnalysisContext()};
}

template <typename Callable>
std::string requireInvalidUnchanged(
    Database& database, TransactionGeneration generation,
    const CommittedEditIdentity& before, Callable&& callable,
    const std::string& message)
{
  try {
    callable();
  } catch (const std::invalid_argument& error) {
    const CommittedEditIdentity after
        = NativeTimingEditPreparer::captureCurrentIdentity(
            database, generation, editTestAnalysisContext(), kSessionToken);
    require(after == before, message + " changed exact native identity");
    return error.what();
  }
  throw std::runtime_error(message);
}

void testNanPlacementRejectsBeforeMutation()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  const TransactionGeneration generation{0};
  const CommittedEditIdentity current
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), generation, editTestAnalysisContext(),
          kSessionToken);
  const BaselineSemanticIdentity baseline
      = baselineFrom(fixture.database(), current);
  TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(fixture.database());
  DesignEditPlan plan = bufferInsertionPlan(
      fixture.database(), baseline, generation, editTestCreationToken());
  plan.edits.front().placement_inputs
      = PlacedInputs{std::numeric_limits<double>::quiet_NaN(), 0.0};
  const std::string error = requireInvalidUnchanged(
      fixture.database(), generation, current,
      [&] {
        static_cast<void>(NativeTimingEditPreparer::prepare(
            fixture.database(), plan, prepareContext(baseline, current),
            resolver));
      },
      "NaN placement reached native mutation preparation");
  require(error.find("placed coordinates must be finite") != std::string::npos,
          "NaN placement reported the wrong gate: " + error);
}

void testMaximumGenerationReportsOverflowWithoutMutation()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  constexpr TransactionGeneration generation{
      std::numeric_limits<std::uint64_t>::max()};
  const CommittedEditIdentity current
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), generation, editTestAnalysisContext(),
          kSessionToken);
  const BaselineSemanticIdentity baseline
      = baselineFrom(fixture.database(), current);
  TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(fixture.database());
  const DesignEditPlan plan = bufferInsertionPlan(
      fixture.database(), baseline, generation, editTestCreationToken());
  const TimingEditContractViolations violations = validateDesignEditPlan(plan);
  require(!violations.empty()
              && violations.front().code
                     == TimingEditContractCode::kCapacityOverflow,
          "maximum generation did not reach typed capacity_overflow");
  const std::string error
      = std::string(token(violations.front().code)) + ": "
        + requireInvalidUnchanged(
              fixture.database(), generation, current,
              [&] {
                static_cast<void>(NativeTimingEditPreparer::prepare(
                    fixture.database(), plan,
                    prepareContext(baseline, current), resolver));
              },
              "generation overflow reached native mutation preparation");
  require(error.find("overflow") != std::string::npos
              && error.find("generation cannot advance") != std::string::npos,
          "generation overflow reported the wrong gate: " + error);
}

void testGeneratedNameCollisionRejectsWithoutMutation()
{
  NativeEditFixture fixture = makeNativeEditFixture();
  const TransactionGeneration generation{0};
  const CreationToken creation_token = editTestCreationToken();
  const TokenMaterialization names = defaultTokenMaterialization(
      creation_token, nativeLibraryCellRef(fixture.database(), "BUF_X1"));
  Net collision;
  collision.set_net_name(names.output_net_name);
  require(fixture.database().get_net_map()
              .emplace(names.output_net_name, std::move(collision))
              .second,
          "collision fixture did not install the generated output-net name");
  const CommittedEditIdentity current
      = NativeTimingEditPreparer::captureCurrentIdentity(
          fixture.database(), generation, editTestAnalysisContext(),
          kSessionToken);
  const BaselineSemanticIdentity baseline
      = baselineFrom(fixture.database(), current);
  TimingEditResolverState resolver
      = TimingEditResolverState::bindBaseline(fixture.database());
  const DesignEditPlan plan = bufferInsertionPlan(
      fixture.database(), baseline, generation, creation_token);
  const std::string error = requireInvalidUnchanged(
      fixture.database(), generation, current,
      [&] {
        static_cast<void>(NativeTimingEditPreparer::prepare(
            fixture.database(), plan, prepareContext(baseline, current),
            resolver));
      },
      "generated native-name collision was accepted");
  require(error.find("collides with a live native name") != std::string::npos,
          "name collision reported the wrong prepare gate: " + error);
}

}  // namespace

int main()
{
  testNanPlacementRejectsBeforeMutation();
  testMaximumGenerationReportsOverflowWithoutMutation();
  testGeneratedNameCollisionRejectsWithoutMutation();
  return 0;
}
