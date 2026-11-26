#include "multivariate_stats.h"

#include <cassert>
#include <cmath>
#include <limits>

using nvlink::stream_stats::MultivariateAccumulator;
using nvlink::stream_stats::kMaxDimensions;

int main() {
    MultivariateAccumulator left(3);
    std::array<double, kMaxDimensions> row{};
    row[0] = 1.0; row[1] = 2.0; row[2] = 100.0;
    assert(left.observe(row, 0b111));
    row[0] = 2.0; row[1] = 4.0;
    assert(left.observe(row, 0b011)); // dimension 2 is missing, not zero
    row[0] = 3.0; row[1] = 6.0;
    assert(left.observe(row, 0b011));

    auto summary = left.snapshot();
    const auto xy = 1;
    assert(summary.pair_counts[xy] == 3);
    assert(std::fabs(summary.covariance[xy] - (4.0 / 3.0)) < 1e-9);
    assert(std::fabs(summary.correlation[xy] - 1.0) < 1e-9);
    assert(summary.pair_counts[2] == 1);

    MultivariateAccumulator right(3);
    row[0] = 4.0; row[1] = 8.0;
    assert(right.observe(row, 0b011));
    row[0] = 4.0; row[2] = 400.0;
    assert(right.observe(row, 0b101));
    row[0] = 5.0; row[2] = 500.0;
    assert(right.observe(row, 0b101));
    assert(left.merge(right));
    summary = left.snapshot();
    assert(summary.pair_counts[xy] == 4);
    assert(std::fabs(summary.correlation[xy] - 1.0) < 1e-9);
    const auto xz = 2;
    assert(summary.pair_counts[xz] == 3);
    assert(std::fabs(summary.correlation[xz] - 1.0) < 1e-9);
    assert(!left.merge(left));

    row[0] = std::numeric_limits<double>::infinity();
    assert(!left.observe(row, 0b001));
    assert(left.snapshot().pair_counts[0] == 4);

    MultivariateAccumulator too_many(33);
    assert(!too_many.observe(row, 1));

    MultivariateAccumulator overflow(1);
    row[0] = 1.0e308;
    assert(overflow.observe(row, 1));
    row[0] = -1.0e308;
    assert(!overflow.observe(row, 1));
    assert(overflow.snapshot().pair_counts[0] == 1);
    left.reset();
    assert(left.snapshot().pair_counts[xy] == 0);
    return 0;
}
