// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors
#include "symmetric_eigensolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace overhauled::numerics {
namespace {

bool fail(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
    return false;
}

} // namespace

bool symmetric_eigen_decomposition(
    const std::vector<double>& matrix,
    std::size_t dimension,
    SymmetricEigenDecomposition* result,
    std::string* error,
    double tolerance,
    std::size_t max_sweeps) {
    if (result == nullptr) return fail(error, "result pointer is null");
    if (dimension == 0 || dimension > kMaxEigenDimension) {
        return fail(error, "matrix dimension is outside 1..=64");
    }
    if (matrix.size() != dimension * dimension) {
        return fail(error, "matrix storage length does not equal dimension squared");
    }
    if (!std::isfinite(tolerance) || tolerance <= 0.0 || tolerance >= 1.0 ||
        max_sweeps == 0 || max_sweeps > 1024) {
        return fail(error, "invalid tolerance or Jacobi sweep limit");
    }
    for (double value : matrix) {
        if (!std::isfinite(value)) return fail(error, "matrix contains a non-finite value");
    }

    double scale = 0.0;
    for (double value : matrix) scale = std::max(scale, std::abs(value));
    const double divisor = scale == 0.0 ? 1.0 : scale;
    std::vector<double> a(matrix.size());
    for (std::size_t index = 0; index < matrix.size(); ++index) {
        a[index] = matrix[index] / divisor;
    }

    for (std::size_t row = 0; row < dimension; ++row) {
        for (std::size_t column = row + 1; column < dimension; ++column) {
            const double left = a[row * dimension + column];
            const double right = a[column * dimension + row];
            if (std::abs(left - right) >
                tolerance * (1.0 + std::max(std::abs(left), std::abs(right)))) {
                return fail(error, "matrix is not symmetric within tolerance");
            }
            const double average = 0.5 * (left + right);
            a[row * dimension + column] = average;
            a[column * dimension + row] = average;
        }
    }

    const std::vector<double> original = a;
    std::vector<double> vectors(dimension * dimension, 0.0);
    for (std::size_t index = 0; index < dimension; ++index) {
        vectors[index * dimension + index] = 1.0;
    }

    bool converged = dimension == 1;
    std::size_t sweeps_used = 0;
    for (std::size_t sweep = 0; sweep < max_sweeps; ++sweep) {
        double max_off_diagonal = 0.0;
        double max_diagonal = 0.0;
        for (std::size_t index = 0; index < dimension; ++index) {
            max_diagonal = std::max(max_diagonal, std::abs(a[index * dimension + index]));
        }

        for (std::size_t p = 0; p < dimension; ++p) {
            for (std::size_t q = p + 1; q < dimension; ++q) {
                const double apq = a[p * dimension + q];
                max_off_diagonal = std::max(max_off_diagonal, std::abs(apq));
                if (std::abs(apq) <= tolerance * std::max(max_diagonal, 1.0)) continue;

                const double app = a[p * dimension + p];
                const double aqq = a[q * dimension + q];
                const double tau = (aqq - app) / (2.0 * apq);
                const double sign = tau < 0.0 ? -1.0 : 1.0;
                const double t = sign / (std::abs(tau) + std::hypot(tau, 1.0));
                const double cosine = 1.0 / std::sqrt(1.0 + t * t);
                const double sine = t * cosine;

                for (std::size_t k = 0; k < dimension; ++k) {
                    if (k == p || k == q) continue;
                    const double akp = a[k * dimension + p];
                    const double akq = a[k * dimension + q];
                    const double new_kp = cosine * akp - sine * akq;
                    const double new_kq = sine * akp + cosine * akq;
                    a[k * dimension + p] = new_kp;
                    a[p * dimension + k] = new_kp;
                    a[k * dimension + q] = new_kq;
                    a[q * dimension + k] = new_kq;
                }

                a[p * dimension + p] = app - t * apq;
                a[q * dimension + q] = aqq + t * apq;
                a[p * dimension + q] = 0.0;
                a[q * dimension + p] = 0.0;

                for (std::size_t k = 0; k < dimension; ++k) {
                    const double vkp = vectors[k * dimension + p];
                    const double vkq = vectors[k * dimension + q];
                    vectors[k * dimension + p] = cosine * vkp - sine * vkq;
                    vectors[k * dimension + q] = sine * vkp + cosine * vkq;
                }
            }
        }

        sweeps_used = sweep + 1;
        double remaining = 0.0;
        double diagonal_scale = 1.0;
        for (std::size_t row = 0; row < dimension; ++row) {
            diagonal_scale = std::max(diagonal_scale, std::abs(a[row * dimension + row]));
            for (std::size_t column = row + 1; column < dimension; ++column) {
                remaining = std::max(remaining, std::abs(a[row * dimension + column]));
            }
        }
        if (remaining <= tolerance * diagonal_scale) {
            converged = true;
            break;
        }
    }
    if (!converged) return fail(error, "Jacobi iteration did not converge");

    std::vector<std::size_t> order(dimension);
    for (std::size_t index = 0; index < dimension; ++index) order[index] = index;
    std::sort(order.begin(), order.end(), [&a, dimension](std::size_t left, std::size_t right) {
        return a[left * dimension + left] < a[right * dimension + right];
    });

    SymmetricEigenDecomposition candidate;
    candidate.eigenvalues.resize(dimension);
    candidate.eigenvectors.assign(dimension * dimension, 0.0);
    candidate.sweeps = sweeps_used;
    for (std::size_t column = 0; column < dimension; ++column) {
        candidate.eigenvalues[column] = a[order[column] * dimension + order[column]] * divisor;
        if (!std::isfinite(candidate.eigenvalues[column])) {
            return fail(error, "eigenvalue rescaling overflowed");
        }
        for (std::size_t row = 0; row < dimension; ++row) {
            candidate.eigenvectors[row * dimension + column] =
                vectors[row * dimension + order[column]];
        }
    }

    double residual_squared = 0.0;
    double matrix_norm_squared = 0.0;
    double orthogonality_squared = 0.0;
    for (std::size_t row = 0; row < dimension; ++row) {
        for (std::size_t column = 0; column < dimension; ++column) {
            double av = 0.0;
            for (std::size_t k = 0; k < dimension; ++k) {
                av += original[row * dimension + k] *
                      candidate.eigenvectors[k * dimension + column];
            }
            const double lambda = candidate.eigenvalues[column] / divisor;
            const double vlambda = candidate.eigenvectors[row * dimension + column] * lambda;
            const double difference = av - vlambda;
            residual_squared += difference * difference;
            matrix_norm_squared += original[row * dimension + column] *
                                   original[row * dimension + column];

            double dot = 0.0;
            for (std::size_t k = 0; k < dimension; ++k) {
                dot += candidate.eigenvectors[k * dimension + row] *
                       candidate.eigenvectors[k * dimension + column];
            }
            const double expected = row == column ? 1.0 : 0.0;
            const double orthogonal_difference = dot - expected;
            orthogonality_squared += orthogonal_difference * orthogonal_difference;
        }
    }
    candidate.relative_residual =
        std::sqrt(residual_squared) / std::max(std::sqrt(matrix_norm_squared), 1.0);
    candidate.orthogonality_error = std::sqrt(orthogonality_squared);
    *result = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace overhauled::numerics
