#include "audio_io.h"

#include <algorithm>
#include <cstdio>

#include "mixer.h"

namespace audiomix {

AudioIO::AudioIO() {
    const PaError err = Pa_Initialize();
    paInitialized_ = (err == paNoError);
    if (!paInitialized_) {
        std::fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
    }
}

AudioIO::~AudioIO() {
    close();
    if (paInitialized_) {
        Pa_Terminate();
    }
}

void AudioIO::listDevices() {
    const int count = Pa_GetDeviceCount();
    if (count < 0) {
        std::fprintf(stderr, "Pa_GetDeviceCount failed: %s\n", Pa_GetErrorText(count));
        return;
    }
    std::printf("Available audio devices:\n");
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info) continue;
        std::printf("  [%d] %s  (in:%d out:%d, default sr:%.0f)\n", i, info->name,
                    info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate);
    }
}

bool AudioIO::open(Mixer* mixer, double sampleRateHz, unsigned long framesPerBuffer,
                    int numInputChannels, int numOutputChannels, std::string& errorOut) {
    if (!paInitialized_) {
        errorOut = "PortAudio failed to initialize";
        return false;
    }

    mixer_ = mixer;
    numInputChannels_ = numInputChannels;
    numOutputChannels_ = numOutputChannels;
    framesPerBuffer_ = framesPerBuffer;

    const PaDeviceIndex inDev = Pa_GetDefaultInputDevice();
    const PaDeviceIndex outDev = Pa_GetDefaultOutputDevice();

    if (inDev == paNoDevice) {
        errorOut = "no default input device found";
        return false;
    }
    if (outDev == paNoDevice) {
        errorOut = "no default output device found";
        return false;
    }

    const PaDeviceInfo* inInfo = Pa_GetDeviceInfo(inDev);
    const PaDeviceInfo* outInfo = Pa_GetDeviceInfo(outDev);

    if (!inInfo || inInfo->maxInputChannels < numInputChannels_) {
        errorOut = "default input device does not support " +
                   std::to_string(numInputChannels_) +
                   " input channels (has " +
                   std::to_string(inInfo ? inInfo->maxInputChannels : 0) +
                   "). Connect a multi-channel audio interface (e.g. a "
                   "4-in USB interface) or reduce the requested channel count.";
        return false;
    }
    if (!outInfo || outInfo->maxOutputChannels < numOutputChannels_) {
        errorOut = "default output device does not support " +
                   std::to_string(numOutputChannels_) + " output channels";
        return false;
    }

    PaStreamParameters inParams{};
    inParams.device = inDev;
    inParams.channelCount = numInputChannels_;
    inParams.sampleFormat = paFloat32; // interleaved
    inParams.suggestedLatency = inInfo->defaultLowInputLatency;
    inParams.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outParams{};
    outParams.device = outDev;
    outParams.channelCount = numOutputChannels_;
    outParams.sampleFormat = paFloat32; // interleaved
    outParams.suggestedLatency = outInfo->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    inputDeinterleaved_.assign(static_cast<size_t>(numInputChannels_),
                                std::vector<float>(framesPerBuffer_, 0.0f));
    inputChannelPtrs_.assign(static_cast<size_t>(numInputChannels_), nullptr);

    const PaError err = Pa_OpenStream(&stream_, &inParams, &outParams, sampleRateHz,
                                       framesPerBuffer_, paNoFlag, &AudioIO::paCallbackTrampoline,
                                       this);
    if (err != paNoError) {
        errorOut = std::string("Pa_OpenStream failed: ") + Pa_GetErrorText(err);
        stream_ = nullptr;
        return false;
    }

    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(stream_);
    if (streamInfo) {
        reportedLatencySeconds_ = streamInfo->inputLatency + streamInfo->outputLatency;
    } else {
        reportedLatencySeconds_ = inParams.suggestedLatency + outParams.suggestedLatency;
    }
    mixer_->setReportedLatencySeconds(reportedLatencySeconds_);

    return true;
}

bool AudioIO::start(std::string& errorOut) {
    if (!stream_) {
        errorOut = "stream not open";
        return false;
    }
    const PaError err = Pa_StartStream(stream_);
    if (err != paNoError) {
        errorOut = std::string("Pa_StartStream failed: ") + Pa_GetErrorText(err);
        return false;
    }
    running_ = true;
    return true;
}

bool AudioIO::stop() {
    if (!stream_ || !running_) return true;
    const PaError err = Pa_StopStream(stream_);
    running_ = false;
    return err == paNoError;
}

void AudioIO::close() {
    if (stream_) {
        if (running_) {
            Pa_StopStream(stream_);
            running_ = false;
        }
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
}

int AudioIO::paCallbackTrampoline(const void* input, void* output, unsigned long frameCount,
                                   const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                   PaStreamCallbackFlags /*statusFlags*/, void* userData) {
    auto* self = static_cast<AudioIO*>(userData);
    return self->paCallback(static_cast<const float*>(input), static_cast<float*>(output),
                             frameCount);
}

int AudioIO::paCallback(const float* interleavedInput, float* interleavedOutput,
                         unsigned long frameCount) {
    // Real-time thread: no allocation, no locks beyond Mixer's non-blocking
    // try_lock for the (optional) WAV writer.
    if (frameCount != framesPerBuffer_) {
        // PortAudio should always deliver exactly framesPerBuffer_ since we
        // requested a fixed (not "unspecified") buffer size; if a host API
        // ever hands us a short/partial final buffer, fail safe with silence
        // rather than reading/writing out of bounds.
        for (unsigned long i = 0; i < frameCount * static_cast<unsigned long>(numOutputChannels_);
             ++i) {
            interleavedOutput[i] = 0.0f;
        }
        return paContinue;
    }

    if (interleavedInput == nullptr) {
        // Input underflow/xrun: treat as silence for this block.
        for (int ch = 0; ch < numInputChannels_; ++ch) {
            std::fill(inputDeinterleaved_[static_cast<size_t>(ch)].begin(),
                      inputDeinterleaved_[static_cast<size_t>(ch)].end(), 0.0f);
        }
    } else {
        for (unsigned long i = 0; i < frameCount; ++i) {
            for (int ch = 0; ch < numInputChannels_; ++ch) {
                inputDeinterleaved_[static_cast<size_t>(ch)][i] =
                    interleavedInput[i * static_cast<unsigned long>(numInputChannels_) +
                                      static_cast<unsigned long>(ch)];
            }
        }
    }

    for (int ch = 0; ch < numInputChannels_; ++ch) {
        inputChannelPtrs_[static_cast<size_t>(ch)] = inputDeinterleaved_[static_cast<size_t>(ch)].data();
    }

    if (interleavedOutput != nullptr) {
        mixer_->process(inputChannelPtrs_.data(), interleavedOutput, frameCount);
    }

    return paContinue;
}

} // namespace audiomix
