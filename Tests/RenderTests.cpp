#include "DSP/VocalRenderer.h"
#include "Model/NoteModel.h"
#include "DSP/Resampler.h"
#include "DSP/TimeMap.h"
#include "Analysis/VocalAnalyzer.h"
#include "TestSignals.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <limits>

using namespace moontune;
using namespace moontune::testsignals;

namespace
{
    struct Setup
    {
        double sr;
        std::shared_ptr<TakeAudio> take;
        AnalysisResult analysis;
        std::vector<Note> notes;
        std::vector<float> audio;
    };

    Setup makeSetup (const std::vector<MelodyNote>& melody, double sr, double seconds,
                     const std::function<void (std::vector<float>&)>& extra = {})
    {
        Setup s;
        s.sr = sr;
        s.audio = renderMelody (melody, sr, seconds);
        if (extra) extra (s.audio);
        s.take = std::make_shared<TakeAudio> (sr);
        s.take->write (0, s.audio.data(), (int) s.audio.size());
        s.analysis = VocalAnalyzer::analyse (*s.take);
        juce::int64 id = 1;
        for (const auto& a : s.analysis.notes)
        {
            Note n;
            n.id = id++;
            n.startSeconds = a.startSeconds;
            n.endSeconds = a.endSeconds;
            n.detectedPitch = a.pitch;
            s.notes.push_back (n);
        }
        return s;
    }

    std::vector<float> renderWith (const Setup& s, const std::vector<Note>& notes, RenderOptions o = {})
    {
        auto r = VocalRenderer::render (*s.take, s.analysis, notes, o);
        if (r == nullptr) return s.audio;
        return r->samples;
    }

    double rms (const std::vector<float>& x, double t0, double t1, double sr)
    {
        const auto a = (size_t) (t0 * sr), b = std::min (x.size(), (size_t) (t1 * sr));
        double acc = 0.0;
        for (size_t i = a; i < b; ++i) acc += (double) x[i] * x[i];
        return std::sqrt (acc / std::max<size_t> (1, b - a));
    }

    double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

    float medianPitch (const AnalysisResult& r, double t0, double t1)
    {
        std::vector<float> v;
        for (int i = r.frameIndexAt (t0); i <= r.frameIndexAt (t1) && i < (int) r.frames.size(); ++i)
            if (r.frames[(size_t) i].cls == FrameClass::Voiced) v.push_back (r.frames[(size_t) i].pitch);
        if (v.empty()) return 0.0f;
        std::sort (v.begin(), v.end());
        return v[v.size() / 2];
    }

    // Spectral-envelope centroid between 300 and 2000 Hz (formant region) over [t0, t1].
    double formantCentroid (const std::vector<float>& x, double t0, double t1, double sr)
    {
        const int order = 13, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<double> mag ((size_t) N / 2, 0.0);
        int frames = 0;
        for (auto start = (size_t) (t0 * sr); start + (size_t) N < (size_t) (t1 * sr) && start + (size_t) N < x.size(); start += (size_t) N / 4)
        {
            std::vector<float> buf ((size_t) N * 2, 0.0f);
            for (int i = 0; i < N; ++i)
                buf[(size_t) i] = x[start + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (N - 1)));
            fft.performFrequencyOnlyForwardTransform (buf.data());
            for (int k = 0; k < N / 2; ++k) mag[(size_t) k] += buf[(size_t) k];
            ++frames;
        }
        // Envelope: max-hold over +-150 Hz (connects harmonic peaks), then centroid.
        const double binHz = sr / N;
        const int w = (int) (150.0 / binHz);
        double num = 0.0, den = 0.0;
        for (int k = (int) (300.0 / binHz); k < (int) (2000.0 / binHz); ++k)
        {
            double env = 0.0;
            for (int j = std::max (0, k - w); j <= std::min (N / 2 - 1, k + w); ++j) env = std::max (env, mag[(size_t) j]);
            num += env * env * k * binHz;
            den += env * env;
        }
        return den > 0.0 ? num / den : 0.0;
    }

    float maxStep (const std::vector<float>& x, double t0, double t1, double sr)
    {
        float m = 0.0f;
        for (auto i = (size_t) (t0 * sr) + 1; i < std::min (x.size(), (size_t) (t1 * sr)); ++i)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }

    float bandEnergyDb (const std::vector<float>& x, double t0, double t1, double sr, double f)
    {
        // Goertzel-like single-frequency power via correlation.
        const auto a = (size_t) (t0 * sr), b = std::min (x.size(), (size_t) (t1 * sr));
        double re = 0.0, im = 0.0;
        for (size_t i = a; i < b; ++i)
        {
            const double ph = 2.0 * juce::MathConstants<double>::pi * f * (double) i / sr;
            re += x[i] * std::cos (ph); im += x[i] * std::sin (ph);
        }
        return (float) db (std::sqrt (re * re + im * im) / std::max<size_t> (1, b - a));
    }
}

class RenderTests final : public juce::UnitTest
{
public:
    RenderTests() : juce::UnitTest ("Vocal renderer (pitch / time / formant / EQ)", "MoonTune") {}

    void runTest() override
    {
        const std::vector<MelodyNote> melody { { 0.3, 0.9, 57 }, { 0.9, 1.6, 60, 5.5, 70 }, { 1.6, 2.2, 62 } };

        beginTest ("No edits: nothing rendered, audio untouched");
        {
            auto s = makeSetup (melody, 48000.0, 2.6);
            expect (VocalRenderer::render (*s.take, s.analysis, s.notes, {}) == nullptr);
        }

        beginTest ("Pitch shift: accurate, level kept, unedited notes bit-exact, no clicks");
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (float cents : { 100.0f, -200.0f, 300.0f, 700.0f })
            {
                auto s = makeSetup (melody, sr, 2.6);
                expectEquals ((int) s.notes.size(), 3);
                if (s.notes.size() != 3) continue;
                auto edited = s.notes;
                edited[1].pitchShiftCents = cents;
                const auto y = renderWith (s, edited);
                if (std::getenv ("MOONTUNE_DUMP") != nullptr)
                {
                    juce::File f ("/tmp/mt_" + juce::String ((int) sr) + "_" + juce::String ((int) cents) + ".raw");
                    f.replaceWithData (y.data(), y.size() * sizeof (float));
                    RenderOptions tv; tv.engine = Engine::trueVoice;
                    const auto y2 = renderWith (s, edited, tv);
                    juce::File ("/tmp/mt_tv_" + juce::String ((int) sr) + "_" + juce::String ((int) cents) + ".raw").replaceWithData (y2.data(), y2.size() * sizeof (float));
                    juce::File ("/tmp/mt_src_" + juce::String ((int) sr) + ".raw").replaceWithData (s.audio.data(), s.audio.size() * sizeof (float));
                }

                const auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                const float before = medianPitch (s.analysis, 1.0, 1.5), after = medianPitch (r, 1.0, 1.5);
                const auto what = juce::String (sr) + " Hz, " + juce::String (cents) + " c";
                expectWithinAbsoluteError ((after - before) * 100.0f, cents, 8.0f, what);
                // Loudness is preserved (no level drop or boost from re-pitching).
                expectWithinAbsoluteError (db (rms (y, 1.0, 1.5, sr) / rms (s.audio, 1.0, 1.5, sr)), 0.0, 0.5, what);

                // Before the edit the output is the recording, sample for sample. The legato note after
                // it keeps its pitch and level while the voice's phase settles back (<= 0.4 %), and is
                // sample-exact again once it has.
                float diff = 0.0f;
                for (auto i = (size_t) (0.3 * sr); i < (size_t) (0.75 * sr); ++i) diff = std::max (diff, std::abs (y[i] - s.audio[i]));
                for (auto i = (size_t) (2.12 * sr); i < (size_t) (2.6 * sr); ++i) diff = std::max (diff, std::abs (y[i] - s.audio[i]));
                expectEquals (diff, 0.0f, what);
                expectWithinAbsoluteError ((medianPitch (r, 1.75, 2.1) - medianPitch (s.analysis, 1.75, 2.1)) * 100.0f, 0.0f, 8.0f, what);   // phase settling <= 0.4 % (7 c), only until it has drained
                expectWithinAbsoluteError (db (rms (y, 1.75, 2.1, sr) / rms (s.audio, 1.75, 2.1, sr)), 0.0, 0.3, what);

                // Transitions are glides, not jumps: no sample step larger than in the source (+ margin).
                expectLessThan (maxStep (y, 0.7, 1.9, sr), maxStep (s.audio, 0.7, 1.9, sr) * 1.6f + 0.002f, what);
            }

        beginTest ("Sample-rate conversion (ARA events at another rate): transparent, no delay");
        {
            SincResampler rs;
            for (auto [inRate, outRate] : { std::pair { 48000.0, 44100.0 }, std::pair { 44100.0, 48000.0 }, std::pair { 96000.0, 44100.0 } })
            {
                const double ratio = inRate / outRate;
                const int n = (int) inRate;
                std::vector<float> in ((size_t) n);
                for (int i = 0; i < n; ++i)
                    in[(size_t) i] = (float) (0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / inRate)
                                            + 0.2 * std::sin (2.0 * juce::MathConstants<double>::pi * 15000.0 * i / inRate));
                const int m = (int) (n / ratio) - 200;
                std::vector<float> out ((size_t) m);
                rs.process (in.data(), n, 0.0, ratio, out.data(), m);
                double err = 0.0, ref = 0.0;
                for (int i = 200; i < m - 200; ++i)
                {
                    const double t = i / outRate;
                    const double want = 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t)
                                      + 0.2 * std::sin (2.0 * juce::MathConstants<double>::pi * 15000.0 * t);
                    err += (out[(size_t) i] - want) * (out[(size_t) i] - want);
                    ref += want * want;
                }
                expectLessThan (10.0 * std::log10 (err / ref), -70.0, juce::String (inRate) + " -> " + juce::String (outRate));
            }
        }

        beginTest ("Hybrid engine: accurate pitch, level kept, untouched audio bit-exact");
        {
            auto s = makeSetup (melody, 48000.0, 2.6);
            if (s.notes.size() == 3)
            {
                auto edited = s.notes;
                edited[1].pitchShiftCents = 300.0f;
                RenderOptions o; o.engine = Engine::hybrid;
                const auto y = renderWith (s, edited, o);
                const auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), 48000.0);
                expectWithinAbsoluteError ((medianPitch (r, 1.0, 1.5) - medianPitch (s.analysis, 1.0, 1.5)) * 100.0f, 300.0f, 8.0f);
                expectWithinAbsoluteError (db (rms (y, 1.0, 1.5, 48000.0) / rms (s.audio, 1.0, 1.5, 48000.0)), 0.0, 0.5);
                float diff = 0.0f;
                for (auto i = (size_t) (0.3 * 48000.0); i < (size_t) (0.75 * 48000.0); ++i) diff = std::max (diff, std::abs (y[i] - s.audio[i]));
                expectEquals (diff, 0.0f);
            }
        }

        beginTest ("Tilt slopes the note; transition time sets the glide length");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.2, 1.4, 62 } }, sr, 1.6);
            if (s.notes.size() == 1)
            {
                auto t = s.notes;
                t[0].pitchTilt = 200.0f;   // -1 st at the start .. +1 st at the end
                const auto y = renderWith (s, t);
                const auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                const float early = medianPitch (r, 0.35, 0.45), late = medianPitch (r, 1.15, 1.25);
                expectWithinAbsoluteError ((late - early) * 100.0f, 200.0f * 0.8f / 1.2f * 1.0f, 30.0f);
            }

            auto s2 = makeSetup ({ { 0.2, 0.9, 60 }, { 0.9, 1.6, 60 } }, sr, 1.8);
            if (s2.notes.size() == 2)
            {
                auto fast = s2.notes, slow = s2.notes;
                fast[1].pitchShiftCents = slow[1].pitchShiftCents = 400.0f;
                slow[1].transitionMs = 300.0f;
                const auto yf = renderWith (s2, fast), ys = renderWith (s2, slow);
                const auto rf = VocalAnalyzer::analyseBuffer (yf.data(), (int) yf.size(), sr);
                const auto rsl = VocalAnalyzer::analyseBuffer (ys.data(), (int) ys.size(), sr);
                const double b = s2.notes[1].startSeconds;
                // 80 ms after the joint the fast glide has arrived, the slow one is still on its way.
                expectGreaterThan (medianPitch (rf, b + 0.07, b + 0.09), 63.5f);
                expectLessThan (medianPitch (rsl, b + 0.07, b + 0.09), 63.0f);
                expectWithinAbsoluteError (medianPitch (rsl, b + 0.25, b + 0.4), 64.0f, 0.15f);
            }
        }

        beginTest ("Plosive bursts (b / p / d) keep their attack in every engine");
        {
            const double sr = 48000.0;
            // Quiet closure, a 6 ms burst, then the note: like "ba" / "pa".
            auto s = makeSetup ({ { 0.1, 0.6, 64 }, { 0.75, 1.5, 62 } }, sr, 1.7, [sr] (std::vector<float>& a)
            {
                juce::Random rng (31);
                const auto b0 = (size_t) (0.745 * sr), b1 = (size_t) (0.751 * sr);
                for (auto i = (size_t) (0.62 * sr); i < b0; ++i) a[i] *= 0.0f;            // closure
                for (auto i = b0; i < b1; ++i) a[i] += 0.6f * (rng.nextFloat() * 2.0f - 1.0f);   // burst
            });
            if (s.notes.size() >= 2)
                for (int engine : { Engine::harmonyClean, Engine::trueVoice, Engine::hybrid })
                {
                    auto edited = s.notes;
                    for (auto& n : edited) if (n.startSeconds > 0.7) n.pitchShiftCents = 400.0f;
                    RenderOptions o; o.engine = engine;
                    const auto y = renderWith (s, edited, o);
                    // The burst itself is the original, sample for sample.
                    float d = 0.0f, peak = 0.0f;
                    for (auto i = (size_t) (0.745 * sr); i < (size_t) (0.751 * sr); ++i)
                    {
                        d = std::max (d, std::abs (y[i] - s.audio[i]));
                        peak = std::max (peak, std::abs (s.audio[i]));
                    }
                    expectLessThan (d, peak * 0.02f, "engine " + juce::String (engine));
                    // ... and the note after it is still shifted.
                    const auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                    expectWithinAbsoluteError (medianPitch (r, 0.95, 1.35), 66.0f, 0.1f);
                }
        }

        beginTest ("Breaths / consonants: silenced or louder on their own, notes untouched");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.1, 0.6, 64 }, { 1.0, 1.5, 67 } }, sr, 1.7,
                                [sr] (std::vector<float>& a) { addNoise (a, sr, 0.65, 0.95, 0.05f, false, 21); });
            Note breath;
            breath.id = 100; breath.kind = Note::breath; breath.startSeconds = 0.68; breath.endSeconds = 0.92;
            breath.detectedPitch = 64.0f; breath.gainDb = Note::silentDb;
            auto notes = s.notes;
            notes.push_back (breath);
            const auto y = renderWith (s, notes);
            expectLessThan (rms (y, 0.72, 0.88, sr), rms (s.audio, 0.72, 0.88, sr) * 0.01f);
            float diff = 0.0f;
            for (auto i = (size_t) (0.2 * sr); i < (size_t) (0.55 * sr); ++i) diff = std::max (diff, std::abs (y[i] - s.audio[i]));
            expectEquals (diff, 0.0f);   // the note before it is untouched
            notes.back().gainDb = 6.0f;
            const auto y2 = renderWith (s, notes);
            expectWithinAbsoluteError (db (rms (y2, 0.72, 0.88, sr) / rms (s.audio, 0.72, 0.88, sr)), 6.0, 0.3);
            notes.back().pitchShiftCents = 500.0f;   // pitch is never applied to a breath
            const auto y3 = renderWith (s, notes);
            float d3 = 0.0f;
            for (auto i = (size_t) (0.72 * sr); i < (size_t) (0.88 * sr); ++i) d3 = std::max (d3, std::abs (y3[i] - y2[i]));
            expectLessThan (d3, 1.0e-4f);
        }

        beginTest ("Formants preserved on pitch shift; formant control moves them");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.2, 1.8, 45 } }, sr, 2.0);   // A2, male range
            expectEquals ((int) s.notes.size(), 1);
            if (s.notes.size() == 1)
            {
                const double base = formantCentroid (s.audio, 0.5, 1.6, sr);

                auto shifted = s.notes;
                shifted[0].pitchShiftCents = 400.0f;
                const double kept = formantCentroid (renderWith (s, shifted), 0.5, 1.6, sr);
                expectWithinAbsoluteError (kept / base, 1.0, 0.06);

                RenderOptions noPreserve;
                noPreserve.preserveFormants = false;
                const double moved = formantCentroid (renderWith (s, shifted, noPreserve), 0.5, 1.6, sr);
                expectWithinAbsoluteError (moved / base, std::pow (2.0, 4.0 / 12.0), 0.08);

                auto formantOnly = s.notes;
                formantOnly[0].formantCents = 300.0f;
                const auto y = renderWith (s, formantOnly);
                expectWithinAbsoluteError (formantCentroid (y, 0.5, 1.6, sr) / base, std::pow (2.0, 3.0 / 12.0), 0.08);
                const auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                expectWithinAbsoluteError (medianPitch (r, 0.5, 1.6), 45.0f, 0.1f);   // pitch unchanged
            }
        }

        beginTest ("Vibrato kept when shifting; Modulation 0 straightens it; Drift edits");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.2, 1.8, 62, 5.5, 90 } }, sr, 2.0);
            expectEquals ((int) s.notes.size(), 1);
            if (s.notes.size() == 1)
            {
                auto shifted = s.notes;
                shifted[0].pitchShiftCents = 200.0f;
                auto y = renderWith (s, shifted);
                auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                expectEquals ((int) r.notes.size(), 1);
                if (r.notes.size() == 1)
                {
                    expect (r.notes[0].hasVibrato);
                    expectWithinAbsoluteError (r.notes[0].vibratoExtentCents, s.analysis.notes[0].vibratoExtentCents, 20.0f);
                    expectWithinAbsoluteError (r.notes[0].pitch, 64.0f, 0.1f);
                }

                auto flat = s.notes;
                flat[0].pitchModulation = 0.0f;
                y = renderWith (s, flat);
                r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                if (r.notes.size() == 1)
                {
                    expect (! r.notes[0].hasVibrato);
                    expectWithinAbsoluteError (r.notes[0].pitch, 62.0f, 0.1f);
                }

                auto deeper = s.notes;
                deeper[0].pitchModulation = 1.5f;
                y = renderWith (s, deeper);
                r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                if (r.notes.size() == 1)
                    expectGreaterThan (r.notes[0].vibratoExtentCents, s.analysis.notes[0].vibratoExtentCents * 1.25f);
            }
        }

        beginTest ("Pencil: the voice follows the drawn pitch exactly; undrawn parts untouched");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.2, 1.8, 62, 5.5, 90 } }, sr, 2.0);   // vibrato note
            expectEquals ((int) s.notes.size(), 1);
            if (s.notes.size() == 1)
            {
                auto drawn = s.notes;
                auto& n = drawn[0];
                const auto frames = (size_t) std::ceil (n.duration() / Note::drawHop) + 1;
                n.drawnCents.assign (frames, std::numeric_limits<float>::quiet_NaN());
                // 0.6 .. 1.2 s: a straight line from 61.0 up to 63.0 (vibrato replaced by the line).
                for (size_t i = 0; i < frames; ++i)
                {
                    const double t = n.startSeconds + (double) i * Note::drawHop;
                    if (t >= 0.6 && t <= 1.2)
                        n.drawnCents[i] = 100.0f * ((61.0f + 2.0f * (float) ((t - 0.6) / 0.6)) - n.detectedPitch);
                }
                expect (n.hasDrawing());
                expect (VocalRenderer::needsRender (drawn, {}));
                auto y = renderWith (s, drawn);
                auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                float worst = 0.0f;
                for (double t = 0.65; t <= 1.15; t += 0.01)
                {
                    const auto* f = r.frameAt (t);
                    if (f == nullptr || f->cls != FrameClass::Voiced) continue;
                    const float want = 61.0f + 2.0f * (float) ((t - 0.6) / 0.6);
                    worst = std::max (worst, std::abs (f->pitch - want));
                }
                logMessage ("  pencil: worst deviation from the drawn line = " + juce::String (worst * 100.0f, 1) + " cents");
                expectLessThan (worst, 0.05f);
                // Outside the drawing the vibrato stays where it was.
                for (double t : { 0.35, 0.45, 1.45, 1.6 })
                {
                    const auto* a = s.analysis.frameAt (t);
                    const auto* b = r.frameAt (t);
                    if (a != nullptr && b != nullptr && a->cls == FrameClass::Voiced && b->cls == FrameClass::Voiced)
                        expectWithinAbsoluteError (b->pitch, a->pitch, 0.1f, "undrawn at " + juce::String (t));
                }
                expectLessThan (maxStep (y, 0.3, 1.7, sr), 0.2f, "clicks");   // no clicks at the edges of the drawing
                // The displayed curve shows the drawing.
                const auto curve = VocalRenderer::notePitchCurve (n, s.analysis, {});
                const int fA = s.analysis.frameIndexAt (n.startSeconds), fm = s.analysis.frameIndexAt (0.9);
                if (fm - fA >= 0 && fm - fA < (int) curve.size())
                    expectWithinAbsoluteError (curve[(size_t) (fm - fA)], 62.0f, 0.05f, "display curve");

                // Saved and loaded with the project.
                NoteModel m1, m2;
                m1.replaceAll (drawn);
                expectEquals (m2.fromValueTree (m1.toValueTree()), 0);
                const auto back = m2.getSnapshot();
                expect (back.size() == 1, "loaded note count");
                if (back.size() == 1) expect (back[0] == drawn[0], "loaded drawing equals");
            }
        }

        beginTest ("Time: move and stretch notes; neighbours follow; total length kept");
        {
            const double sr = 48000.0;
            auto s = makeSetup (melody, sr, 2.6);
            if (s.notes.size() == 3)
            {
                auto moved = s.notes;
                moved[1].timeShiftSeconds = 0.12;   // later
                auto y = renderWith (s, moved);
                auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                expectEquals ((int) r.notes.size(), 3);
                if (r.notes.size() == 3)
                {
                    expectWithinAbsoluteError (r.notes[1].startSeconds, s.notes[1].startSeconds + 0.12, 0.03);
                    expectWithinAbsoluteError (r.notes[1].pitch, 60.0f, 0.15f);
                    expectWithinAbsoluteError (r.notes[0].pitch, 57.0f, 0.15f);
                }
                expectEquals (y.size(), s.audio.size());

                auto stretched = s.notes;
                stretched[2].endEditSeconds = 0.2;   // last note longer, into the silence after it
                y = renderWith (s, stretched);
                r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                if (r.notes.size() == 3)
                {
                    expectWithinAbsoluteError (r.notes[2].endSeconds - r.notes[2].startSeconds,
                                               s.notes[2].duration() + 0.2, 0.04);
                    expectWithinAbsoluteError (r.notes[2].pitch, 62.0f, 0.15f);
                }
            }
        }

        beginTest ("Consonants keep their length when the note before them is shortened");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.2, 0.9, 60 }, { 1.0, 1.6, 64 } }, sr, 1.8,
                                [sr] (std::vector<float>& a) { addNoise (a, sr, 0.925, 0.975, 0.15f, true, 3); });
            expectEquals ((int) s.notes.size(), 2);
            if (s.notes.size() == 2)
            {
                auto e = s.notes;
                e[1].timeShiftSeconds = -0.015;   // second note earlier: the gap with the "s" shrinks
                const auto map = TimeMap::build (e, &s.analysis, 0.0, 1.8, true);
                // The sibilant region keeps its speed; the voiced edges absorb the change.
                expectWithinAbsoluteError (map.rateAt (map.toOutput (0.95)), 1.0, 0.02);
                const auto unprotected = TimeMap::build (e, &s.analysis, 0.0, 1.8, false);
                expectGreaterThan (unprotected.rateAt (unprotected.toOutput (0.95)), 1.1);

                // Gap squeezed below the consonant's own length: the consonant is compressed
                // only as much as needed, much less than with uniform scaling.
                e[1].timeShiftSeconds = -0.03;
                const auto tight = TimeMap::build (e, &s.analysis, 0.0, 1.8, true);
                const auto tightU = TimeMap::build (e, &s.analysis, 0.0, 1.8, false);
                const double rProt = tight.rateAt (tight.toOutput (0.95)), rUni = tightU.rateAt (tightU.toOutput (0.95));
                expectLessThan (rProt, rUni - 0.15);
                expectEquals (tight.toOutput (1.8), 1.8);
            }
        }

        beginTest ("Note EQ only affects its own note");
        {
            const double sr = 48000.0;
            auto s = makeSetup (melody, sr, 2.6);
            if (s.notes.size() == 3)
            {
                auto e = s.notes;
                e[1].eq.enabled = true;
                e[1].eq.bands[2] = { true, EqBand::peak, 1046.5f, 12.0f, 2.0f };   // 4th harmonic of C4
                const auto y = renderWith (s, e);
                const float boost = bandEnergyDb (y, 1.0, 1.5, sr, 1046.5) - bandEnergyDb (s.audio, 1.0, 1.5, sr, 1046.5);
                expectGreaterThan (boost, 8.0f);
                float diff = 0.0f;
                for (auto i = (size_t) (0.3 * sr); i < (size_t) (0.8 * sr); ++i) diff = std::max (diff, std::abs (y[i] - s.audio[i]));
                expectEquals (diff, 0.0f);

                RenderOptions noEq; noEq.eq = false;   // bypass stage
                expect (VocalRenderer::render (*s.take, s.analysis, e, noEq) == nullptr);
            }
        }

        beginTest ("Note gain");
        {
            const double sr = 44100.0;
            auto s = makeSetup (melody, sr, 2.6);
            if (s.notes.size() == 3)
            {
                auto e = s.notes;
                e[2].gainDb = -6.0f;
                const auto y = renderWith (s, e);
                expectWithinAbsoluteError (db (rms (y, 1.75, 2.1, sr) / rms (s.audio, 1.75, 2.1, sr)), -6.0, 0.3);
            }
        }

        beginTest ("Scale correction layer: on/off and speed");
        {
            const double sr = 48000.0;
            auto s = makeSetup ({ { 0.2, 1.2, 60.4 } }, sr, 1.4);   // 40 cents sharp
            if (s.notes.size() == 1)
            {
                auto e = s.notes;
                e[0].correctionCents = -40.0f;
                auto y = renderWith (s, e);
                auto r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                expectWithinAbsoluteError (medianPitch (r, 0.4, 1.1), 60.0f, 0.06f);

                RenderOptions off; off.scaleCorrection = false;
                expect (VocalRenderer::render (*s.take, s.analysis, e, off) == nullptr);

                e[0].correctionSpeedMs = 300.0f;   // slow: the start keeps more of the original
                y = renderWith (s, e);
                r = VocalAnalyzer::analyseBuffer (y.data(), (int) y.size(), sr);
                expectGreaterThan (medianPitch (r, 0.22, 0.30), 60.2f);
                expectWithinAbsoluteError (medianPitch (r, 0.7, 1.1), 60.0f, 0.06f);
            }
        }

        beginTest ("Performance: 3 minutes with every note edited");
        {
            const double sr = 48000.0;
            std::vector<float> audio ((size_t) (180.0 * sr), 0.0f);
            VoiceSynth synth { sr };
            for (double t = 0.5; t < 179.0; t += 0.45)
            {
                const auto a = (juce::int64) (t * sr), b = (juce::int64) ((t + 0.4) * sr);
                synth.render (audio, a, (int) (b - a), [] (double) { return 60.0; },
                              [t] (double x) { return envelope (x, t, t + 0.4); });
            }
            TakeAudio take (sr);
            take.write (0, audio.data(), (int) audio.size());
            const auto analysis = VocalAnalyzer::analyse (take);
            std::vector<Note> notes;
            juce::int64 id = 1;
            for (const auto& a : analysis.notes)
            {
                Note n; n.id = id++; n.startSeconds = a.startSeconds; n.endSeconds = a.endSeconds;
                n.detectedPitch = a.pitch; n.pitchShiftCents = 150.0f; n.timeShiftSeconds = 0.01;
                notes.push_back (n);
            }
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            auto r = VocalRenderer::render (take, analysis, notes, {});
            const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            logMessage ("  full render of 180 s took " + juce::String (secs, 2) + " s");

            // An edit with the voice analysis already cached (what happens while the user works).
            VoiceModelCache cache;
            VocalRenderer::render (take, analysis, notes, {}, nullptr, &cache);
            for (auto& n : notes) n.pitchShiftCents = 120.0f;
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            auto r2 = VocalRenderer::render (take, analysis, notes, {}, nullptr, &cache);
            const double secs2 = (juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0;
            logMessage ("  re-render after an edit (analysis cached) took " + juce::String (secs2, 2) + " s");
            expect (r2 != nullptr);
            RenderOptions tv; tv.engine = Engine::trueVoice;
            const auto t2 = juce::Time::getMillisecondCounterHiRes();
            VocalRenderer::render (take, analysis, notes, tv, nullptr, &cache);
            notes[notes.size() / 2].pitchShiftCents = 300.0f;   // one note changed
            const auto t3 = juce::Time::getMillisecondCounterHiRes();
            VocalRenderer::render (take, analysis, notes, {}, nullptr, &cache);
            const double secs3 = (juce::Time::getMillisecondCounterHiRes() - t3) / 1000.0;
            logMessage ("  re-render after editing one note took " + juce::String (secs3, 2) + " s");
            expectLessThan (secs3, 4.0);   // shared CI machines are slower; the time is logged above
            logMessage ("  True Voice re-render took " + juce::String ((juce::Time::getMillisecondCounterHiRes() - t2) / 1000.0, 2) + " s");
            expect (r != nullptr);
            expectLessThan (secs, 30.0);
        }
    }
};

static RenderTests renderTests;
