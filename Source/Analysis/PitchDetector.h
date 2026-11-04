#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include "AnalysisResult.h"

namespace moontune
{

// Monophonic F0 tracker for vocals.
//
// 1. The input is low-pass filtered and decimated to ~22-24 kHz, so analysis
//    behaves the same at 44.1 / 48 / 88.2 / 96 kHz.
// 2. Per frame, the YIN cumulative-mean-normalised difference function is computed
//    (via FFT). Several period candidates (local minima) are kept per frame instead
//    of committing to one, because a voice's harmonics produce dips at half / double
//    the true period.
// 3. A Viterbi search over all frames picks the candidate path with the best balance
//    of periodicity and smooth pitch movement, including an unvoiced state. This is
//    what suppresses octave errors and single-frame glitches.
//
// Own implementation of the published YIN method (de Cheveigné & Kawahara, 2002)
// plus standard HMM smoothing; no third-party code.
struct PitchDetectorSettings
{
    float minF0 = 55.0f;          // A1 - below the lowest bass notes in practice
    float maxF0 = 1400.0f;        // ~F6 - above soprano high notes
    double hopSeconds = 0.005;
    double windowSeconds = 0.025;
};

class PitchDetector
{
public:
    using Settings = PitchDetectorSettings;

    // Analyses `numSamples` of mono audio whose first sample sits at timeline
    // position `startSeconds`. Writes frames into `frames` at global indices
    // (frame i centred at i * hop). `frames` must already be large enough.
    static void analyse (const float* samples, int numSamples, double sampleRate, double startSeconds,
                         const Settings&, std::vector<AnalysisFrame>& frames, float silenceGateDb);

    // Exposed for tests.
    static std::vector<float> decimate (const float* x, int n, int factor);
    static int decimationFactor (double sampleRate) noexcept;
};

} // namespace moontune
