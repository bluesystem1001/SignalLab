#include "signallab/app/display_downsampler.hpp"

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
    if (pixelWidth == 0) {
        return {};
    }

    const auto samples = signal.samples();
    const std::size_t fullResolutionLimit =
        samples.size() / 2 + samples.size() % 2;

    if (pixelWidth >= fullResolutionLimit) {
        std::vector<DisplayPoint> points;
        points.reserve(samples.size());

        for (std::size_t index = 0; index < samples.size(); ++index) {
            points.push_back(makePoint(signal, index));
        }

        return points;
    }

    std::vector<DisplayPoint> points;
    points.reserve(pixelWidth * 2);

    for (std::size_t bucket = 0; bucket < pixelWidth; ++bucket) {
        const std::size_t firstIndex = bucket * samples.size() / pixelWidth;
        const std::size_t pastLastIndex =
            (bucket + 1) * samples.size() / pixelWidth;

        std::size_t minimumIndex = firstIndex;
        std::size_t maximumIndex = firstIndex;

        for (std::size_t index = firstIndex + 1;
             index < pastLastIndex;
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
    if (pixelWidth == 0
        || spectrum.frequenciesHz.empty()
        || spectrum.frequenciesHz.size() != spectrum.amplitudes.size()) {
        return {};
    }

    const std::size_t pointCount = spectrum.amplitudes.size();
    const std::size_t fullResolutionLimit =
        pointCount / 2 + pointCount % 2;

    if (pixelWidth >= fullResolutionLimit) {
        std::vector<SpectrumDisplayPoint> points;
        points.reserve(pointCount);
        for (std::size_t index = 0; index < pointCount; ++index) {
            points.push_back(makeSpectrumPoint(spectrum, index));
        }
        return points;
    }

    std::vector<SpectrumDisplayPoint> points;
    points.reserve(pixelWidth * 2);

    for (std::size_t bucket = 0; bucket < pixelWidth; ++bucket) {
        const std::size_t firstIndex = bucket * pointCount / pixelWidth;
        const std::size_t pastLastIndex =
            (bucket + 1) * pointCount / pixelWidth;

        std::size_t minimumIndex = firstIndex;
        std::size_t maximumIndex = firstIndex;
        for (std::size_t index = firstIndex + 1;
             index < pastLastIndex;
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
