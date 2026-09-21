#pragma once
//
// channel.h
//
// One mono input channel of the mixer: gain/mute, a per-channel feedback
// detector, and a small bank of notch filters that get dynamically tuned
// onto whatever frequencies the detector flags as ringing.
//
#include <array>
#include <atomic>
#include <cstddef>

#include "feedback_detector.h"
#include "notch_filter.h"

namespace audiomix {

// Up to 4 simultaneous feedback notches per channel. In practice a room
// rarely rings at more than 1-2 frequencies at once; 4 gives headroom for
// a genuinely problematic room/PA combination without letting the filter
// bank grow unbounded.
constexpr int kMaxNotchesPerChannel = 4;

class Channel {
public:
    Channel();

    // Non-real-time: call once at startup.
    void configure(double sampleRateHz, size_t blockSize, double notchQ);

    // Thread-safe controls (CLI thread) -----------------------------------
    void setGain(float linearGain);
    float gain() const { return gain_.load(std::memory_order_relaxed); }

    void setMuted(bool muted);
    bool muted() const { return muted_.load(std::memory_order_relaxed); }

    void setFeedbackSuppressionEnabled(bool enabled);
    bool feedbackSuppressionEnabled() const {
        return suppressionEnabled_.load(std::memory_order_relaxed);
    }

    void setSensitivity(float sensitivity01);
    float sensitivity() const { return detector_.sensitivity(); }

    // Number of currently-active suppression notches (for status output).
    int activeNotchCount() const;

    // Audio-thread only ----------------------------------------------------
    // Processes `n` samples in place: feedback detection -> notch
    // suppression -> gain -> mute.
    void process(float* buf, size_t n);

    int index = 0;

private:
    int findSlotFor(double freqHz) const;
    int findFreeSlot() const;
    int findOldestSlot() const;

    double sampleRate_ = 48000.0;
    size_t blockSize_ = 256;

    std::atomic<float> gain_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> suppressionEnabled_{true};

    FeedbackDetector detector_;

    std::array<NotchFilter, kMaxNotchesPerChannel> notches_;
    std::array<bool, kMaxNotchesPerChannel> notchActive_{};
    std::array<int, kMaxNotchesPerChannel> releaseCountdown_{}; // audio-thread only
    std::array<unsigned long, kMaxNotchesPerChannel> notchAge_{}; // for LRU replacement

    unsigned long blockCounter_ = 0; // audio-thread only

    // How many consecutive blocks a notched frequency must stay quiet
    // (below release threshold) before we free that notch slot. At 256
    // samples / 48kHz this is ~1.3s of held suppression, which avoids
    // rapid on/off "chattering" on borderline material.
    static constexpr int kReleaseHoldBlocks = 120;
    static constexpr float kReleaseThresholdDb = -18.0f; // relative drop to release
};

} // namespace audiomix
