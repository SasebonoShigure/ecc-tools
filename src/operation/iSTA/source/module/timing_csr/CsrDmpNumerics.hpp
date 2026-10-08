// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include <cmath>
#include <cstdint>

#if defined(__CUDACC__)
#define ISTA_DMP_HD __host__ __device__
#else
#define ISTA_DMP_HD
#endif

namespace ista::csr::dmp_detail {
// Native scaled-pivot LU and back-substitution for the two/three DMP equations.
// Scratch is private to this call; matrix and delta are updated in place.
ISTA_DMP_HD inline bool solveJacobian(int order, double matrix[3][3], double delta[3])
{
  double scale[3]{};
  int indices[3]{};

  for (int32_t row = 0; row < order; row++) {
    double largest_value = 0.0;
    for (int32_t column = 0; column < order; column++) {
      largest_value = (largest_value < std::abs(matrix[row][column]) ? std::abs(matrix[row][column]) : largest_value);
    }
    if (largest_value == 0.0 || !std::isfinite(largest_value)) {
      return false;
    }
    scale[row] = 1.0 / largest_value;
  }

  int32_t last_index = order - 1;
  for (int32_t column = 0; column < order; column++) {
    for (int32_t row = 0; row < column; row++) {
      double value = matrix[row][column];
      for (int32_t index = 0; index < row; index++) {
        value -= matrix[row][index] * matrix[index][column];
      }
      matrix[row][column] = value;
    }

    double largest_value = 0.0;
    int32_t pivot_row = column;
    for (int32_t row = column; row < order; row++) {
      double value = matrix[row][column];
      for (int32_t index = 0; index < column; index++) {
        value -= matrix[row][index] * matrix[index][column];
      }
      matrix[row][column] = value;
      double scaled_value = scale[row] * std::abs(value);
      if (scaled_value >= largest_value) {
        largest_value = scaled_value;
        pivot_row = row;
      }
    }
    if (column != pivot_row) {
      for (int i = 0; i < 3; ++i) {
      const double old = matrix[pivot_row][i];
      matrix[pivot_row][i] = matrix[column][i];
      matrix[column][i] = old;
    }
      scale[pivot_row] = scale[column];
    }
    indices[column] = pivot_row;
    if (matrix[column][column] == 0.0) {
      matrix[column][column] = 1E-20;
    }
    if (column != last_index) {
      double inverse_pivot = 1.0 / matrix[column][column];
      for (int32_t row = column + 1; row < order; row++) {
        matrix[row][column] *= inverse_pivot;
      }
    }
  }

  int32_t first_nonzero = -1;
  for (int32_t row = 0; row < order; row++) {
    int32_t pivot_row = indices[row];
    double value = delta[pivot_row];
    delta[pivot_row] = delta[row];
    if (first_nonzero != -1) {
      for (int32_t column = first_nonzero; column < row; column++) {
        value -= matrix[row][column] * delta[column];
      }
    } else if (value != 0.0) {
      first_nonzero = row;
    }
    delta[row] = value;
  }

  for (int32_t row = order - 1; row >= 0; row--) {
    double value = delta[row];
    for (int32_t column = row + 1; column < order; column++) {
      value -= matrix[row][column] * delta[column];
    }
    delta[row] = value / matrix[row][row];
  }

  return true;
}
}  // namespace ista::csr::dmp_detail
#undef ISTA_DMP_HD
