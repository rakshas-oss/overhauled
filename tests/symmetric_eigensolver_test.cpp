// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors
#include "symmetric_eigensolver.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

int main() {
    using overhauled::numerics::SymmetricEigenDecomposition;
    using overhauled::numerics::symmetric_eigen_decomposition;

    // Calls with side effects are outside assert so they still execute under NDEBUG.
    {
        SymmetricEigenDecomposition result;
        std::string error;
        const bool ok = symmetric_eigen_decomposition(
            {4.0, 0.0, 0.0, 0.0, -2.0, 0.0, 0.0, 0.0, 1.0}, 3, &result, &error);
        assert(ok);
        assert(result.eigenvalues.size() == 3);
        assert(result.eigenvalues[0] == -2.0);
        assert(result.eigenvalues[1] == 1.0);
        assert(result.eigenvalues[2] == 4.0);
        assert(result.relative_residual < 1.0e-12);
        assert(result.orthogonality_error < 1.0e-12);
    }
    {
        SymmetricEigenDecomposition result;
        std::string error;
        const bool ok = symmetric_eigen_decomposition(
            {2.0, 1.0, 1.0, 2.0}, 2, &result, &error);
        assert(ok);
        assert(std::abs(result.eigenvalues[0] - 1.0) < 1.0e-12);
        assert(std::abs(result.eigenvalues[1] - 3.0) < 1.0e-12);
        assert(result.relative_residual < 1.0e-12);
        assert(result.orthogonality_error < 1.0e-12);
    }
    {
        SymmetricEigenDecomposition result;
        std::string error;
        const bool ok = symmetric_eigen_decomposition(
            {4.0, 1.0, 1.0, 1.0, 3.0, 0.0, 1.0, 0.0, 2.0}, 3, &result, &error);
        assert(ok);
        assert(result.eigenvalues[0] > 0.0);
        assert(result.relative_residual < 1.0e-11);
        assert(result.orthogonality_error < 1.0e-11);
    }
    {
        SymmetricEigenDecomposition result;
        std::string error;
        assert(!symmetric_eigen_decomposition({}, 0, &result, &error));
        assert(!error.empty());
        assert(!symmetric_eigen_decomposition({1.0, 2.0}, 2, &result, &error));
        assert(!symmetric_eigen_decomposition({1.0, 2.0, 0.0, 1.0}, 2, &result, &error));
        assert(!symmetric_eigen_decomposition(
            {1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::quiet_NaN(), 1.0}, 2, &result, &error));
        assert(!symmetric_eigen_decomposition(std::vector<double>(65 * 65, 0.0), 65, &result, &error));
    }
    {
        SymmetricEigenDecomposition result;
        std::string error;
        const bool zero_ok = symmetric_eigen_decomposition(std::vector<double>(9, 0.0), 3, &result, &error);
        assert(zero_ok);
        assert(result.eigenvalues == std::vector<double>({0.0, 0.0, 0.0}));
        const bool large_ok = symmetric_eigen_decomposition(
            {1.0e200, 2.0e199, 2.0e199, 3.0e200}, 2, &result, &error);
        assert(large_ok);
        assert(result.relative_residual < 1.0e-11);
    }

    std::cout << "symmetric_eigensolver_test passed\n";
    return 0;
}
