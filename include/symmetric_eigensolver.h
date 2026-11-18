// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace overhauled::numerics {

constexpr std::size_t kMaxEigenDimension = 64;
constexpr double kDefaultEigenTolerance = 1.0e-12;
constexpr std::size_t kDefaultEigenMaxSweeps = 128;

struct SymmetricEigenDecomposition {
    // Eigenvalues are returned in ascending order.
    std::vector<double> eigenvalues;
    // Row-major matrix whose columns are the corresponding eigenvectors.
    std::vector<double> eigenvectors;
    double relative_residual = 0.0;
    double orthogonality_error = 0.0;
    std::size_t sweeps = 0;
};

// Returns false on invalid shape, non-finite/asymmetric input, invalid options,
// non-convergence, or eigenvalue rescaling overflow. The input is not mutated.
bool symmetric_eigen_decomposition(
    const std::vector<double>& matrix,
    std::size_t dimension,
    SymmetricEigenDecomposition* result,
    std::string* error,
    double tolerance = kDefaultEigenTolerance,
    std::size_t max_sweeps = kDefaultEigenMaxSweeps);

} // namespace overhauled::numerics
