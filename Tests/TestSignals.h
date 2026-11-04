#pragma once

// Synthetic vocal-like test signals. Not a real voice, but it has what matters for
// pitch analysis: a harmonic spectrum shaped by vowel formants, pitch glides,
// vibrato, level envelopes, and noise (sibilants, breaths).

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace moontune::testsignals
{

inline double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

// Vowel "a"-like spectral envelope: three formants.
inline double formantGain (double hz)
{
    auto peak = [] (double f, double centre, double bw, double amp)
    { return amp * std::exp (-0.5 * std::pow ((f - centre) / bw, 2.0)); };
    return 0.08 + peak (hz, 750.0, 130.0, 1.0) + peak (hz, 1200.0, 160.0, 0.6) + peak (hz, 2600.0, 250.0, 0.3);
}

struct VoiceSynth
{
    double sampleRate;
    std::vector<double> phases = std::vector<double> (80, 0.0);
    float firstHarmonicGain = 1.0f;   // < 1 simulates a weak fundamental

    // pitchAt(t) -> MIDI, levelAt(t) -> linear amplitude
    void render (std::vector<float>& out, juce::int64 startSample, int numSamples,
                 const std::function<double (double)>& pitchAt, const std::function<double (double)>& levelAt)
    {
        const double nyquistLimit = std::min (8000.0, sampleRate * 0.45);
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) (startSample + i) / sampleRate;
            const double f0 = midiToHz (pitchAt (t));
            const double level = levelAt (t);
            double s = 0.0;
            for (int k = 1; k <= (int) phases.size(); ++k)
            {
                const double fk = f0 * k;
                if (fk > nyquistLimit) break;
                auto& ph = phases[(size_t) k - 1];
                ph += juce::MathConstants<double>::twoPi * fk / sampleRate;
                if (ph > juce::MathConstants<double>::twoPi) ph -= juce::MathConstants<double>::twoPi;
                double amp = formantGain (fk) / std::pow ((double) k, 0.7);
                if (k == 1) amp *= firstHarmonicGain;
                s += amp * std::sin (ph);
            }
            out[(size_t) (startSample + i)] += (float) (s * level * 0.25);
        }
    }
};

inline void addNoise (std::vector<float>& out, double sampleRate, double t0, double t1, float level,
                      bool highPassed, juce::uint32 seed = 1)
{
    juce::Random rng ((juce::int64) seed);
    float prev = 0.0f, lp = 0.0f;
    const auto a = (juce::int64) (t0 * sampleRate), b = std::min ((juce::int64) out.size(), (juce::int64) (t1 * sampleRate));
    for (auto i = a; i < b; ++i)
    {
        const float w = rng.nextFloat() * 2.0f - 1.0f;
        float v;
        if (highPassed) { v = w - prev; prev = w; }        // sibilant-like
        else { lp += 0.1f * (w - lp); v = lp * 3.0f; }       // breath-like
        const double fade = std::min (1.0, std::min ((double) (i - a) / (0.01 * sampleRate), (double) (b - i) / (0.01 * sampleRate)));
        out[(size_t) i] += v * level * (float) fade;
    }
}

// Smooth envelope: attack/release ramps of `ramp` seconds inside [t0, t1].
inline double envelope (double t, double t0, double t1, double ramp = 0.02)
{
    if (t < t0 || t > t1) return 0.0;
    return std::min (1.0, std::min ((t - t0) / ramp, (t1 - t) / ramp));
}

struct MelodyNote { double start, end, pitch; double vibratoHz = 0.0, vibratoCents = 0.0; };

// Legato melody: pitch glides between consecutive notes over `glide` seconds.
inline std::vector<float> renderMelody (const std::vector<MelodyNote>& notes, double sampleRate, double totalSeconds,
                                        double glide = 0.04, float firstHarmonicGain = 1.0f)
{
    std::vector<float> out ((size_t) (totalSeconds * sampleRate), 0.0f);
    VoiceSynth synth { sampleRate };
    synth.firstHarmonicGain = firstHarmonicGain;

    auto pitchAt = [&] (double t)
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            if (t < n.start || t >= n.end) continue;
            double p = n.pitch;
            if (n.vibratoHz > 0.0)
            {
                const double depth = std::min (1.0, (t - n.start) / 0.25);   // vibrato fades in
                p += depth * (n.vibratoCents / 200.0) * std::sin (juce::MathConstants<double>::twoPi * n.vibratoHz * (t - n.start));
            }
            if (i + 1 < notes.size() && notes[i + 1].start - n.end < 1.0e-6 && t > n.end - glide * 0.5)
            {
                const double x = (t - (n.end - glide * 0.5)) / glide;   // 0..0.5
                p += (notes[i + 1].pitch - n.pitch) * 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * x));
            }
            if (i > 0 && n.start - notes[i - 1].end < 1.0e-6 && t < n.start + glide * 0.5)
            {
                const double x = 0.5 + (t - n.start) / glide;           // 0.5..1
                p = notes[i - 1].pitch + (n.pitch - notes[i - 1].pitch) * 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * x));
            }
            return p;
        }
        return notes.empty() ? 60.0 : notes.front().pitch;
    };

    // Phrase envelope: continuous across touching notes.
    auto levelAt = [&] (double t)
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (t < notes[i].start || t >= notes[i].end) continue;
            size_t first = i, last = i;
            while (first > 0 && notes[first].start - notes[first - 1].end < 1.0e-6) --first;
            while (last + 1 < notes.size() && notes[last + 1].start - notes[last].end < 1.0e-6) ++last;
            return envelope (t, notes[first].start, notes[last].end);
        }
        return 0.0;
    };

    synth.render (out, 0, (int) out.size(), pitchAt, levelAt);
    return out;
}

} // namespace moontune::testsignals
