// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "CsrSnapshotStorage.hpp"

namespace ista::csr::snapshot_test {

[[nodiscard]] CsrSnapshotStorageReport independentReferenceInputStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata);

}  // namespace ista::csr::snapshot_test
