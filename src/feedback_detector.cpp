#include "feedback_detector.h"

#include <algorithm>
#include <cmath>

namespace audiomix {

namespace {
// sensitivity 0..1 -> peakiness ratio required to start counting.
// Lower ratio = easier to trigger = more sensitive.
double thresholdForSensitivity(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    constexpr double kLoosest = 14.0; // sensitivity ~0
    constexpr double kTightest = 3.5; // sensitivity ~1
    return kLoosest + (kTightest - kLoosest) * s;
}

// sensitivity 0..1 -> number of consecutive blocks a peak must persist.
int persistenceForSensitivity(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    constexpr double kSlowest = 10.0; // sensitivity ~0 (~53ms @256/48k... see note)
    constexpr double kFastest = 2.0;  // sensitivity ~1
    return static_cast<int>(std::round(kSlowest + (kFastest - kSlowest) * s));
}
} // namespace

FeedbackDetector::FeedbackDetector() = default;

FeedbackDetector::~FeedbackDetector() {
    if (plan_) fftw_destroy_plan(plan_);
    if (fftIn_) fftw_free(fftIn_);
    if (fftOut_) fftw_free(fftOut_);
}

void FeedbackDetector::configure(double sampleRateHz, size_t fftSize, size_t hopSize) {
    sampleRate_ = sampleRateHz;
    fftSize_ = fftSize;
    hopSize_ = hopSize;
    numBins_ = fftSize_ / 2 + 1;

    ring_.assign(fftSize_, 0.0f);
    ringWritePos_ = 0;
    samplesBuffered_ = 0;

    window_.resize(fftSize_);
    for (size_t i = 0; i < fftSize_; ++i) {
        // Hann window.
        window_[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) /
                                            static_cast<double>(fftSize_ - 1)));
    }

    if (plan_) fftw_destroy_plan(plan_);
    if (fftIn_) fftw_free(fftIn_);
    if (fftOut_) fftw_free(fftOut_);

    fftIn_ = static_cast<double*>(fftw_malloc(sizeof(double) * fftSize_));
    fftOut_ = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * numBins_));
    std::fill(fftIn_, fftIn_ + fftSize_, 0.0);

    // FFTW_MEASURE would give a faster plan but perturbs fftIn_ and takes
    // noticeably longer at startup; FFTW_ESTIMATE is fine for our sizes and
    // keeps startup instantaneous. Plan creation happens once, off the
    // audio thread, so this cost never hits the real-time path.
    plan_ = fftw_plan_dft_r2c_1d(static_cast<int>(fftSize_), fftIn_, fftOut_, FFTW_ESTIMATE);

    magnitude_.assign(numBins_, 0.0);
    ringCount_.assign(numBins_, 0);
    flagged_.assign(numBins_, false);
}

void FeedbackDetector::setSensitivity(float sensitivity01) {
    sensitivity_ = std::clamp(sensitivity01, 0.0f, 1.0f);
}

void FeedbackDetector::pushSamples(const float* block, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        ring_[ringWritePos_] = block[i];
        ringWritePos_ = (ringWritePos_ + 1) % fftSize_;
    }
    samplesBuffered_ = std::min(samplesBuffered_ + n, fftSize_);
}

void FeedbackDetector::computeSpectrum() {
    // Copy ring buffer out in chronological order, applying the Hann window,
    // directly into the FFTW input array.
    const size_t start = ringWritePos_; // oldest sample position
    for (size_t i = 0; i < fftSize_; ++i) {
        const size_t idx = (start + i) % fftSize_;
        fftIn_[i] = static_cast<double>(ring_[idx]) * window_[i];
    }

    fftw_execute(plan_);

    const double norm = 2.0 / static_cast<double>(fftSize_);
    for (size_t k = 0; k < numBins_; ++k) {
        const double re = fftOut_[k][0];
        const double im = fftOut_[k][1];
        magnitude_[k] = std::sqrt(re * re + im * im) * norm;
    }
}

std::vector<FeedbackEvent> FeedbackDetector::process(const float* block, size_t n) {
    std::vector<FeedbackEvent> events;

    pushSamples(block, n);
    if (samplesBuffered_ < fftSize_) {
        // Not enough history yet (startup only) — nothing to analyze.
        return events;
    }

    computeSpectrum();

    const double threshold = thresholdForSensitivity(sensitivity_);
    const int requiredPersistence = persistenceForSensitivity(sensitivity_);

    // Skip sub-audible/rumble (<80Hz) and near-Nyquist bins; feedback in
    // live sound almost always lives in the 200Hz-8kHz range where mic and
    // speaker/room gain is highest.
    const double binHz = sampleRate_ / static_cast<double>(fftSize_);
    const size_t loBin = std::max<size_t>(1, static_cast<size_t>(80.0 / binHz));
    const size_t hiBin = std::min(numBins_ - 1, static_cast<size_t>(8000.0 / binHz));

    for (size_t k = loBin; k <= hiBin; ++k) {
        // Estimate local noise floor from a neighborhood around k, excluding
        // bins immediately adjacent to k (so a wide, non-tonal loud passage
        // doesn't get mistaken for a narrow peak against itself).
        const size_t nStart = (k > static_cast<size_t>(kNeighborhoodBins))
                                   ? k - kNeighborhoodBins
                                   : 0;
        const size_t nEnd = std::min(numBins_ - 1, k + kNeighborhoodBins);

        double sum = 0.0;
        int count = 0;
        for (size_t j = nStart; j <= nEnd; ++j) {
            if (j >= (k >= static_cast<size_t>(kExclusionBins) ? k - kExclusionBins : 0) &&
                j <= k + kExclusionBins) {
                continue; // exclusion zone around the candidate bin itself
            }
            sum += magnitude_[j];
            ++count;
        }
        const double noiseFloor = (count > 0) ? (sum / count) : 1e-9;
        const double peakiness = magnitude_[k] / std::max(noiseFloor, 1e-9);

        // Also require some minimum absolute level so silence/noise floor
        // jitter never triggers (avoids false positives on a near-silent
        // channel where everything is "peaky" relative to near-zero noise).
        constexpr double kMinAbsMagnitude = 1e-4;

        if (peakiness >= threshold && magnitude_[k] >= kMinAbsMagnitude) {
            ringCount_[k] = std::min(ringCount_[k] + 1, requiredPersistence + 4);
        } else {
            ringCount_[k] = std::max(ringCount_[k] - 1, 0);
            if (ringCount_[k] == 0) {
                flagged_[k] = false;
            }
        }

        if (ringCount_[k] >= requiredPersistence && !flagged_[k]) {
            flagged_[k] = true;

            // Raw FFT bin resolution (binHz, ~47Hz at our default 1024/48kHz
            // settings) is far coarser than what a Q~10 notch needs to bite
            // hard on the actual ringing frequency: a notch centered even
            // 20-30Hz off from the true tone attenuates it by only a few dB
            // instead of collapsing it. Quadratic (parabolic) interpolation
            // across the peak bin and its two neighbors, in the log
            // (dB) domain, gives a much better sub-bin frequency estimate
            // from the same spectrum at negligible extra cost.
            double refinedBin = static_cast<double>(k);
            if (k > 0 && k < numBins_ - 1) {
                const double a = 20.0 * std::log10(std::max(magnitude_[k - 1], 1e-12));
                const double b = 20.0 * std::log10(std::max(magnitude_[k], 1e-12));
                const double c = 20.0 * std::log10(std::max(magnitude_[k + 1], 1e-12));
                const double denom = (a - 2.0 * b + c);
                if (std::fabs(denom) > 1e-9) {
                    const double p = 0.5 * (a - c) / denom;
                    if (p > -1.0 && p < 1.0) { // sanity bound
                        refinedBin = static_cast<double>(k) + p;
                    }
                }
            }

            const double freq = refinedBin * binHz;
            const float confidence = static_cast<float>(
                std::clamp((peakiness - threshold) / threshold, 0.0, 1.0));
            events.push_back(FeedbackEvent{freq, confidence});
        }
    }

    return events;
}

float FeedbackDetector::magnitudeAtDb(double freqHz) const {
    if (numBins_ == 0) return -120.0f;
    const double binHz = sampleRate_ / static_cast<double>(fftSize_);
    long k = std::lround(freqHz / binHz);
    k = std::clamp<long>(k, 0, static_cast<long>(numBins_) - 1);
    const double mag = magnitude_[static_cast<size_t>(k)];
    return static_cast<float>(20.0 * std::log10(std::max(mag, 1e-9)));
}

} // namespace audiomix
