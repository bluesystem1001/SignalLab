#pragma once

#include <cstddef>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::algorithm {

class StreamingFirFilter {
public:
    [[nodiscard]] static core::Result<StreamingFirFilter> create(
        std::vector<double> coefficients);

    void reset() noexcept;
    [[nodiscard]] double processSample(double sample) noexcept;

private:
    explicit StreamingFirFilter(std::vector<double> coefficients);

    std::vector<double> coefficients_;
    std::vector<double> history_;
    std::size_t nextWriteIndex_{0};
};

}
