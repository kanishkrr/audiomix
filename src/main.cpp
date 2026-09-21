// main.cpp
//
// AudioMix — real-time 4-channel mixing engine with acoustic feedback
// detection and suppression.
//
// Usage:
//   audiomix                 run with defaults (48kHz, 256-sample blocks,
//                             default system audio devices)
//   audiomix --list-devices  print available PortAudio devices and exit
//   audiomix --help          print this usage text and exit
//
// Once running, type commands at the prompt (see `help`):
//   channel 1 gain 0.7
//   feedback suppress ON
//   sensitivity 0.8
//   record output.wav
//   record stop
//   status
//   quit
//
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "audio_io.h"
#include "mixer.h"

#ifdef _WIN32
// Windows doesn't have POSIX select()-on-stdin; fall back to a plain
// blocking read loop. Ctrl+C still terminates the process (recording may
// be truncated); use the 'quit' command for a clean shutdown.
#define AUDIOMIX_SIMPLE_STDIN_LOOP 1
#else
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#define AUDIOMIX_SIMPLE_STDIN_LOOP 0
#endif

namespace {

// Only async-signal-safe operations here: set a flag, nothing else.
volatile std::sig_atomic_t gRunning = 1;

void onSignal(int /*sig*/) { gRunning = 0; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void printBanner(const audiomix::AudioIO& io, double sampleRate, unsigned long blockSize) {
    std::printf("=====================================================\n");
    std::printf(" AudioMix - real-time mixing engine with feedback AI\n");
    std::printf("=====================================================\n");
    std::printf("Sample rate:        %.0f Hz\n", sampleRate);
    std::printf("Block size:         %lu samples (%.2f ms)\n", blockSize,
                1000.0 * static_cast<double>(blockSize) / sampleRate);
    const double latencyMs = io.reportedLatencySeconds() * 1000.0;
    std::printf("Reported I/O latency: %.2f ms\n", latencyMs);
    if (latencyMs > 7.0) {
        std::printf(
            "WARNING: reported latency exceeds the 7ms target. This is\n"
            "         determined by your OS audio backend/driver and\n"
            "         physical interface, not just this program. On\n"
            "         Linux, try a JACK or low-latency ALSA setup; on\n"
            "         macOS, CoreAudio is usually already low-latency;\n"
            "         on Windows, prefer an ASIO-capable interface.\n");
    }
    std::printf("-----------------------------------------------------\n");
    std::printf("Type 'help' for commands, 'quit' to exit.\n\n");
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-devices") == 0) {
            audiomix::AudioIO::listDevices();
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "AudioMix - real-time 4-channel mixer with feedback detection\n\n"
                "Usage:\n"
                "  audiomix                 run with default audio devices\n"
                "  audiomix --list-devices  print available audio devices and exit\n"
                "  audiomix --help          show this message\n");
            return 0;
        }
    }

    constexpr double kSampleRate = 48000.0;
    constexpr unsigned long kBlockSize = 256;
    constexpr int kNumInputChannels = audiomix::kNumChannels;      // 4
    constexpr int kNumOutputChannels = audiomix::kNumOutputChannels; // 2 (stereo master)

    audiomix::Mixer mixer;
    mixer.configure(kSampleRate, kBlockSize);

    audiomix::AudioIO audioIO;
    std::string err;
    if (!audioIO.open(&mixer, kSampleRate, kBlockSize, kNumInputChannels, kNumOutputChannels,
                       err)) {
        std::fprintf(stderr, "Failed to open audio stream: %s\n", err.c_str());
        std::fprintf(stderr, "Run 'audiomix --list-devices' to see available devices.\n");
        return 1;
    }
    if (!audioIO.start(err)) {
        std::fprintf(stderr, "Failed to start audio stream: %s\n", err.c_str());
        return 1;
    }

    printBanner(audioIO, kSampleRate, kBlockSize);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

#if AUDIOMIX_SIMPLE_STDIN_LOOP
    std::string line;
    while (gRunning && std::getline(std::cin, line)) {
        std::string cmd = trim(line);
        if (cmd.empty()) continue;
        std::string lower = cmd;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "quit" || lower == "exit") break;
        std::string response;
        mixer.handleCommand(cmd, response);
        if (!response.empty()) std::printf("%s\n", response.c_str());
    }
#else
    std::printf("> ");
    std::fflush(stdout);
    std::string pending;
    while (gRunning) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms poll: responsive to Ctrl+C without busy-waiting
        const int rv = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (rv > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                break; // stdin closed (EOF)
            }
            std::string cmd = trim(line);
            if (!cmd.empty()) {
                std::string lower = cmd;
                for (auto& c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower == "quit" || lower == "exit") break;
                std::string response;
                mixer.handleCommand(cmd, response);
                if (!response.empty()) std::printf("%s\n", response.c_str());
            }
            std::printf("> ");
            std::fflush(stdout);
        }
        // else: select() timed out; loop back around to re-check gRunning
        // (this is what makes Ctrl+C responsive even while blocked "on
        // input").
    }
#endif

    std::printf("\nShutting down...\n");
    audioIO.stop();
    mixer.stopRecording(); // finalize WAV header if a recording was in progress
    audioIO.close();

    return 0;
}
