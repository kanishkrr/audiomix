#pragma once
//
// audio_io.h
//
// Thin wrapper around PortAudio: opens a single full-duplex stream
// (numInputChannels in, numOutputChannels out, fixed framesPerBuffer,
// interleaved float32) and forwards each callback to Mixer::process().
//
#include <cstddef>
#include <string>
#include <vector>

#include <portaudio.h>

namespace audiomix {

class Mixer;

class AudioIO {
public:
    AudioIO();
    ~AudioIO();

    // Opens the stream. Chooses the system default input/output devices
    // and requests the lowest suggested latency each device reports, so
    // the host API (ALSA/CoreAudio/WASAPI/etc.) can pick the smallest
    // buffering it's able to guarantee glitch-free.
    //
    // framesPerBuffer is fixed (not "unspecified") because we need
    // deterministic, small blocks for the <7ms latency target and for the
    // feedback detector's block-synchronous analysis.
    bool open(Mixer* mixer, double sampleRateHz, unsigned long framesPerBuffer,
              int numInputChannels, int numOutputChannels, std::string& errorOut);

    bool start(std::string& errorOut);
    bool stop();
    void close();

    bool isRunning() const { return running_; }

    // Sum of PortAudio-reported input + output stream latency, in seconds.
    // This is the actual round-trip figure to compare against the <7ms
    // target; it depends on the OS audio backend and physical interface,
    // not just on framesPerBuffer, so it's only known after open().
    double reportedLatencySeconds() const { return reportedLatencySeconds_; }

    static void listDevices();

private:
    static int paCallbackTrampoline(const void* input, void* output, unsigned long frameCount,
                                     const PaStreamCallbackTimeInfo* timeInfo,
                                     PaStreamCallbackFlags statusFlags, void* userData);

    int paCallback(const float* interleavedInput, float* interleavedOutput,
                   unsigned long frameCount);

    PaStream* stream_ = nullptr;
    Mixer* mixer_ = nullptr;
    bool paInitialized_ = false;
    bool running_ = false;
    double reportedLatencySeconds_ = 0.0;

    int numInputChannels_ = 4;
    int numOutputChannels_ = 2;
    unsigned long framesPerBuffer_ = 256;

    // Deinterleave scratch for the input side (audio-thread only,
    // preallocated at open() so the callback never allocates).
    std::vector<std::vector<float>> inputDeinterleaved_; // [ch][frames]
    std::vector<const float*> inputChannelPtrs_;
};

} // namespace audiomix
