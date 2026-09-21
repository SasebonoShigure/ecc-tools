// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <memory>
#include <thread>

#include "TimingEditResolver.hpp"

namespace ista {
class Database;
namespace csr {
struct CsrReferenceInput;
struct CsrTimingResult;
struct StaAnalysisContext;
}
namespace timing_edit {
class NativeExecutionAuthority;
}
namespace buffer_trace {

class BufferFixedTraceGenerator;
class BufferTraceProductionFreshAdapter;
class NativeBufferObservationExtractor;

// Stack-scoped diagnostic replay borrows. NativeEditTransaction owns each
// actual edit and rolls it back if the callback does not commit it.
class BufferReplayContext final
{
 public:
  BufferReplayContext(const BufferReplayContext&) = delete;
  BufferReplayContext& operator=(const BufferReplayContext&) = delete;
  BufferReplayContext(BufferReplayContext&&) = delete;
  BufferReplayContext& operator=(BufferReplayContext&&) = delete;

 private:
  BufferReplayContext(Database& database, const csr::StaAnalysisContext& analysis,
                      const timing_edit::NativeExecutionAuthority& authority,
                      timing_edit::BaselineSemanticIdentity baseline,
                      timing_edit::CommittedEditIdentity identity,
                      timing_edit::TimingEditResolverState resolver,
                      void (*compare_timing)(const csr::CsrReferenceInput&, const csr::CsrTimingResult&, const Database&));
  ~BufferReplayContext() noexcept;

  void requireActive() const;
  [[nodiscard]] Database& database() const;
  [[nodiscard]] const csr::StaAnalysisContext& analysisContext() const;
  [[nodiscard]] const timing_edit::NativeExecutionAuthority& authority() const;

  Database& _database;
  const csr::StaAnalysisContext& _analysis;
  const timing_edit::NativeExecutionAuthority& _authority;
  std::shared_ptr<const timing_edit::BaselineSemanticIdentity> _baseline;
  timing_edit::CommittedEditIdentity _identity;
  timing_edit::TimingEditResolverState _resolver;
  void (*_compare_timing)(const csr::CsrReferenceInput&, const csr::CsrTimingResult&, const Database&);
  std::thread::id _owner_thread;
  bool _active{true};

  friend class BufferFixedTraceGenerator;
  friend class BufferTraceProductionFreshAdapter;
  friend class BufferTraceReplayer;
  friend class NativeBufferObservationExtractor;
};
}  // namespace buffer_trace
}  // namespace ista
