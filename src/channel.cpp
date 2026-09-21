#include "channel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audiomix {

Channel::Channel() = default;

void Channel::configure(double sampleRateHz, size_t blockSize, double notchQ) {
    sampleRate_ = sampleRateHz;
    blockSize_ = blockSize;

    // 1024-sample analysis window at 48kHz -> ~46.9Hz bin resolution,
    // updated every 256-sample hop (75% overlap). Gives the detector
    // enough frequency resolution to isolate a ringing tone without
    // adding any extra I/O latency (the analysis window is purely
    // internal bookkeeping; the audio path itself still only ever
    // buffers one 256-sample block at a time).
    detector_.configure(sampleRate_, /*fftSize=*/1024, /*hopSize=*/blockSize_);

    for (auto& notch : notches_) {
        notch.configure(sampleRate_, notchQ);
        notch.setEnabled(false);
    }
    notchActive_.fill(false);
    releaseCountdown_.fill(0);
    notchAge_.fill(0);
    blockCounter_ = 0;
}

void Channel::setGain(float linearGain) {
    gain_.store(std::max(0.0f, linearGain), std::memory_order_relaxed);
}

void Channel::setMuted(bool muted) {
    muted_.store(muted, std::memory_order_relaxed);
}

void Channel::setFeedbackSuppressionEnabled(bool enabled) {
    suppressionEnabled_.store(enabled, std::memory_order_relaxed);
}

void Channel::setSensitivity(float sensitivity01) {
    detector_.setSensitivity(sensitivity01);
}

int Channel::activeNotchCount() const {
    int n = 0;
    for (bool a : notchActive_) {
        if (a) ++n;
    }
    return n;
}

int Channel::findSlotFor(double freqHz) const {
    // Find an already-active slot whose frequency is close to freqHz
    // (within ~1 analysis bin worth of Hz), so we retune rather than
    // spawn a duplicate notch for essentially the same ring.
    const double toleranceHz = (sampleRate_ / 1024.0) * 1.5;
    for (int i = 0; i < kMaxNotchesPerChannel; ++i) {
        if (notchActive_[i] && std::abs(notches_[i].frequency() - freqHz) < toleranceHz) {
            return i;
        }
    }
    return -1;
}

int Channel::findFreeSlot() const {
    for (int i = 0; i < kMaxNotchesPerChannel; ++i) {
        if (!notchActive_[i]) return i;
    }
    return -1;
}

int Channel::findOldestSlot() const {
    int oldest = 0;
    unsigned long oldestAge = std::numeric_limits<unsigned long>::max();
    for (int i = 0; i < kMaxNotchesPerChannel; ++i) {
        if (notchAge_[i] < oldestAge) {
            oldestAge = notchAge_[i];
            oldest = i;
        }
    }
    return oldest;
}

void Channel::process(float* buf, size_t n) {
    ++blockCounter_;

    const bool suppression = suppressionEnabled_.load(std::memory_order_relaxed);

    if (suppression) {
        // Run detection on the pre-notch signal every block so we keep
        // seeing the "raw" ringing energy even while a notch is actively
        // suppressing it downstream (needed both to catch new/additional
        // ring frequencies and to decide when to release existing notches).
        auto events = detector_.process(buf, n);

        for (const auto& ev : events) {
            int slot = findSlotFor(ev.frequencyHz);
            if (slot < 0) {
                slot = findFreeSlot();
            }
            if (slot < 0) {
                slot = findOldestSlot(); // steal the longest-held notch
            }

            notches_[slot].setFrequency(ev.frequencyHz);
            notches_[slot].resetState();
            notches_[slot].setEnabled(true);
            notchActive_[slot] = true;
            notchAge_[slot] = blockCounter_;
            releaseCountdown_[slot] = kReleaseHoldBlocks;
        }

        // Release check: for every active notch, see if the frequency it's
        // sitting on has actually quieted down; if so count down, and free
        // the slot once it's been quiet for kReleaseHoldBlocks in a row.
        for (int i = 0; i < kMaxNotchesPerChannel; ++i) {
            if (!notchActive_[i]) continue;
            const float db = detector_.magnitudeAtDb(notches_[i].frequency());
            if (db < kReleaseThresholdDb) {
                if (--releaseCountdown_[i] <= 0) {
                    notches_[i].setEnabled(false);
                    notchActive_[i] = false;
                }
            } else {
                releaseCountdown_[i] = kReleaseHoldBlocks;
            }
        }
    } else {
        // Suppression toggled off: disable any active notches so the
        // channel runs clean (but keep their state so we don't need to
        // re-learn instantly if it's toggled back on).
        for (int i = 0; i < kMaxNotchesPerChannel; ++i) {
            notches_[i].setEnabled(false);
        }
    }

    // Apply the (up to 4) active notch filters in series.
    for (auto& notch : notches_) {
        notch.updateCoefficients();
        notch.processBlock(buf, n);
    }

    // Gain + mute.
    const float g = muted_.load(std::memory_order_relaxed)
                        ? 0.0f
                        : gain_.load(std::memory_order_relaxed);
    if (g != 1.0f) {
        for (size_t i = 0; i < n; ++i) {
            buf[i] *= g;
        }
    }
}

} // namespace audiomix
