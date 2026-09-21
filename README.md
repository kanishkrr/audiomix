# AudioMix

A real-time, 4-channel audio mixing engine in C++ with automatic acoustic
feedback (howl-round) detection and suppression. Built for live sound /
event-production use: mic inputs -> per-channel gain -> feedback-aware
notch filtering -> stereo master bus -> speakers, with optional
record-to-WAV.

## Features

- 4 mono input channels -> stereo master bus, 48kHz, fixed 256-sample
  blocks (5.33ms per block), via PortAudio.
- Per-channel FFT-based feedback detector (FFTW) that looks for narrowband
  spectral peaks that are both louder than their surroundings *and*
  sustained across consecutive blocks — the signature of a closed-loop
  feedback ring, as opposed to a transient note or consonant.
- Automatic biquad IIR notch filters (RBJ "Audio EQ Cookbook" design,
  Q ~ 10) that get tuned onto a detected ringing frequency in real time,
  using sub-bin parabolic interpolation so the notch lands close to the
  true frequency rather than the nearest ~47Hz-wide FFT bin.
- Live CLI for gain, feedback suppression, sensitivity, and WAV recording.
- Soft (tanh) limiter on the master bus so multi-channel overload rolls off
  smoothly instead of hard-clipping.

## Requirements

- CMake >= 3.16
- A C++17 compiler (tested with GCC 13 and Clang)
- [PortAudio](http://www.portaudio.com/) (v19)
- [FFTW3](http://www.fftw.org/) (double precision)
- [libsndfile](http://libsndfile.github.io/libsndfile/) (for WAV recording)

### Installing dependencies

**Debian/Ubuntu:**
```bash
sudo apt-get install build-essential cmake pkg-config \
    portaudio19-dev libfftw3-dev libsndfile1-dev
```

**macOS (Homebrew):**
```bash
brew install cmake pkg-config portaudio fftw libsndfile
```

**Windows:**
The easiest path is [vcpkg](https://github.com/microsoft/vcpkg):
```powershell
vcpkg install portaudio fftw3 libsndfile
```
then configure CMake with
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake`.
(The project's CLI input loop uses a POSIX `select()`-based approach for
responsive Ctrl+C handling on Linux/macOS; on Windows it falls back to a
plain blocking read — see "Known limitations" below.)

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

This produces the `audiomix` executable in `build/`.

## Running

```bash
./audiomix                 # run with the system's default audio devices
./audiomix --list-devices  # list available PortAudio devices and exit
./audiomix --help          # usage
```

On startup, AudioMix opens a full-duplex stream on your default audio
input/output device (4 input channels required — see "Hardware" below),
prints the PortAudio-reported round-trip latency, and drops you into an
interactive command prompt.

### CLI commands

| Command                          | Effect                                              |
|-----------------------------------|------------------------------------------------------|
| `channel <1-4> gain <0.0-4.0>`    | Set a channel's linear gain (e.g. `channel 1 gain 0.7`) |
| `channel <1-4> mute` / `unmute`  | Mute/unmute a channel                                |
| `feedback suppress ON` / `OFF`   | Enable/disable automatic feedback suppression         |
| `sensitivity <0.0-1.0>`          | Feedback detector sensitivity, applied to all channels |
| `record <file.wav>`              | Start recording the master output to a WAV file       |
| `record stop`                    | Stop the current recording                            |
| `master gain <0.0-4.0>`          | Set master bus gain                                   |
| `status`                         | Print current gains, mutes, notch activity, latency    |
| `help`                           | List commands                                         |
| `quit` / `exit`                  | Shut down cleanly (finalizes any in-progress WAV file) |

Example session:
```
> channel 1 gain 0.7
OK: channel 1 gain -> 0.7
> sensitivity 0.8
OK: sensitivity -> 0.8
> feedback suppress ON
OK: feedback suppression ON
> record output.wav
OK: recording to 'output.wav'
> status
master gain=1 | feedback suppress=ON | sensitivity=0.8 | recording=YES | latency=5.80ms
  ch1: gain=0.7 active_notches=0
  ch2: gain=1 active_notches=0
  ch3: gain=1 active_notches=0
  ch4: gain=1 active_notches=0
> record stop
OK: recording stopped
> quit
```

Ctrl+C also shuts the process down cleanly (stream stopped, any active WAV
recording finalized) on Linux/macOS.

## Architecture

```
 mic/line in x4                                          speakers/interface out
      |                                                            ^
      v                                                            |
 +-----------+   +---------+   +------------------+   +-----------+
 | AudioIO   |-->| Channel |-->| notch filter bank|-->| Mixer     |--> stereo out
 | (PortAudio|   | (gain,  |   | (up to 4 biquads |   | (sum,     |      |
 |  callback)|   |  mute)  |   |  per channel)    |   |  master   |      v
 +-----------+   +----+----+   +--------^---------+   |  gain,    |   WAV file
                      |                 |              |  limiter, |  (libsndfile,
                      v                 |              |  record)  |   optional)
                +------------+          |              +-----------+
                | Feedback   |----------+
                | Detector   |  (tunes notch frequency
                | (FFTW)     |   onto detected ring)
                +------------+
```

- **`audio_io`** wraps PortAudio: opens one full-duplex stream (4 in / 2
  out, `paFloat32`, fixed 256-frame callback), deinterleaves the input,
  and calls `Mixer::process()` once per callback. The real-time callback
  never allocates.
- **`channel`** owns one mono input's gain/mute state, its
  `FeedbackDetector`, and a bank of up to 4 `NotchFilter`s. Each block: run
  detection on the pre-notch signal -> activate/retune/release notches as
  needed -> apply the active notch chain -> apply gain/mute.
- **`feedback_detector`** keeps a 1024-sample analysis window (75% overlap
  at the 256-sample hop), Hann-windowed, and runs one real FFT (FFTW,
  plan created once at startup) per block. It flags a bin as "ringing"
  when its magnitude is both far above its local neighborhood (peakiness)
  and has stayed that way for several consecutive blocks (persistence) —
  the combination that separates a resonant feedback loop from ordinary
  transient program material. Sub-bin frequency is refined via parabolic
  interpolation across the peak bin and its neighbors.
- **`notch_filter`** is an RBJ-cookbook biquad notch (Q configurable, 10 by
  default). Frequency/Q/enabled are atomics settable from any thread;
  coefficient recomputation happens on the audio thread itself right
  before processing, so there's no locking on the real-time path.
- **`mixer`** sums the 4 processed channels to a stereo bus with
  equal-power center panning, applies master gain and a soft (tanh)
  limiter, optionally taps the output to a WAV file (via a non-blocking
  `try_lock` so a `record`/`record stop` transition can never stall the
  audio callback), and parses the CLI command language.

### Thread-safety model

- The PortAudio callback runs on its own real-time thread. `main()`'s CLI
  loop runs on the main thread and only ever touches shared state through
  `std::atomic` fields (gain, mute, sensitivity, suppression on/off,
  notch target frequency) or, for the WAV file handle, a mutex the audio
  thread only ever `try_lock`s (never blocks).
- No heap allocation happens inside `AudioIO::paCallback`, `Mixer::process`,
  `Channel::process`, `FeedbackDetector::process`, or `NotchFilter::process*`
  — all buffers (FFTW plans/arrays, scratch buffers, notch banks) are sized
  and allocated once in the various `configure()` calls at startup.

## Latency

Target: **< 7ms** total round-trip (input device -> processing -> output
device).

At 256 samples / 48kHz, one buffer alone is ~5.33ms. AudioIO requests each
device's lowest suggested latency
(`PaDeviceInfo::defaultLowInputLatency` / `defaultLowOutputLatency`) and
opens the stream with a *fixed* 256-frame callback (not
`paFramesPerBufferUnspecified`), then reports the actual
PortAudio-measured round-trip (`PaStreamInfo::inputLatency +
outputLatency`) at startup, along with a warning if it exceeds 7ms.

**Whether you actually land under 7ms depends on your OS audio backend and
interface**, not just this code:
- **macOS**: CoreAudio is low-latency by default on built-in and most
  class-compliant USB interfaces; 256-sample buffers at 48kHz typically
  land well under 7ms round-trip.
- **Linux**: plain ALSA can vary a lot by driver; for consistently low,
  predictable latency use JACK or a real-time-patched kernel with a
  low-latency ALSA configuration.
- **Windows**: WDM/MME latency is typically too high for this target; use
  an ASIO-capable audio interface (most dedicated USB audio interfaces
  ship ASIO drivers) — PortAudio will pick it up as a distinct device.

Run with `./audiomix` and check the printed "Reported I/O latency" line;
use `./audiomix --list-devices` to see what's available if you need to
pick a specific low-latency interface.

## Hardware

The default configuration expects an audio interface exposing at least 4
input channels and 2 output channels (e.g. a Focusrite Scarlett 4i4,
Behringer UMC404HD, or similar small-format USB interface commonly used
in live sound / event production). If your default device doesn't have 4
inputs, `audiomix` will print a clear error naming the shortfall; use
`--list-devices` to see channel counts for everything PortAudio can see
and pick/attach an interface accordingly.

## Known limitations / things a production deployment would add next

- **Fixed 4-in / stereo-out topology.** Channels are always center-panned;
  there's no per-channel pan control. Straightforward to add
  (`Mixer::process` already has the per-sample pan multiply — just make it
  per-channel and atomic instead of a fixed constant).
- **WAV recording taps the post-limiter master bus only**, not
  individual channels. Multitrack recording would mean N `SNDFILE*`
  writers instead of one.
- **CLI input loop uses POSIX `select()`** for responsive Ctrl+C handling
  without a second thread; on Windows this falls back to a simple
  blocking `std::getline` loop (Ctrl+C still terminates the process, but
  cleanup/finalization of an in-progress recording relies on you typing
  `quit` first for a guaranteed-clean WAV file).
- **Device selection is "system default" only.** For a specific audio
  interface, extend `AudioIO::open()` to accept a device index (already
  exposed via `--list-devices`); this is a small, contained change since
  the device lookup is already isolated in that one function.
- **No persistent config file.** All settings reset to defaults
  (unity gain, suppression ON, sensitivity 0.5) on restart; the CLI
  command set would just need a `save`/`load` pair around a small
  serialization of `Mixer`'s atomics.
