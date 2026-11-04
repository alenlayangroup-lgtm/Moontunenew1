#include "VocalAnalyzer.h"
#include <limits>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace moontune
{

namespace
{
    bool isVoiced (const AnalysisFrame& f) noexcept { return f.cls == FrameClass::Voiced; }

    // Confidence-weighted median of the pitch over frames [a, b), using the middle part only.
    float weightedCentre (const std::vector<AnalysisFrame>& frames, int a, int b, float trimFraction, float fallback)
    {
        const int len = b - a;
        if (len <= 0)
            return fallback;

        int trim = (int) std::floor (len * trimFraction);
        if (len - 2 * trim < 3)
            trim = 0;

        std::vector<std::pair<float, float>> pw;   // pitch, weight
        for (int i = a + trim; i < b - trim; ++i)
        {
            const auto& f = frames[(size_t) i];
            if (! isVoiced (f) || ! std::isfinite (f.pitch))
                continue;
            pw.emplace_back (f.pitch, f.interpolated ? 0.05f : std::max (0.05f, f.confidence));
        }
        if (pw.empty())
            return fallback;

        std::sort (pw.begin(), pw.end());
        float total = 0.0f;
        for (auto& x : pw) total += x.second;
        float acc = 0.0f;
        for (auto& x : pw)
        {
            acc += x.second;
            if (acc >= total * 0.5f)
                return x.first;
        }
        return pw.back().first;
    }

    std::vector<float> movingAverage (const std::vector<float>& x, int window)
    {
        const int n = (int) x.size(), half = window / 2;
        std::vector<double> prefix ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            prefix[(size_t) i + 1] = prefix[(size_t) i] + x[(size_t) i];
        std::vector<float> out ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const int lo = std::max (0, i - half), hi = std::min (n, i + half + 1);
            out[(size_t) i] = (float) ((prefix[(size_t) hi] - prefix[(size_t) lo]) / (hi - lo));
        }
        return out;
    }
}

float VocalAnalyzer::centrePitch (const AnalysisResult& r, double startSeconds, double endSeconds, float fallback)
{
    const int a = std::max (0, (int) std::ceil (startSeconds / r.hopSeconds - 0.5));
    const int b = std::min ((int) r.frames.size(), (int) std::ceil (endSeconds / r.hopSeconds - 0.5));
    return weightedCentre (r.frames, a, b, 0.2f, fallback);
}

AnalysisResult VocalAnalyzer::analyse (const TakeAudio& take, const Settings& settings,
                                       const std::atomic<bool>* cancel, const std::function<void (float)>& progress)
{
    std::vector<Span> spans;
    for (const auto& r : take.getRanges())
        spans.push_back ({ r.start, r.end });

    return run ([&take] (juce::int64 pos, float* dest, int n) { take.read (pos, dest, n); },
                spans, take.getEndSample(), take.getSampleRate(), settings, cancel, progress);
}

AnalysisResult VocalAnalyzer::analyseBuffer (const float* samples, int numSamples, double sampleRate,
                                             const Settings& settings)
{
    return run ([samples, numSamples] (juce::int64 pos, float* dest, int n)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const auto p = pos + i;
                        dest[i] = (p >= 0 && p < numSamples) ? samples[p] : 0.0f;
                    }
                },
                { { 0, (juce::int64) numSamples } }, numSamples, sampleRate, settings, nullptr, {});
}

AnalysisResult VocalAnalyzer::run (const std::function<void (juce::int64, float*, int)>& reader,
                                   const std::vector<Span>& spans, juce::int64 totalSamples, double sampleRate,
                                   const Settings& settings, const std::atomic<bool>* cancel,
                                   const std::function<void (float)>& progress)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    AnalysisResult result;
    result.hopSeconds = settings.pitch.hopSeconds;
    const double hop = result.hopSeconds;

    const int numFrames = (int) std::ceil ((double) totalSamples / sampleRate / hop) + 1;
    result.frames.assign ((size_t) std::max (0, numFrames), {});
    if (totalSamples <= 0 || spans.empty())
        return result;

    // Pass 1: levels (RMS over 20 ms around each frame centre).
    const int halfWin = std::max (1, (int) std::lround (sampleRate * 0.010));
    juce::int64 totalSpan = 0;
    for (const auto& s : spans)
        totalSpan += s.end - s.start;

    std::vector<std::vector<float>> spanAudio;
    for (const auto& s : spans)
    {
        std::vector<float> audio ((size_t) (s.end - s.start));
        reader (s.start, audio.data(), (int) audio.size());

        std::vector<double> sq (audio.size() + 1, 0.0), hq (audio.size() + 1, 0.0);
        for (size_t i = 0; i < audio.size(); ++i)
            sq[i + 1] = sq[i] + (double) audio[i] * audio[i];
        {
            // 4 kHz high-pass (RBJ, Q 0.707): energy of sibilants ("s", "ts") lives up there.
            const double w0 = 2.0 * juce::MathConstants<double>::pi * std::min (4000.0, 0.4 * sampleRate) / sampleRate;
            const double alpha = std::sin (w0) / (2.0 * 0.7071), c = std::cos (w0);
            const double a0 = 1.0 + alpha;
            const double b0 = (1.0 + c) / 2.0 / a0, b1 = -(1.0 + c) / a0, b2 = b0, a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (size_t i = 0; i < audio.size(); ++i)
            {
                const double x = audio[i];
                const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                hq[i + 1] = hq[i] + y * y;
            }
        }

        const int f0 = std::max (0, (int) std::ceil ((double) s.start / sampleRate / hop - 1.0e-9));
        const int f1 = std::min (numFrames - 1, (int) std::floor ((double) s.end / sampleRate / hop - 1.0e-9));
        for (int fi = f0; fi <= f1; ++fi)
        {
            const auto centre = (juce::int64) std::llround (fi * hop * sampleRate) - s.start;
            const auto lo = std::clamp<juce::int64> (centre - halfWin, 0, (juce::int64) audio.size());
            const auto hi = std::clamp<juce::int64> (centre + halfWin, 0, (juce::int64) audio.size());
            if (hi <= lo)
                continue;
            const double ms = (sq[(size_t) hi] - sq[(size_t) lo]) / (double) (hi - lo);
            result.frames[(size_t) fi].levelDb = (float) (10.0 * std::log10 (ms + 1.0e-12));
            const double e = sq[(size_t) hi] - sq[(size_t) lo];
            result.frames[(size_t) fi].hfRatio = e > 1.0e-12 ? (float) juce::jlimit (0.0, 1.0, (hq[(size_t) hi] - hq[(size_t) lo]) / e) : 0.0f;
        }
        spanAudio.push_back (std::move (audio));
    }

    for (const auto& f : result.frames)
        result.peakLevelDb = std::max (result.peakLevelDb, f.levelDb);
    const float gate = std::max (-60.0f, result.peakLevelDb - 45.0f);

    // Pass 2: pitch per captured span.
    juce::int64 done = 0;
    for (size_t si = 0; si < spans.size(); ++si)
    {
        if (cancel != nullptr && cancel->load())
            return {};

        const auto& s = spans[si];
        PitchDetector::analyse (spanAudio[si].data(), (int) spanAudio[si].size(), sampleRate,
                                (double) s.start / sampleRate, settings.pitch, result.frames, gate);
        spanAudio[si].clear();
        spanAudio[si].shrink_to_fit();

        done += s.end - s.start;
        if (progress)
            progress ((float) done / (float) std::max<juce::int64> (1, totalSpan));
    }

    // Sibilants next to a vowel can look periodic to the pitch tracker: a frame whose energy is
    // mostly above 4 kHz is a consonant, not a sung note.
    for (auto& f : result.frames)
        if (f.cls == FrameClass::Voiced && f.hfRatio >= 0.6f)
        {
            f.cls = FrameClass::Unvoiced;
            f.pitch = std::numeric_limits<float>::quiet_NaN();
            f.confidence = 0.0f;
        }

    segmentNotes (result, settings);

    // Noise regions: audible unvoiced runs of at least 15 ms, classified as
    //   sibilant ("s", "ts", "sh"): most energy above 4 kHz;
    //   plosive  ("k", "t", "p"):   short burst right after a quiet closure;
    //   breath:                      everything else (longer, broadband, darker).
    const int minNoise = std::max (1, (int) std::lround (0.015 / hop));
    for (int i = 0; i < numFrames;)
    {
        if (result.frames[(size_t) i].cls != FrameClass::Unvoiced) { ++i; continue; }
        int j = i;
        while (j < numFrames && result.frames[(size_t) j].cls == FrameClass::Unvoiced)
            ++j;
        if (j - i >= minNoise)
        {
            NoiseRegion nz { std::max (0.0, (i - 0.5) * hop), (j - 0.5) * hop, NoiseRegion::breath };
            double eSum = 0.0, hfSum = 0.0;
            float peak = -120.0f;
            for (int k = i; k < j; ++k)
            {
                const auto& f = result.frames[(size_t) k];
                const double e = std::pow (10.0, f.levelDb / 10.0);
                eSum += e; hfSum += e * f.hfRatio;
                peak = std::max (peak, f.levelDb);
            }
            const double hf = eSum > 0.0 ? hfSum / eSum : 0.0;
            float before = -120.0f;
            for (int k = std::max (0, i - 4); k < i; ++k) before = std::max (before, result.frames[(size_t) k].levelDb);
            const double dur = nz.endSeconds - nz.startSeconds;
            const bool burst = dur <= 0.06 && (before < peak - 10.0f || i == 0 || result.frames[(size_t) i - 1].cls == FrameClass::Silence);
            if (burst && hf < 0.6)
                nz.kind = NoiseRegion::plosive;      // short burst after a closure: k / t / p
            else if (hf >= 0.4)
                nz.kind = NoiseRegion::sibilant;     // s / ts / sh
            else if (dur < 0.08)
                nz.kind = NoiseRegion::plosive;
            result.noise.push_back (nz);
        }
        i = j;
    }

    result.analysisSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    return result;
}

void VocalAnalyzer::segmentNotes (AnalysisResult& r, const Settings& settings)
{
    const double hop = r.hopSeconds;
    const int n = (int) r.frames.size();
    const int smoothWin = std::max (3, (int) std::lround (0.160 / hop));     // ~ one vibrato cycle
    const int hold = std::max (2, (int) std::lround (settings.noteChangeHoldSeconds / hop));
    const int minNote = std::max (2, (int) std::lround (settings.minNoteSeconds / hop));
    const int dipRadius = std::max (2, (int) std::lround (0.100 / hop));

    for (int a = 0; a < n;)
    {
        if (! isVoiced (r.frames[(size_t) a])) { ++a; continue; }
        int b = a;
        while (b < n && isVoiced (r.frames[(size_t) b]))
            ++b;

        // --- Run [a, b) ---------------------------------------------------------
        const int len = b - a;
        std::vector<float> p ((size_t) len), lv ((size_t) len);
        for (int i = 0; i < len; ++i)
        {
            p[(size_t) i] = r.frames[(size_t) (a + i)].pitch;
            lv[(size_t) i] = r.frames[(size_t) (a + i)].levelDb;
        }
        const auto s = movingAverage (p, smoothWin);

        std::vector<int> bounds { 0 };

        // Pitch-level changes.
        {
            int segStart = 0, over = 0;
            double sum = 0.0;
            int count = 0;
            for (int i = 0; i < len; ++i)
            {
                const double ref = count > 0 ? sum / count : s[(size_t) i];
                if (std::abs (s[(size_t) i] - ref) > settings.noteChangeSemitones)
                    ++over;
                else
                {
                    over = 0;
                    sum += s[(size_t) i];
                    ++count;
                }

                if (over >= hold)
                {
                    // Boundary at the steepest raw-pitch movement in the transition zone.
                    const int lo = std::max (segStart + 1, i - over - smoothWin / 2);
                    const int hi = std::min (len - 2, i);
                    int best = std::clamp (i - over + 1, segStart + 1, std::max (segStart + 1, len - 1));
                    float bestSlope = -1.0f;
                    for (int k = lo; k <= hi; ++k)
                    {
                        const float slope = std::abs (p[(size_t) k + 1] - p[(size_t) k - 1]);
                        if (slope > bestSlope) { bestSlope = slope; best = k; }
                    }
                    if (best > segStart)
                    {
                        bounds.push_back (best);
                        segStart = best;
                    }
                    sum = 0.0;
                    count = 0;
                    for (int k = segStart; k <= i; ++k) { sum += s[(size_t) k]; ++count; }
                    over = 0;
                }
            }
        }
        bounds.push_back (len);

        // Re-articulations: clear level dips inside a segment.
        {
            std::vector<int> extra;
            for (size_t bi = 0; bi + 1 < bounds.size(); ++bi)
            {
                const int s0 = bounds[bi], s1 = bounds[bi + 1];
                for (int m = s0 + minNote; m < s1 - minNote; ++m)
                {
                    bool isMin = true;
                    for (int k = std::max (s0, m - dipRadius / 2); k <= std::min (s1 - 1, m + dipRadius / 2) && isMin; ++k)
                        if (lv[(size_t) k] < lv[(size_t) m])
                            isMin = false;
                    if (! isMin)
                        continue;

                    float leftMax = -200.0f, rightMax = -200.0f;
                    for (int k = std::max (s0, m - dipRadius); k < m; ++k) leftMax = std::max (leftMax, lv[(size_t) k]);
                    for (int k = m + 1; k <= std::min (s1 - 1, m + dipRadius); ++k) rightMax = std::max (rightMax, lv[(size_t) k]);
                    if (std::min (leftMax, rightMax) - lv[(size_t) m] >= settings.rearticulationDipDb)
                    {
                        extra.push_back (m);
                        m += minNote;
                    }
                }
            }
            bounds.insert (bounds.end(), extra.begin(), extra.end());
            std::sort (bounds.begin(), bounds.end());
            bounds.erase (std::unique (bounds.begin(), bounds.end()), bounds.end());
        }

        // Merge fragments shorter than minNote into the neighbour with the closer pitch.
        {
            auto segPitch = [&] (int s0, int s1) { return weightedCentre (r.frames, a + s0, a + s1, 0.2f, 60.0f); };
            bool changed = true;
            while (changed && bounds.size() > 2)
            {
                changed = false;
                for (size_t bi = 0; bi + 1 < bounds.size(); ++bi)
                {
                    if (bounds[bi + 1] - bounds[bi] >= minNote)
                        continue;
                    const float here = segPitch (bounds[bi], bounds[bi + 1]);
                    const bool hasLeft = bi > 0, hasRight = bi + 2 < bounds.size();
                    if (! hasLeft && ! hasRight)
                        break;
                    bool mergeLeft = hasLeft;
                    if (hasLeft && hasRight)
                        mergeLeft = std::abs (segPitch (bounds[bi - 1], bounds[bi]) - here)
                                 <= std::abs (segPitch (bounds[bi + 1], bounds[bi + 2]) - here);
                    bounds.erase (bounds.begin() + (std::ptrdiff_t) (mergeLeft ? bi : bi + 1));
                    changed = true;
                    break;
                }
            }
        }

        // Create notes.
        for (size_t bi = 0; bi + 1 < bounds.size(); ++bi)
        {
            const int s0 = a + bounds[bi], s1 = a + bounds[bi + 1];
            const int segLen = s1 - s0;
            if (segLen < 2)
                continue;

            AnalysedNote note;
            note.startSeconds = std::max (0.0, (s0 - 0.5) * hop);
            note.endSeconds = (s1 - 0.5) * hop;
            note.pitch = weightedCentre (r.frames, s0, s1, 0.2f, r.frames[(size_t) s0].pitch);

            float confSum = 0.0f;
            int interp = 0;
            for (int i = s0; i < s1; ++i)
            {
                confSum += r.frames[(size_t) i].confidence;
                if (r.frames[(size_t) i].interpolated) ++interp;
            }
            note.confidence = confSum / (float) segLen;

            // Vibrato: oscillation of the pitch around its ~160 ms moving average.
            const double dur = note.endSeconds - note.startSeconds;
            if (dur >= 0.3)
            {
                const int c0 = s0 + segLen / 10, c1 = s1 - segLen / 10;
                std::vector<float> res;
                for (int i = c0; i < c1; ++i)
                    res.push_back (p[(size_t) (i - a)] - s[(size_t) (i - a)]);

                double rms = 0.0;
                for (auto v : res) rms += (double) v * v;
                rms = std::sqrt (rms / (double) std::max<size_t> (1, res.size()));

                int crossings = 0, sign = 0;
                for (auto v : res)
                {
                    const int sgn = v > 0.03f ? 1 : (v < -0.03f ? -1 : 0);
                    if (sgn != 0 && sign != 0 && sgn != sign) ++crossings;
                    if (sgn != 0) sign = sgn;
                }
                const double centralDur = (double) res.size() * hop;
                const float rate = centralDur > 0 ? (float) (crossings / (2.0 * centralDur)) : 0.0f;
                const float extent = (float) (2.0 * std::sqrt (2.0) * rms * 100.0);

                bool regular = false;
                if (rate > 0.0f)
                {
                    const int lag = (int) std::lround (1.0 / (rate * hop));
                    if (lag > 0 && lag < (int) res.size())
                    {
                        double num = 0.0, den = 0.0;
                        for (size_t i = 0; i + (size_t) lag < res.size(); ++i)
                            num += (double) res[i] * res[i + (size_t) lag];
                        for (auto v : res) den += (double) v * v;
                        regular = den > 0.0 && num / den > 0.3;
                    }
                }

                if (rate >= 3.5f && rate <= 9.0f && extent >= 30.0f && regular)
                {
                    note.hasVibrato = true;
                    note.vibratoRateHz = rate;
                    note.vibratoExtentCents = extent;
                }
            }

            note.reliable = note.confidence >= 0.7f && interp <= segLen / 5;
            r.notes.push_back (note);
        }

        a = b;
    }
}

} // namespace moontune
