#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <sstream>

namespace audiomix {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    return tokens;
}

bool parseDouble(const std::string& s, double& out) {
    try {
        size_t pos = 0;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

bool parseInt(const std::string& s, int& out) {
    try {
        size_t pos = 0;
        out = std::stoi(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

// Simple soft limiter (tanh) applied to the master bus so an overload
// (several channels clipping at once) rolls off smoothly instead of
// hard-clipping into a harsh digital crunch.
inline float softLimit(float x) {
    constexpr float kThreshold = 0.891f; // ~ -1dBFS, where soft-knee begins
    const float ax = std::fabs(x);
    if (ax <= kThreshold) return x;
    const float sign = (x < 0.0f) ? -1.0f : 1.0f;
    const float over = ax - kThreshold;
    return sign * (kThreshold + std::tanh(over * 4.0f) / 4.0f);
}

} // namespace

Mixer::Mixer() = default;

Mixer::~Mixer() {
    stopRecording();
}

void Mixer::configure(double sampleRateHz, size_t blockSize) {
    sampleRate_ = sampleRateHz;
    blockSize_ = blockSize;

    for (int i = 0; i < kNumChannels; ++i) {
        channels_[static_cast<size_t>(i)].index = i;
        channels_[static_cast<size_t>(i)].configure(sampleRate_, blockSize_, kDefaultNotchQ);
        channelScratch_[i].assign(blockSize_, 0.0f);
    }

    recordScratch_.assign(blockSize_ * static_cast<size_t>(kNumOutputChannels), 0.0f);
}

void Mixer::setMasterGain(float linearGain) {
    masterGain_.store(std::max(0.0f, linearGain), std::memory_order_relaxed);
}

void Mixer::process(const float* const* inputs, float* interleavedOutput, size_t n) {
    // Equal-power center pan constant (each mono channel routed evenly to
    // L and R): 1/sqrt(2) keeps a center-panned signal at the same
    // perceived loudness as a hard-panned one.
    constexpr float kCenterPan = 0.70710678f;

    const float masterGain = masterGain_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < n * static_cast<size_t>(kNumOutputChannels); ++i) {
        interleavedOutput[i] = 0.0f;
    }

    for (int ch = 0; ch < kNumChannels; ++ch) {
        float* scratch = channelScratch_[ch].data();
        std::copy(inputs[ch], inputs[ch] + n, scratch);

        channels_[static_cast<size_t>(ch)].process(scratch, n);

        for (size_t i = 0; i < n; ++i) {
            const float s = scratch[i] * kCenterPan * masterGain;
            interleavedOutput[2 * i] += s;     // L
            interleavedOutput[2 * i + 1] += s; // R
        }
    }

    for (size_t i = 0; i < n * static_cast<size_t>(kNumOutputChannels); ++i) {
        interleavedOutput[i] = softLimit(interleavedOutput[i]);
    }

    if (recording_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(recMutex_, std::try_to_lock);
        if (lock.owns_lock() && recFile_ != nullptr) {
            sf_writef_float(recFile_, interleavedOutput, static_cast<sf_count_t>(n));
        }
        // If the lock wasn't available (a start/stop is in flight on the
        // CLI thread right now), we intentionally skip writing this block
        // rather than block the audio callback.
    }
}

void Mixer::applySensitivityToAllChannels(float s01) {
    sensitivityGlobal_.store(s01, std::memory_order_relaxed);
    for (auto& c : channels_) c.setSensitivity(s01);
}

void Mixer::applySuppressionToAllChannels(bool enabled) {
    feedbackSuppressionGlobal_.store(enabled, std::memory_order_relaxed);
    for (auto& c : channels_) c.setFeedbackSuppressionEnabled(enabled);
}

bool Mixer::startRecording(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lock(recMutex_);
    if (recFile_ != nullptr) {
        err = "already recording (call 'record stop' first)";
        return false;
    }

    SF_INFO info{};
    info.samplerate = static_cast<int>(sampleRate_);
    info.channels = kNumOutputChannels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) {
        err = std::string("could not open '") + path + "' for writing: " + sf_strerror(nullptr);
        return false;
    }

    recFile_ = f;
    recInfo_ = info;
    recording_.store(true, std::memory_order_relaxed);
    return true;
}

void Mixer::stopRecording() {
    std::lock_guard<std::mutex> lock(recMutex_);
    recording_.store(false, std::memory_order_relaxed);
    if (recFile_ != nullptr) {
        sf_close(recFile_);
        recFile_ = nullptr;
    }
}

std::string Mixer::statusString() const {
    std::ostringstream oss;
    oss << "master gain=" << masterGain_.load() << " | feedback suppress="
        << (feedbackSuppressionGlobal_.load() ? "ON" : "OFF")
        << " | sensitivity=" << sensitivityGlobal_.load()
        << " | recording=" << (recording_.load() ? "YES" : "no")
        << " | latency=" << (reportedLatencySeconds_ * 1000.0) << "ms\n";
    for (int i = 0; i < kNumChannels; ++i) {
        const auto& c = channels_[static_cast<size_t>(i)];
        oss << "  ch" << (i + 1) << ": gain=" << c.gain() << (c.muted() ? " [MUTED]" : "")
            << " active_notches=" << c.activeNotchCount();
        if (i != kNumChannels - 1) oss << "\n";
    }
    return oss.str();
}

bool Mixer::handleCommand(const std::string& line, std::string& response) {
    auto tokens = tokenize(line);
    if (tokens.empty()) {
        response = "";
        return true;
    }
    for (auto& t : tokens) t = toLower(t);

    // channel <n> gain <g>
    // channel <n> mute
    // channel <n> unmute
    if (tokens[0] == "channel") {
        if (tokens.size() < 2) {
            response = "usage: channel <1-4> gain <0.0-4.0> | channel <1-4> mute|unmute";
            return false;
        }
        int chNum = 0;
        if (!parseInt(tokens[1], chNum) || chNum < 1 || chNum > kNumChannels) {
            response = "error: channel number must be 1-" + std::to_string(kNumChannels);
            return false;
        }
        Channel& ch = channels_[static_cast<size_t>(chNum - 1)];

        if (tokens.size() >= 4 && tokens[2] == "gain") {
            double g = 0.0;
            if (!parseDouble(tokens[3], g) || g < 0.0) {
                response = "error: gain must be a non-negative number (e.g. 0.7)";
                return false;
            }
            ch.setGain(static_cast<float>(g));
            std::ostringstream oss;
            oss << "OK: channel " << chNum << " gain -> " << g;
            response = oss.str();
            return true;
        }
        if (tokens.size() >= 3 && tokens[2] == "mute") {
            ch.setMuted(true);
            response = "OK: channel " + std::to_string(chNum) + " muted";
            return true;
        }
        if (tokens.size() >= 3 && tokens[2] == "unmute") {
            ch.setMuted(false);
            response = "OK: channel " + std::to_string(chNum) + " unmuted";
            return true;
        }
        response = "usage: channel <1-4> gain <value> | channel <1-4> mute|unmute";
        return false;
    }

    // feedback suppress on|off
    if (tokens[0] == "feedback") {
        if (tokens.size() >= 3 && tokens[1] == "suppress") {
            if (tokens[2] == "on") {
                applySuppressionToAllChannels(true);
                response = "OK: feedback suppression ON";
                return true;
            }
            if (tokens[2] == "off") {
                applySuppressionToAllChannels(false);
                response = "OK: feedback suppression OFF";
                return true;
            }
        }
        response = "usage: feedback suppress ON|OFF";
        return false;
    }

    // sensitivity <0.0-1.0>
    if (tokens[0] == "sensitivity") {
        if (tokens.size() < 2) {
            response = "usage: sensitivity <0.0-1.0>";
            return false;
        }
        double s = 0.0;
        if (!parseDouble(tokens[1], s) || s < 0.0 || s > 1.0) {
            response = "error: sensitivity must be between 0.0 and 1.0";
            return false;
        }
        applySensitivityToAllChannels(static_cast<float>(s));
        std::ostringstream oss;
        oss << "OK: sensitivity -> " << s;
        response = oss.str();
        return true;
    }

    // record <filename.wav>
    // record stop
    if (tokens[0] == "record") {
        if (tokens.size() < 2) {
            response = "usage: record <filename.wav> | record stop";
            return false;
        }
        if (tokens[1] == "stop") {
            if (!isRecording()) {
                response = "not currently recording";
                return true;
            }
            stopRecording();
            response = "OK: recording stopped";
            return true;
        }
        // Reconstruct original-case filename (tokens[] was lowercased).
        auto rawTokens = tokenize(line);
        const std::string& path = rawTokens[1];
        std::string err;
        if (!startRecording(path, err)) {
            response = "error: " + err;
            return false;
        }
        response = "OK: recording to '" + path + "'";
        return true;
    }

    // master gain <g>  (bonus, not in required spec but harmless/useful)
    if (tokens[0] == "master" && tokens.size() >= 3 && tokens[1] == "gain") {
        double g = 0.0;
        if (!parseDouble(tokens[2], g) || g < 0.0) {
            response = "error: master gain must be a non-negative number";
            return false;
        }
        setMasterGain(static_cast<float>(g));
        response = "OK: master gain -> " + std::to_string(g);
        return true;
    }

    if (tokens[0] == "status") {
        response = statusString();
        return true;
    }

    if (tokens[0] == "help") {
        response =
            "Commands:\n"
            "  channel <1-4> gain <0.0-4.0>   set a channel's linear gain\n"
            "  channel <1-4> mute|unmute      mute/unmute a channel\n"
            "  feedback suppress ON|OFF       enable/disable feedback suppression\n"
            "  sensitivity <0.0-1.0>          detector sensitivity (all channels)\n"
            "  record <file.wav>              start recording master output\n"
            "  record stop                    stop recording\n"
            "  master gain <0.0-4.0>          set master bus gain\n"
            "  status                         show current mixer state\n"
            "  quit | exit                    shut down";
        return true;
    }

    response = "error: unrecognized command '" + line + "' (type 'help')";
    return false;
}

} // namespace audiomix
