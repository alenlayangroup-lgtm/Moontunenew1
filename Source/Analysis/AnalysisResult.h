#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <limits>
#include <vector>

namespace moontune
{

enum class FrameClass : juce::uint8
{
    Silence = 0,   // below the level gate
    Unvoiced,      // audible but no reliable pitch: breath, sibilants, plosives, noise
    Voiced         // periodic: pitch is valid
};

// One analysis frame (every hopSeconds on the take's timeline).
struct AnalysisFrame
{
    float pitch = std::numeric_limits<float>::quiet_NaN();  // MIDI note (fractional), NaN if not voiced
    float confidence = 0.0f;                                // 0..1, periodicity of the chosen pitch
    float levelDb = -120.0f;                                // RMS level, dBFS
    FrameClass cls = FrameClass::Silence;
    bool interpolated = false;                              // pitch bridged over a short dropout
    float hfRatio = 0.0f;                                   // share of the energy above ~4 kHz (sibilants)
};

struct AnalysedNote
{
    double startSeconds = 0.0, endSeconds = 0.0;
    float pitch = 60.0f;              // perceived centre pitch (MIDI, fractional)
    float confidence = 0.0f;          // mean frame confidence
    bool reliable = true;             // false: user should check this note
    bool hasVibrato = false;
    float vibratoRateHz = 0.0f;
    float vibratoExtentCents = 0.0f;  // peak-to-peak
};

struct NoiseRegion
{
    enum Kind { breath = 1, sibilant = 2, plosive = 3 };   // same numbers as Note::Kind
    double startSeconds = 0.0, endSeconds = 0.0;   // unvoiced but audible (breath / consonant)
    int kind = breath;
};

struct AnalysisResult
{
    static constexpr int analyzerVersion = 3;   // bump when results change

    double hopSeconds = 0.005;
    std::vector<AnalysisFrame> frames;          // frame i is centred at i * hopSeconds
    std::vector<AnalysedNote> notes;
    std::vector<NoiseRegion> noise;
    float peakLevelDb = -120.0f;
    double analysisSeconds = 0.0;               // wall-clock time the analysis took

    int frameIndexAt (double seconds) const noexcept
    {
        return (int) std::floor (seconds / hopSeconds + 0.5);
    }

    const AnalysisFrame* frameAt (double seconds) const noexcept
    {
        const int i = frameIndexAt (seconds);
        return (i >= 0 && i < (int) frames.size()) ? &frames[(size_t) i] : nullptr;
    }
};

} // namespace moontune
