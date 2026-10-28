#include "signallab/algorithm/streaming_fir_filter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace signallab::algorithm {

core::Result<StreamingFirFilter> StreamingFirFilter::create(
    std::vector<double> coefficients)
{
    if (coefficients.empty()) {
        return core::Result<StreamingFirFilter>::failure(
            core::ErrorCode::InvalidParameter,
            "An FIR filter must contain at least one coefficient.");
    }
    for (const double coefficient : coefficients) {
        if (!std::isfinite(coefficient)) {
            return core::Result<StreamingFirFilter>::failure(
                core::ErrorCode::InvalidParameter,
                "Every FIR coefficient must be finite.");
        }
    }

    return core::Result<StreamingFirFilter>::success(
        StreamingFirFilter(std::move(coefficients)));
}

StreamingFirFilter::StreamingFirFilter(std::vector<double> coefficients)
    : coefficients_(std::move(coefficients)),
      history_(coefficients_.size(), 0.0)
{
}

void StreamingFirFilter::reset() noexcept
{
    std::fill(history_.begin(), history_.end(), 0.0);
    nextWriteIndex_ = 0;
}

double StreamingFirFilter::processSample(double sample) noexcept
{
    history_[nextWriteIndex_] = sample;
    std::size_t historyIndex = nextWriteIndex_;
    long double output = 0.0L;
    for (const double coefficient : coefficients_) {
        output += static_cast<long double>(coefficient)
            * static_cast<long double>(history_[historyIndex]);
        historyIndex = historyIndex == 0U
            ? history_.size() - 1U
            : historyIndex - 1U;
    }
    ++nextWriteIndex_;
    if (nextWriteIndex_ == history_.size()) {
        nextWriteIndex_ = 0;
    }
    return static_cast<double>(output);
}

}
