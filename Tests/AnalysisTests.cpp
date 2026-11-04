#include "Analysis/VocalAnalyzer.h"
#include "TestSignals.h"

#include <algorithm>
#include <cmath>

using namespace moontune;
using namespace moontune::testsignals;

namespace
{
    constexpr double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };

    struct PitchStats { float medianErrCents = 0, maxErrCents = 0, voicedFraction = 0; int octaveErrors = 0; };

    PitchStats statsOver (const AnalysisResult& r, double t0, double t1, double expectedMidi)
    {
        std::vector<float> errs;
        int total = 0, octave = 0;
        for (int i = r.frameIndexAt (t0); i <= r.frameIndexAt (t1) && i < (int) r.frames.size(); ++i)
        {
            ++total;
            const auto& f = r.frames[(size_t) i];
            if (f.cls != FrameClass::Voiced) continue;
            const float e = std::abs (f.pitch - (float) expectedMidi) * 100.0f;
            if (e > 600.0f) ++octave;
            errs.push_back (e);
        }
        PitchStats s;
        if (! errs.empty())
        {
            std::sort (errs.begin(), errs.end());
            s.medianErrCents = errs[errs.size() / 2];
            s.maxErrCents = errs.back();
        }
        s.voicedFraction = total > 0 ? (float) errs.size() / (float) total : 0.0f;
        s.octaveErrors = octave;
        return s;
    }

    juce::String rateName (double sr) { return juce::String (sr / 1000.0, 1) + " kHz"; }
}

class AnalysisTests final : public juce::UnitTest
{
public:
    AnalysisTests() : juce::UnitTest ("Vocal analysis (F0 + notes)", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Steady vowels E2..C6: accurate pitch, no octave errors, all sample rates");
        {
            for (double sr : sampleRates)
                for (double midi : { 40.0, 45.0, 52.0, 57.0, 64.0, 69.0, 76.0, 81.0, 84.0 })   // E2 .. C6
                {
                    auto audio = renderMelody ({ { 0.1, 0.9, midi } }, sr, 1.0);
                    const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
                    const auto s = statsOver (r, 0.2, 0.8, midi);
                    const auto what = rateName (sr) + " midi " + juce::String (midi);
                    expectLessThan (s.medianErrCents, 3.0f, what);
                    expectLessThan (s.maxErrCents, 25.0f, what);
                    expectEquals (s.octaveErrors, 0, what);
                    expectGreaterThan (s.voicedFraction, 0.98f, what);
                    expectEquals ((int) r.notes.size(), 1, what);
                }
        }

        beginTest ("Weak fundamental (strong 2nd harmonic) stays in the right octave");
        {
            for (double sr : { 44100.0, 96000.0 })
                for (double midi : { 43.0, 50.0, 57.0 })
                {
                    auto audio = renderMelody ({ { 0.1, 0.9, midi } }, sr, 1.0, 0.04, 0.08f);
                    const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
                    const auto s = statsOver (r, 0.2, 0.8, midi);
                    expectEquals (s.octaveErrors, 0, rateName (sr) + " midi " + juce::String (midi));
                    expectLessThan (s.medianErrCents, 5.0f);
                }
        }

        beginTest ("Noise only: no notes, frames unvoiced");
        {
            const double sr = 48000.0;
            std::vector<float> audio ((size_t) sr, 0.0f);
            addNoise (audio, sr, 0.0, 1.0, 0.3f, true, 3);
            addNoise (audio, sr, 0.0, 1.0, 0.3f, false, 4);
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
            expectEquals ((int) r.notes.size(), 0);
            int voiced = 0;
            for (const auto& f : r.frames) if (f.cls == FrameClass::Voiced) ++voiced;
            expectLessThan ((float) voiced / (float) r.frames.size(), 0.05f);
            expect (! r.noise.empty());
        }

        beginTest ("Silence: everything below gate is Silence, no notes");
        {
            std::vector<float> audio (48000, 0.0f);
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), 48000.0);
            expectEquals ((int) r.notes.size(), 0);
            for (const auto& f : r.frames)
                if (f.cls != FrameClass::Silence) { expect (false, "non-silent frame"); break; }
        }

        beginTest ("Legato melody with glides and vibrato: notes, boundaries, vibrato flag");
        {
            const std::vector<MelodyNote> melody {
                { 0.20, 0.60, 57 }, { 0.60, 1.00, 59 }, { 1.00, 1.40, 60 }, { 1.40, 1.80, 62 },
                { 1.80, 2.90, 64, 5.5, 80.0 },   // long note with +-40 cent vibrato
                { 2.90, 3.30, 62 }, { 3.30, 3.70, 60 }, { 3.70, 4.30, 57 } };

            for (double sr : sampleRates)
            {
                auto audio = renderMelody (melody, sr, 4.6);
                const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
                const auto what = rateName (sr);

                expectEquals ((int) r.notes.size(), (int) melody.size(), what);
                if (r.notes.size() != melody.size())
                {
                    for (auto& n : r.notes)
                        logMessage ("  note " + juce::String (n.startSeconds, 3) + "-" + juce::String (n.endSeconds, 3)
                                    + " pitch " + juce::String (n.pitch, 2));
                    continue;
                }

                for (size_t i = 0; i < melody.size(); ++i)
                {
                    const auto& n = r.notes[i];
                    expectLessThan (std::abs (n.pitch - (float) melody[i].pitch) * 100.0f, 15.0f,
                                    what + " pitch of note " + juce::String ((int) i));
                    if (i > 0)
                        expectLessThan (std::abs (n.startSeconds - melody[i].start), 0.04,
                                        what + " boundary " + juce::String ((int) i));
                    expect (n.hasVibrato == (i == 4), what + " vibrato flag note " + juce::String ((int) i));
                    expect (n.reliable, what + " reliable " + juce::String ((int) i));
                }

                const auto& vib = r.notes[4];
                expectWithinAbsoluteError (vib.vibratoRateHz, 5.5f, 1.0f, what);
                expectWithinAbsoluteError (vib.vibratoExtentCents, 80.0f, 25.0f, what);
                expectWithinAbsoluteError (r.notes.front().startSeconds, 0.20, 0.03, what);
                expectWithinAbsoluteError (r.notes.back().endSeconds, 4.30, 0.04, what);
            }
        }

        beginTest ("Same pitch sung twice with a consonant in between -> two notes; consonant marked as noise");
        {
            const double sr = 48000.0;
            auto audio = renderMelody ({ { 0.2, 0.7, 62 }, { 0.78, 1.3, 62 } }, sr, 1.5);
            addNoise (audio, sr, 0.70, 0.78, 0.15f, true, 9);   // "s"
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
            expectEquals ((int) r.notes.size(), 2);
            bool noiseFound = false;
            for (const auto& nz : r.noise)
                if (nz.startSeconds < 0.76 && nz.endSeconds > 0.72) noiseFound = true;
            expect (noiseFound);
        }

        beginTest ("Re-articulation by level dip (no gap) splits the note");
        {
            const double sr = 44100.0;
            std::vector<float> audio ((size_t) (1.4 * sr), 0.0f);
            VoiceSynth synth { sr };
            synth.render (audio, 0, (int) audio.size(), [] (double) { return 60.0; },
                          [] (double t)
                          {
                              if (t < 0.1 || t > 1.3) return 0.0;
                              const double dip = 1.0 - 0.85 * std::exp (-0.5 * std::pow ((t - 0.7) / 0.02, 2.0));
                              return envelope (t, 0.1, 1.3) * dip;
                          });
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
            expectEquals ((int) r.notes.size(), 2);
            if (r.notes.size() == 2)
                expectWithinAbsoluteError (r.notes[1].startSeconds, 0.7, 0.03);
        }

        beginTest ("Breath between phrases is noise, not a note");
        {
            const double sr = 48000.0;
            auto audio = renderMelody ({ { 0.1, 0.8, 64 }, { 1.4, 2.0, 67 } }, sr, 2.2);
            addNoise (audio, sr, 0.95, 1.30, 0.05f, false, 11);
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
            expectEquals ((int) r.notes.size(), 2);
            bool breath = false;
            for (const auto& nz : r.noise)
                if (nz.startSeconds < 1.2 && nz.endSeconds > 1.0) breath = true;
            expect (breath);
        }

        beginTest ("Breath, sibilant (s / ts) and plosive (k) are told apart");
        {
            const double sr = 48000.0;
            auto audio = renderMelody ({ { 0.1, 0.6, 64 }, { 1.0, 1.5, 67 }, { 1.9, 2.4, 62 } }, sr, 2.6);
            addNoise (audio, sr, 0.65, 0.95, 0.05f, false, 11);   // breath: dark, long
            addNoise (audio, sr, 1.62, 1.82, 0.08f, true, 12);    // sibilant: bright
            {
                // plosive: quiet closure, then a 25 ms mid-frequency burst right before the next note
                juce::Random rng (13);
                const auto a = (size_t) (1.86 * sr), b = (size_t) (1.885 * sr);
                float lp = 0.0f;
                for (size_t i = a; i < b; ++i) { lp += 0.2f * ((rng.nextFloat() * 2.0f - 1.0f) - lp); audio[i] += 0.4f * lp; }
            }
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
            auto kindAt = [&r] (double t)
            {
                for (const auto& nz : r.noise) if (t >= nz.startSeconds - 0.01 && t <= nz.endSeconds + 0.01) return nz.kind;
                return 0;
            };
            expectEquals (kindAt (0.8), (int) NoiseRegion::breath);
            expectEquals (kindAt (1.72), (int) NoiseRegion::sibilant);
            expectEquals (kindAt (1.872), (int) NoiseRegion::plosive);
            expectEquals ((int) r.notes.size(), 3);
        }

        beginTest ("Centre pitch helper matches note pitch");
        {
            auto audio = renderMelody ({ { 0.1, 0.9, 65.3 } }, 48000.0, 1.0);
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), 48000.0);
            expectWithinAbsoluteError (VocalAnalyzer::centrePitch (r, 0.1, 0.9, 0.0f), 65.3f, 0.05f);
            expectEquals (VocalAnalyzer::centrePitch (r, 5.0, 6.0, -1.0f), -1.0f);   // outside: fallback
        }

        beginTest ("Performance: 3 minutes of vocal at 48 kHz");
        {
            const double sr = 48000.0;
            std::vector<MelodyNote> mel;
            juce::Random rng (5);
            for (double t = 0.5; t < 179.0; t += 0.45)
                mel.push_back ({ t, t + 0.4, 55.0 + rng.nextInt (14) });
            std::vector<float> audio ((size_t) (180.0 * sr), 0.0f);
            VoiceSynth synth { sr };
            for (const auto& n : mel)
            {
                const auto a = (juce::int64) (n.start * sr), b = (juce::int64) (n.end * sr);
                synth.render (audio, a, (int) (b - a), [&n] (double) { return n.pitch; },
                              [&n] (double t) { return envelope (t, n.start, n.end); });
            }
            const auto r = VocalAnalyzer::analyseBuffer (audio.data(), (int) audio.size(), sr);
            logMessage ("  analysis of 180 s took " + juce::String (r.analysisSeconds, 2) + " s, notes: "
                        + juce::String ((int) r.notes.size()) + " / " + juce::String ((int) mel.size()));
            expectLessThan (r.analysisSeconds, 30.0);
            expectGreaterThan ((int) r.notes.size(), (int) (mel.size() * 0.95));
            expectLessThan ((int) r.notes.size(), (int) (mel.size() * 1.05) + 1);
        }
    }
};

static AnalysisTests analysisTests;
