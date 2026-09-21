#include "notch_filter.h"

#include <algorithm>
#include <cmath>

namespace audiomix {

NotchFilter::NotchFilter() = default;

void NotchFilter::configure(double sampleRateHz, double q) {
    sampleRate_ = sampleRateHz;
    targetQ_.store(q, std::memory_order_relaxed);
}

void NotchFilter::setFrequency(double freqHz) {
    targetFreq_.store(freqHz, std::memory_order_relaxed);
}

void NotchFilter::setQ(double q) {
    targetQ_.store(q, std::memory_order_relaxed);
}

void NotchFilter::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
}

void NotchFilter::resetState() {
    x1_ = x2_ = y1_ = y2_ = 0.0;
}

void NotchFilter::recompute(double freq, double q) {
    // Clamp to sane, stable range: above DC, below Nyquist with margin.
    freq = std::clamp(freq, 20.0, sampleRate_ * 0.49);
    q = std::max(q, 0.1);

    const double w0 = 2.0 * M_PI * freq / sampleRate_;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);

    // RBJ Audio EQ Cookbook, notch (band-reject) filter.
    const double a0 = 1.0 + alpha;
    b0_ = 1.0 / a0;
    b1_ = (-2.0 * cosw0) / a0;
    b2_ = 1.0 / a0;
    a1_ = (-2.0 * cosw0) / a0;
    a2_ = (1.0 - alpha) / a0;

    lastFreq_ = freq;
    lastQ_ = q;
}

void NotchFilter::updateCoefficients() {
    const double f = targetFreq_.load(std::memory_order_relaxed);
    const double q = targetQ_.load(std::memory_order_relaxed);
    if (f != lastFreq_ || q != lastQ_) {
        recompute(f, q);
    }
}

} // namespace audiomix
