#pragma once
//
// notch_filter.h
//
// Single second-order (biquad) IIR notch filter, RBJ "Audio EQ Cookbook"
// design, used to surgically suppress a narrow band around a detected
// feedback frequency without materially coloring the rest of the signal
// (Q ~ 10 by default -> roughly 1/10th-octave-wide notch).
//
// Real-time-safety contract:
//   - setFrequency()/setQ()/setEnabled() may be called from ANY thread
//     (e.g. the CLI thread). They only do atomic stores.
//   - updateCoefficients() and processSample()/processBlock() MUST be
//     called from the audio thread only. updateCoefficients() recomputes
//     the biquad coefficients (a few trig calls) but only when the target
//     frequency/Q actually changed since the last call, so steady-state
//     cost is a couple of atomic loads and comparisons.
//
#include <atomic>
#include <cstddef>

namespace audiomix {

class NotchFilter {
public:
    NotchFilter();

    // Non-real-time: call once at startup (or when sample rate changes).
    void configure(double sampleRateHz, double q = 10.0);

    // Thread-safe controls -----------------------------------------------
    void setFrequency(double freqHz);
    void setQ(double q);
    void setEnabled(bool enabled);

    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    double frequency() const { return targetFreq_.load(std::memory_order_relaxed); }
    double q() const { return targetQ_.load(std::memory_order_relaxed); }

    // Audio-thread only ----------------------------------------------------
    // Recomputes biquad coefficients if the target frequency/Q changed.
    // Call once per block before processing.
    void updateCoefficients();

    // Resets filter state (delay elements). Call when re-enabling a notch
    // at a new frequency to avoid a transient click from stale history.
    void resetState();

    inline float processSample(float x) {
        if (!enabled_.load(std::memory_order_relaxed)) {
            return x;
        }
        const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return static_cast<float>(y);
    }

    inline void processBlock(float* buf, size_t n) {
        if (!enabled_.load(std::memory_order_relaxed)) {
            return;
        }
        for (size_t i = 0; i < n; ++i) {
            buf[i] = processSample(buf[i]);
        }
    }

private:
    void recompute(double freq, double q);

    double sampleRate_ = 48000.0;

    std::atomic<double> targetFreq_{1000.0};
    std::atomic<double> targetQ_{10.0};
    std::atomic<bool> enabled_{false};

    double lastFreq_ = -1.0;
    double lastQ_ = -1.0;

    // Normalized biquad coefficients (a0 == 1 after normalization).
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0;
    double a1_ = 0.0, a2_ = 0.0;

    // Direct Form I delay elements.
    double x1_ = 0.0, x2_ = 0.0;
    double y1_ = 0.0, y2_ = 0.0;
};

} // namespace audiomix
