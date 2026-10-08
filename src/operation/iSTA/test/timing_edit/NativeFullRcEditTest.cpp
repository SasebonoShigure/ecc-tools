// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <cmath>
#include <stdexcept>
#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "DataManager.hpp"
#include "NativeFullEditPreparer.hpp"
#include "StaTimingEditSessionProbeAccess.hpp"
#include "TimingEditTransactionNativeSupport.hpp"
#include "TimingEditValidation.hpp"
#include "TimingEditRcFixture.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace {
using namespace ista;
using namespace ista::timing_edit;
using namespace ista::timing_edit::test;
void require(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}

}

void testNativeFullRcTransactions()
{
  installNativeTransactionBaseline();
  GuardedTestSession session;
  auto status = StaTimingEditSessionProbeAccess::runWithSingleOperationGuard(
      session.session(), [&](std::uint64_t, StaCsrSessionContext& context, const NativeExecutionAuthority& authority) {
        requireReleasedSnapshotForNativeMutation(context, authority.sessionToken());
        auto& database = STADM.getDatabase();
        auto& library = database.get_parasitic_library();
        library.set_capacitive_unit("PF");
        library.set_resistance_unit("OHM");
        wire(library.get_net_map(), "n0", "in", "out", 125.0);
        {
          auto initial = NativeEditTransaction::prepareFull(authority);
          NativeCandidateExecutor::executeFull(initial);
          initial.commit();
        }
        auto rc_context = editTestAnalysisContext();
        rc_context.rc_corner = "pre_route";
        auto exported = csr::exportCsrReferenceInputCandidate(database, rc_context, authority.sessionToken().value());
        require(std::holds_alternative<csr::CsrReferenceInput>(exported), "RC baseline export failed");
        const auto identity = NativeTimingEditPreparer::identityFromInput(
            database, std::get<csr::CsrReferenceInput>(exported), TransactionGeneration{0});
        const BaselineSemanticIdentity baseline{database.get_design_name(), identity.current_fingerprints,
                                               identity.canonical_pre_state};
        auto state = NativeFullEditPreparer::bindBaseline(
            database, std::get<csr::CsrReferenceInput>(exported), baseline);
        auto plan = bufferInsertionPlan(database, baseline, TransactionGeneration{0}, editTestCreationToken());
        std::string source = "n0", driver = "in";
        for (std::uint64_t generation = 0; generation < 2; ++generation) {
          plan.expected_generation = TransactionGeneration{generation};
          auto& edit = plan.edits.front();
          edit.creation_token = editTestCreationToken(generation + 1);
          edit.rc_inputs = ParasiticRcInputs{ParasiticEditPolicy::kPreRouteTree};
          edit.placement_inputs = PlacedInputs{5.0 + generation, 7.0};
          const auto names = defaultTokenMaterialization(edit.creation_token, edit.library_cell);
          const auto* old_owner = &library.get_net_map().at(source);
          const auto old_rc = csr::detail::projectCanonicalParasiticNet(source, *old_owner);
          for (int attempt = 0; attempt < 3; ++attempt) {
            {
              std::map<std::string, ParasiticNet> trees;
              wire(trees, source, driver, names.input_pin_name, 75.0);
              wire(trees, names.output_net_name, names.output_pin_name, "out", 50.0);
              auto pending = NativeFullEditPreparer::prepare(database, authority,
                  {plan.expected_generation, plan.edits}, state, std::move(trees));
              require(!authority.readyForQuery() && state.generation().value() == generation,
                      "full RC edit published before commit");
              if (attempt != 0) NativeCandidateExecutor::executeFull(pending.transaction());
              if (attempt == 2) require(pending.commit(), "full RC commit failed");
            }
            require(authority.readyForQuery(), "full RC transaction retained pending state");
            if (attempt != 2)
              require(state.generation().value() == generation
                          && &library.get_net_map().at(source) == old_owner
                          && csr::detail::projectCanonicalParasiticNet(source, *old_owner) == old_rc
                          && !database.get_net_map().contains(names.output_net_name),
                      "full RC abandon or computed rollback did not restore old state");
          }
          require(state.generation().value() == generation + 1
                      && database.get_pin_map().at("out").get_net_name() == names.output_net_name,
                  "full RC commit lost generation or connectivity");
          source = names.output_net_name;
          driver = names.output_pin_name;
          edit.original_net = CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet};
        }
        return StaStatus::success(session.session().state());
      });
  if (!status.ok()) throw std::runtime_error(status.message);
  require(session.closeAndRelease().ok(), "full RC session cleanup failed");
}
