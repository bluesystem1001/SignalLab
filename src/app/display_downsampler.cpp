#include "signallab/app/display_downsampler.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace signallab::app {
namespace {

DisplayPoint makePoint(const core::Signal& signal, std::size_t index)
{
    return DisplayPoint{
        signal.sampleTimeSeconds(index),
        signal.samples()[index]
    };
}

SpectrumDisplayPoint makeSpectrumPoint(
    const algorithm::Spectrum& spectrum,
    std::size_t index)
{
    return SpectrumDisplayPoint{
        spectrum.frequenciesHz[index],
        spectrum.amplitudes[index]
    };
}

}  // namespace

std::vector<DisplayPoint> makeDisplayPoints(
    const core::Signal& signal,
    std::size_t pixelWidth)
{
    return makeDisplayPoints(signal, pixelWidth, 0, signal.sampleCount());
}

std::vector<DisplayPoint> makeDisplayPoints(
    const core::Signal& signal,
    std::size_t pixelWidth,
    std::size_t firstIndex,
    std::size_t pastLastIndex)
{
    const auto samples = signal.samples();
    firstIndex = std::min(firstIndex, samples.size());
    pastLastIndex = std::min(pastLastIndex, samples.size());

    if (pixelWidth == 0 || firstIndex >= pastLastIndex) {
        return {};
    }

    const std::size_t pointCount = pastLastIndex - firstIndex;
    const std::size_t fullResolutionLimit =
        pointCount / 2 + pointCount % 2;

    if (pixelWidth >= fullResolutionLimit) {
        std::vector<DisplayPoint> points;
        points.reserve(pointCount);

        for (std::size_t index = firstIndex;
             index < pastLastIndex;
             ++index) {
            points.push_back(makePoint(signal, index));
        }

        return points;
    }

    std::vector<DisplayPoint> points;
    points.reserve(pixelWidth * 2);

    for (std::size_t bucket = 0; bucket < pixelWidth; ++bucket) {
        const std::size_t bucketFirstIndex =
            firstIndex + bucket * pointCount / pixelWidth;
        const std::size_t bucketPastLastIndex =
            firstIndex + (bucket + 1) * pointCount / pixelWidth;

        std::size_t minimumIndex = bucketFirstIndex;
        std::size_t maximumIndex = bucketFirstIndex;

        for (std::size_t index = bucketFirstIndex + 1;
             index < bucketPastLastIndex;
             ++index) {
            if (samples[index] < samples[minimumIndex]) {
                minimumIndex = index;
            }
            if (samples[index] > samples[maximumIndex]) {
                maximumIndex = index;
            }
        }

        if (minimumIndex == maximumIndex) {
            points.push_back(makePoint(signal, minimumIndex));
        } else if (minimumIndex < maximumIndex) {
            points.push_back(makePoint(signal, minimumIndex));
            points.push_back(makePoint(signal, maximumIndex));
        } else {
            points.push_back(makePoint(signal, maximumIndex));
            points.push_back(makePoint(signal, minimumIndex));
        }
    }

    return points;
}

std::vector<SpectrumDisplayPoint> makeSpectrumDisplayPoints(
    const algorithm::Spectrum& spectrum,
    std::size_t pixelWidth)
{
    return makeSpectrumDisplayPoints(
        spectrum,
        pixelWidth,
        0,
        spectrum.frequenciesHz.size());
}

std::vector<SpectrumDisplayPoint> makeSpectrumDisplayPoints(
    const algorithm::Spectrum& spectrum,
    std::size_t pixelWidth,
    std::size_t firstIndex,
    std::size_t pastLastIndex)
{
    if (spectrum.frequenciesHz.size() != spectrum.amplitudes.size()) {
        return {};
    }

    const std::size_t pointCount = spectrum.amplitudes.size();
    firstIndex = std::min(firstIndex, pointCount);
    pastLastIndex = std::min(pastLastIndex, pointCount);

    if (pixelWidth == 0 || firstIndex >= pastLastIndex) {
        return {};
    }

    const std::size_t rangePointCount = pastLastIndex - firstIndex;
    const std::size_t fullResolutionLimit =
        rangePointCount / 2 + rangePointCount % 2;

    if (pixelWidth >= fullResolutionLimit) {
        std::vector<SpectrumDisplayPoint> points;
        points.reserve(rangePointCount);
        for (std::size_t index = firstIndex;
             index < pastLastIndex;
             ++index) {
            points.push_back(makeSpectrumPoint(spectrum, index));
        }
        return points;
    }

    std::vector<SpectrumDisplayPoint> points;
    points.reserve(pixelWidth * 2);

    for (std::size_t bucket = 0; bucket < pixelWidth; ++bucket) {
        const std::size_t bucketFirstIndex =
            firstIndex + bucket * rangePointCount / pixelWidth;
        const std::size_t bucketPastLastIndex =
            firstIndex + (bucket + 1) * rangePointCount / pixelWidth;

        std::size_t minimumIndex = bucketFirstIndex;
        std::size_t maximumIndex = bucketFirstIndex;
        for (std::size_t index = bucketFirstIndex + 1;
             index < bucketPastLastIndex;
             ++index) {
            if (spectrum.amplitudes[index]
                < spectrum.amplitudes[minimumIndex]) {
                minimumIndex = index;
            }
            if (spectrum.amplitudes[index]
                > spectrum.amplitudes[maximumIndex]) {
                maximumIndex = index;
            }
        }

        if (minimumIndex == maximumIndex) {
            points.push_back(makeSpectrumPoint(spectrum, minimumIndex));
        } else if (minimumIndex < maximumIndex) {
            points.push_back(makeSpectrumPoint(spectrum, minimumIndex));
            points.push_back(makeSpectrumPoint(spectrum, maximumIndex));
        } else {
            points.push_back(makeSpectrumPoint(spectrum, maximumIndex));
            points.push_back(makeSpectrumPoint(spectrum, minimumIndex));
        }
    }

    return points;
}

}  // namespace signallab::app
