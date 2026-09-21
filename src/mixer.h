#pragma once
//
// mixer.h
//
// Owns the fixed set of input Channels, sums them to a stereo master bus,
// applies master gain and a soft limiter, optionally records the master
// output to a WAV file, and parses/dispatches the CLI command language.
//
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <sndfile.h>

#include "channel.h"

namespace audiomix {

constexpr int kNumChannels = 4;
constexpr int kNumOutputChannels = 2; // stereo master bus
constexpr double kDefaultNotchQ = 10.0;

class Mixer {
public:
    Mixer();
    ~Mixer();

    // Non-real-time: call once at startup.
    void configure(double sampleRateHz, size_t blockSize);

    // Audio-thread only. `inputs` is an array of kNumChannels pointers,
    // each to n mono samples. `interleavedOutput` receives n frames of
    // interleaved stereo (2*n floats).
    void process(const float* const* inputs, float* interleavedOutput, size_t n);

    Channel& channel(int idx) { return channels_[static_cast<size_t>(idx)]; }
    const Channel& channel(int idx) const { return channels_[static_cast<size_t>(idx)]; }

    void setMasterGain(float linearGain);
    float masterGain() const { return masterGain_.load(std::memory_order_relaxed); }

    // Parses one CLI command line and applies it. Always fills `response`
    // with a human-readable result (success message or error). Returns
    // false only on a malformed/unrecognized command (response still set).
    bool handleCommand(const std::string& line, std::string& response);

    // Recording ------------------------------------------------------------
    bool startRecording(const std::string& path, std::string& err);
    void stopRecording();
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }

    // Set by AudioIO after opening the stream, purely for status reporting.
    void setReportedLatencySeconds(double s) { reportedLatencySeconds_ = s; }
    double reportedLatencySeconds() const { return reportedLatencySeconds_; }

    std::string statusString() const;

private:
    void applySensitivityToAllChannels(float s01);
    void applySuppressionToAllChannels(bool enabled);

    std::array<Channel, kNumChannels> channels_;
    std::atomic<float> masterGain_{1.0f};
    std::atomic<bool> feedbackSuppressionGlobal_{true};
    std::atomic<float> sensitivityGlobal_{0.5f};

    double sampleRate_ = 48000.0;
    size_t blockSize_ = 256;
    double reportedLatencySeconds_ = 0.0;

    // Mixing scratch buffers (audio-thread only, preallocated).
    std::vector<float> channelScratch_[kNumChannels];

    // Recording state. Opened/closed from the CLI thread; written from the
    // audio thread using a non-blocking try_lock so a start/stop transition
    // can never stall the real-time callback (a block is simply dropped,
    // not written, on the rare occasion the lock is contended).
    mutable std::mutex recMutex_;
    SNDFILE* recFile_ = nullptr;
    SF_INFO recInfo_{};
    std::atomic<bool> recording_{false};
    std::vector<float> recordScratch_; // interleaved stereo, audio-thread only
};

} // namespace audiomix
