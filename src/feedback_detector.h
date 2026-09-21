#pragma once
//
// feedback_detector.h
//
// Per-channel acoustic feedback (howl-round) detector.
//
// How it works
// ------------
// Classic feedback detectors (Sabine/Behringer/etc. "feedback destroyers")
// look for narrowband spectral peaks that are both:
//   1. Much louder than their surrounding frequencies ("peakiness"), and
//   2. Sustained / growing over consecutive analysis blocks, rather than a
//      one-off transient (a struck note, a consonant, a clap).
//
// Music and speech rarely hold a single, isolated FFT bin far above its
// neighbors for more than a few tens of milliseconds. A closed-loop
// feedback ring, by contrast, is a resonance: it keeps re-injecting energy
// at (approximately) one exact frequency every time the signal goes
// mic -> mixer -> speaker -> room -> mic again, so that bin's energy stays
// elevated call after call. That combination (peakiness + persistence) is
// what this class tests for.
//
// Implementation
// ---------------
// - Samples are pushed in fixed-size hops (== the audio callback block
//   size, 256 samples / 5.33 ms at 48 kHz).
// - Internally we keep a longer analysis window (default 1024 samples,
//   46.9 Hz bin spacing at 48 kHz) so we get usable frequency resolution
//   even though the I/O block is short. The window slides forward by one
//   hop every call (75% overlap at the default sizes).
// - A Hann window + real-input FFTW plan (created once, reused every
//   block -> no per-block allocation, real-time safe) produces the
//   magnitude spectrum.
// - For each bin we estimate a local noise floor from neighboring bins
//   (skipping an exclusion zone right around the bin itself) and compute
//   peakiness = magnitude / noiseFloor.
// - A per-bin integer "ring counter" increments while peakiness clears the
//   sensitivity-scaled threshold and decays otherwise. Once a counter
//   crosses the required persistence count, that bin is reported as a new
//   feedback event (frequency + confidence in [0,1]).
// - magnitudeAtDb() lets the owning Channel keep checking whether an
//   already-notched frequency is still ringing (to decide whether to hold
//   or release that notch).
//
#include <complex>
#include <cstddef>
#include <vector>

#include <fftw3.h>

namespace audiomix {

struct FeedbackEvent {
    double frequencyHz;
    float confidence; // 0..1, roughly how far past threshold this triggered
};

class FeedbackDetector {
public:
    FeedbackDetector();
    ~FeedbackDetector();

    FeedbackDetector(const FeedbackDetector&) = delete;
    FeedbackDetector& operator=(const FeedbackDetector&) = delete;

    // Non-real-time: allocates the FFTW plan and buffers. Call once at
    // startup. hopSize must equal the audio callback block size.
    void configure(double sampleRateHz, size_t fftSize, size_t hopSize);

    // sensitivity in [0,1]: higher = triggers faster / on smaller peaks.
    void setSensitivity(float sensitivity01);
    float sensitivity() const { return sensitivity_; }

    // Audio-thread only. `block` must have exactly hopSize_ samples.
    // Returns any newly-triggered feedback events this call (usually 0 or 1).
    std::vector<FeedbackEvent> process(const float* block, size_t n);

    // Audio-thread only. Approximate current magnitude, in dB, of the last
    // computed spectrum at the given frequency (nearest bin). Used by the
    // owning Channel to decide whether an active notch is still needed.
    float magnitudeAtDb(double freqHz) const;

private:
    void pushSamples(const float* block, size_t n);
    void computeSpectrum();

    double sampleRate_ = 48000.0;
    size_t fftSize_ = 1024;
    size_t hopSize_ = 256;
    size_t numBins_ = 0;

    float sensitivity_ = 0.5f;

    // Sliding analysis ring buffer (raw, unwindowed samples).
    std::vector<float> ring_;
    size_t ringWritePos_ = 0;
    size_t samplesBuffered_ = 0;

    // Hann window, precomputed.
    std::vector<double> window_;

    // FFTW real-to-complex plan and buffers.
    double* fftIn_ = nullptr;         // fftSize_ samples, windowed
    fftw_complex* fftOut_ = nullptr;  // fftSize_/2 + 1 complex bins
    fftw_plan plan_ = nullptr;

    std::vector<double> magnitude_;   // linear magnitude per bin, last block
    std::vector<int> ringCount_;      // persistence counters per bin
    std::vector<bool> flagged_;       // currently-reported (avoid re-spam)

    static constexpr int kExclusionBins = 2;   // ignore bins this close to k
    static constexpr int kNeighborhoodBins = 20; // noise-floor estimation span
};

} // namespace audiomix
