#include "circuit/Measure.h"

#include <algorithm>
#include <cmath>

namespace circuitlab {
namespace {

// First index whose time is >= target. Samples are expected to be ordered.
size_t findTimeIndex(const std::vector<double>& time, size_t begin, size_t end,
                     double target)
{
    size_t index = begin;
    while (index < end && time[index] < target) {
        ++index;
    }
    return index;
}

}  // namespace

std::vector<double> subtractSamples(const std::vector<double>& a,
                                   const std::vector<double>& b)
{
    const size_t count = std::min(a.size(), b.size());
    std::vector<double> result(count, 0.0);
    for (size_t i = 0; i < count; ++i) {
        result[i] = a[i] - b[i];
    }
    return result;
}

WaveStats measureWave(const std::vector<double>& time, const std::vector<double>& values,
                      double settleFraction, double rippleFraction)
{
    WaveStats stats;
    const size_t count = std::min(time.size(), values.size());
    if (count < 8) {
        return stats;
    }

    settleFraction = std::clamp(settleFraction, 0.0, 0.95);
    rippleFraction = std::clamp(rippleFraction, 0.01, 1.0);

    const size_t last = count - 1;
    const double t0 = time[0];
    const double t1 = time[last];
    if (!(t1 > t0)) {
        return stats;
    }

    // Statistics window: skip the startup transient.
    const double windowStart = t0 + (t1 - t0) * settleFraction;
    size_t i0 = findTimeIndex(time, 0, last, windowStart);
    if (i0 >= last) {
        i0 = last > 0 ? last - 1 : 0;
    }
    const double ws = time[i0];
    const double we = t1;
    const double duration = we - ws;
    if (!(duration > 0.0)) {
        return stats;
    }

    // Trapezoidal integration keeps the average meaningful when ngspice emits
    // unevenly spaced points.
    double area = 0.0;
    double squareArea = 0.0;
    double minimum = values[i0];
    double maximum = values[i0];
    bool finite = std::isfinite(values[i0]);
    for (size_t i = i0 + 1; i <= last; ++i) {
        const double dt = time[i] - time[i - 1];
        const double v0 = values[i - 1];
        const double v1 = values[i];
        if (!std::isfinite(v0) || !std::isfinite(v1) || dt < 0.0) {
            finite = false;
            break;
        }
        area += (v0 + v1) * 0.5 * dt;
        squareArea += (v0 * v0 + v1 * v1) * 0.5 * dt;
        minimum = std::min(minimum, v1);
        maximum = std::max(maximum, v1);
    }
    if (!finite || !std::isfinite(area)) {
        return stats;
    }

    const double mean = area / duration;
    const double meanSquare = std::max(0.0, squareArea / duration);

    // Ripple window: the tail of the statistics window.
    const double rippleStart = we - duration * rippleFraction;
    size_t r0 = findTimeIndex(time, i0, last, rippleStart);
    if (r0 >= last) {
        r0 = last > 0 ? last - 1 : 0;
    }
    double rippleMin = values[r0];
    double rippleMax = values[r0];
    int crossings = 0;
    bool havePrevious = false;
    double previous = 0.0;
    for (size_t i = r0; i <= last; ++i) {
        const double v = values[i];
        if (!std::isfinite(v)) {
            havePrevious = false;
            continue;
        }
        rippleMin = std::min(rippleMin, v);
        rippleMax = std::max(rippleMax, v);
        const double shifted = v - mean;
        if (havePrevious) {
            const double previousShifted = previous - mean;
            // Count crossings of the average level: two crossings per period.
            if ((previousShifted < 0.0 && shifted >= 0.0) ||
                (previousShifted > 0.0 && shifted <= 0.0)) {
                ++crossings;
            }
        }
        previous = v;
        havePrevious = true;
    }
    const double rippleDuration = std::max(0.0, we - time[r0]);

    stats.ok = true;
    stats.samples = static_cast<int>(last - i0 + 1);
    stats.timeStart = ws;
    stats.timeEnd = we;
    stats.mean = mean;
    stats.min = minimum;
    stats.max = maximum;
    stats.rms = std::sqrt(meanSquare);
    stats.acRms = std::sqrt(std::max(0.0, meanSquare - mean * mean));
    stats.ripple = std::max(0.0, rippleMax - rippleMin);
    stats.rippleTimeStart = time[r0];
    stats.rippleTimeEnd = we;
    if (crossings >= 2 && rippleDuration > 0.0) {
        stats.frequency = static_cast<double>(crossings) / (2.0 * rippleDuration);
    }
    return stats;
}

}  // namespace circuitlab
