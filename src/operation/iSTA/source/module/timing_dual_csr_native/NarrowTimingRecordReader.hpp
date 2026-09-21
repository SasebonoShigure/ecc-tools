#pragma once
#include <memory>
#include <optional>
#include "AffectedCapturePlan.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "DualSemanticReadView.hpp"
#include "TimingEditResolver.hpp"
namespace ista { class Database; }
namespace ista::csr::detail { class NativeLibraryModels; }
namespace ista::timing_dual_csr {
namespace detail { struct CandidateCaptureStorage; }
class TimingEditDualCsrDescriptorBuilder;
class CandidateBoundCapture final {
 public:
  CandidateBoundCapture(const CandidateBoundCapture&) = delete;
  CandidateBoundCapture& operator=(const CandidateBoundCapture&) = delete;
  CandidateBoundCapture(CandidateBoundCapture&&) noexcept;
  CandidateBoundCapture& operator=(CandidateBoundCapture&&) noexcept;
  ~CandidateBoundCapture();

  [[nodiscard]] bool success() const noexcept;
  [[nodiscard]] std::optional<NarrowCaptureError> error() const noexcept;
  [[nodiscard]] const NarrowCaptureAudit* audit() const noexcept;

 private:
  explicit CandidateBoundCapture(
      std::unique_ptr<detail::CandidateCaptureStorage>) noexcept;
  std::unique_ptr<detail::CandidateCaptureStorage> _storage;

  friend class NarrowTimingRecordReader;
  friend class TimingEditDualCsrDescriptorBuilder;
};

class NarrowTimingRecordReader final {
 public:
  explicit NarrowTimingRecordReader(csr::detail::NativeLibraryModels& models) : _models(models) {}
  [[nodiscard]] CandidateBoundCapture read(
      const ista::Database&, const DualSemanticReadView&,
      const DualCsrArcGroupIndex&,
      timing_edit::TimingEditResolverView,
      const timing_edit::PreparedBatch&,
      const AffectedCapturePlan&) const;
 private:
  csr::detail::NativeLibraryModels& _models;
};
}
