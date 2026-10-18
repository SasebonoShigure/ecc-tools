#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"

namespace ista::csr::cuda_detail {
namespace {
__global__ void patchPathRows(PathRow* rows, const PathRowUpdate* updates, Index count, bool restore)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto& update = updates[i];
    rows[update.row] = restore ? update.before : update.after;
  }
}
void applyRows(PathDomainBuffers& domain, bool restore, Runtime& memory, unsigned threads, CsrCudaTelemetry& telemetry)
{
  if (domain.undo.rows.empty()) return;
  const auto count = domain.undo.rows.size();
  const auto blocks = static_cast<unsigned>(std::min<Index>((count - 1) / threads + 1, 65535));
  patchPathRows<<<blocks, threads, 0, memory.stream()>>>(domain.rows.data, domain.updates.data, count, restore);
  check(cudaGetLastError(), "CUDA path row patch launch");
  ++telemetry.kernel_launches;
}
}  // namespace
}  // namespace ista::csr::cuda_detail

namespace ista::csr {
using namespace cuda_detail;
void CudaDualCsrSession::Impl::allocatePathDomain()
{
  auto& domain = storage->domain;
  auto grow = [&]<class T>(DeviceBuffer<T>& buffer, Index count, Index preserve = 0) {
    if (memory.ensure(buffer, count, preserve)) ++telemetry.grown_buffers;
  };
  const auto old_rows = origin ? checkedProduct(origin->view.node_count, 8) : 0;
  grow(domain.rows, domain.host.rows.size(), old_rows);
  grow(domain.updates, domain.undo.rows.size());
  if (domain.undo.applied && domain.undo.compacted) {
    grow(domain.spare_tags, domain.host.tags.size());
    std::swap(domain.tags, domain.spare_tags);
    domain.tags_swapped = true;
  } else {
    grow(domain.tags, domain.host.tags.size(), origin ? origin->view.path_count : 0);
  }
  if (origin)
    memory.zero(domain.rows.data + old_rows, domain.host.rows.size() - old_rows);
}

void CudaDualCsrSession::Impl::transferPathDomain()
{
  auto& domain = storage->domain;
  if (!domain.undo.applied) return;
  const auto begin = domain.undo.compacted ? 0 : domain.undo.tag_count;
  if (domain.host.tags.size() > begin)
    memory.upload(domain.host.tags.data() + begin, domain.tags.data + begin, domain.host.tags.size() - begin);
  memory.upload(domain.undo.rows, domain.updates.data);
  domain.uploaded = true;
  applyRows(domain, false, memory, storage->kernel_threads, telemetry.cuda);
}

void CudaDualCsrSession::Impl::restorePathRows(CsrCudaTelemetry& cleanup)
{
  auto& domain = storage->domain;
  if (domain.uploaded)
    applyRows(domain, true, memory, storage->kernel_threads, cleanup);
}
}  // namespace ista::csr
