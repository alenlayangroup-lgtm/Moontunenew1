#include "PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace moontune
{

namespace
{
    constexpr float candidateThreshold = 0.5f;   // CMNDF dips above this are not period candidates
    constexpr int maxCandidates = 5;
    constexpr float unvoicedCost = 0.40f;        // cost of declaring an audible frame unvoiced
    constexpr float voicingSwitchCost = 0.20f;
    constexpr float semitoneJumpCost = 0.035f;   // per semitone between consecutive frames
    constexpr float longerLagBias = 0.15f;       // per octave below the frame's highest candidate

    struct Candidate { float pitch; float cmndf; float lag; };

    float lagToMidi (float lag, double fs) noexcept
    {
        const double f0 = fs / (double) lag;
        return (float) (69.0 + 12.0 * std::log2 (f0 / 440.0));
    }

    // 2nd-order Butterworth high-pass (removes rumble/DC that disturbs the difference function).
    void highPass (std::vector<float>& x, double fs, double cutoff)
    {
        const double w0 = juce::MathConstants<double>::twoPi * cutoff / fs;
        const double cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.7071067811865476);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cosw) / 2.0 / a0, b1 = -(1.0 + cosw) / a0, b2 = b0;
        const double a1 = -2.0 * cosw / a0, a2 = (1.0 - alpha) / a0;

        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& s : x)
        {
            const double in = s;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in; y2 = y1; y1 = out;
            s = (float) out;
        }
    }
}

int PitchDetector::decimationFactor (double sampleRate) noexcept
{
    return juce::jmax (1, (int) std::lround (sampleRate / 22050.0));
}

std::vector<float> PitchDetector::decimate (const float* x, int n, int factor)
{
    if (factor <= 1)
        return std::vector<float> (x, x + n);

    // Windowed-sinc low-pass at 0.45 x output rate, Blackman window.
    const int half = 32 * factor;
    const int taps = 2 * half + 1;
    const double fc = 0.45 / (double) factor;   // normalised to input rate
    std::vector<float> h ((size_t) taps);
    double sum = 0.0;
    for (int k = 0; k < taps; ++k)
    {
        const double m = k - half;
        const double sinc = (k == half) ? 2.0 * fc : std::sin (juce::MathConstants<double>::twoPi * fc * m) / (juce::MathConstants<double>::pi * m);
        const double w = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * k / (taps - 1))
                              + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * k / (taps - 1));
        h[(size_t) k] = (float) (sinc * w);
        sum += sinc * w;
    }
    for (auto& c : h)
        c = (float) (c / sum);

    const int outLen = n / factor;
    std::vector<float> y ((size_t) outLen, 0.0f);
    for (int m = 0; m < outLen; ++m)
    {
        const int centre = m * factor;
        const int k0 = juce::jmax (0, half - centre);
        const int k1 = juce::jmin (taps, n - centre + half);
        float acc = 0.0f;
        const float* src = x + centre - half;
        for (int k = k0; k < k1; ++k)
            acc += h[(size_t) k] * src[k];
        y[(size_t) m] = acc;
    }
    return y;
}

void PitchDetector::analyse (const float* samples, int numSamples, double sampleRate, double startSeconds,
                             const Settings& settings, std::vector<AnalysisFrame>& frames, float silenceGateDb)
{
    if (numSamples <= 0 || frames.empty())
        return;

    const int factor = decimationFactor (sampleRate);
    auto y = decimate (samples, numSamples, factor);
    const double fs = sampleRate / factor;
    highPass (y, fs, 50.0);
    const int ny = (int) y.size();

    const int W = juce::jmax (64, (int) std::lround (fs * settings.windowSeconds));
    const int maxLag = juce::jmin (W, (int) std::ceil (fs / settings.minF0));
    const int minLag = juce::jmax (2, (int) std::floor (fs / settings.maxF0));
    const int segLen = W + maxLag;

    int order = 1;
    while ((1 << order) < segLen + 1)
        ++order;
    const int N = 1 << order;
    juce::dsp::FFT fft (order);

    std::vector<float> fa ((size_t) N * 2), fb ((size_t) N * 2), seg ((size_t) segLen);
    std::vector<double> sq ((size_t) segLen + 1), d ((size_t) maxLag + 1), cm ((size_t) maxLag + 1);

    const double hop = settings.hopSeconds;
    const double endSeconds = startSeconds + numSamples / sampleRate;
    const int firstFrame = juce::jmax (0, (int) std::ceil (startSeconds / hop - 1.0e-9));
    const int lastFrame = juce::jmin ((int) frames.size() - 1, (int) std::floor (endSeconds / hop - 1.0e-9));
    if (lastFrame < firstFrame)
        return;

    const int count = lastFrame - firstFrame + 1;
    std::vector<std::vector<Candidate>> cands ((size_t) count);
    std::vector<bool> audible ((size_t) count, false);

    for (int fi = 0; fi < count; ++fi)
    {
        const int gi = firstFrame + fi;
        audible[(size_t) fi] = frames[(size_t) gi].levelDb > silenceGateDb;
        if (! audible[(size_t) fi])
            continue;

        const double centre = (gi * hop - startSeconds) * fs;
        const int s0 = (int) std::lround (centre - segLen * 0.5);
        for (int j = 0; j < segLen; ++j)
        {
            const int idx = s0 + j;
            seg[(size_t) j] = (idx >= 0 && idx < ny) ? y[(size_t) idx] : 0.0f;
        }

        // Cross-correlation r(tau) = sum_{j<W} seg[j] * seg[j+tau] via FFT.
        std::fill (fa.begin(), fa.end(), 0.0f);
        std::fill (fb.begin(), fb.end(), 0.0f);
        std::copy_n (seg.data(), W, fa.data());
        std::copy_n (seg.data(), segLen, fb.data());
        fft.performRealOnlyForwardTransform (fa.data());
        fft.performRealOnlyForwardTransform (fb.data());
        for (int k = 0; k <= N / 2; ++k)
        {
            const float ar = fa[(size_t) (2 * k)], ai = fa[(size_t) (2 * k + 1)];
            const float br = fb[(size_t) (2 * k)], bi = fb[(size_t) (2 * k + 1)];
            fb[(size_t) (2 * k)]     = ar * br + ai * bi;   // conj(A) * B
            fb[(size_t) (2 * k + 1)] = ar * bi - ai * br;
        }
        fft.performRealOnlyInverseTransform (fb.data());   // JUCE scales the inverse by 1/N

        sq[0] = 0.0;
        for (int j = 0; j < segLen; ++j)
            sq[(size_t) j + 1] = sq[(size_t) j] + (double) seg[(size_t) j] * seg[(size_t) j];

        const double e0 = sq[(size_t) W];
        if (e0 <= 1.0e-12)
        {
            audible[(size_t) fi] = false;
            continue;
        }

        double running = 0.0;
        cm[0] = 1.0;
        for (int tau = 0; tau <= maxLag; ++tau)
        {
            const double etau = sq[(size_t) (tau + W)] - sq[(size_t) tau];
            d[(size_t) tau] = juce::jmax (0.0, e0 + etau - 2.0 * (double) fb[(size_t) tau]);
            if (tau > 0)
            {
                running += d[(size_t) tau];
                cm[(size_t) tau] = running > 0.0 ? d[(size_t) tau] * tau / running : 1.0;
            }
        }

        auto& list = cands[(size_t) fi];
        for (int tau = juce::jmax (minLag, 2); tau < maxLag; ++tau)
        {
            const double v = cm[(size_t) tau];
            if (v >= candidateThreshold || v > cm[(size_t) tau - 1] || v > cm[(size_t) tau + 1])
                continue;

            // Parabolic interpolation of the dip.
            const double a = cm[(size_t) tau - 1], b = v, c = cm[(size_t) tau + 1];
            const double denom = a - 2.0 * b + c;
            double shift = denom > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0;
            shift = juce::jlimit (-0.5, 0.5, shift);
            const double refinedValue = b - 0.25 * (a - c) * shift;
            const float lag = (float) (tau + shift);
            list.push_back ({ lagToMidi (lag, fs), (float) juce::jlimit (0.0, 1.0, refinedValue), lag });
        }

        // Keep the shortest-lag candidates: for a periodic voice every multiple of the true
        // period is also a deep dip, so keeping only the deepest ones can drop the true period.
        if ((int) list.size() > maxCandidates)
            list.resize (maxCandidates);   // list is already ordered by lag
    }

    // --- Viterbi over candidates + unvoiced state -----------------------------------
    // State index: 0..k-1 = candidates of the frame, k = unvoiced.
    constexpr float inf = std::numeric_limits<float>::infinity();
    std::vector<std::vector<float>> cost ((size_t) count);
    std::vector<std::vector<int>> back ((size_t) count);

    auto localCost = [&] (int fi, int s) -> float
    {
        const auto& list = cands[(size_t) fi];
        const int k = (int) list.size();
        if (s == k)
            return audible[(size_t) fi] ? unvoicedCost : 0.0f;

        float minLagInFrame = list[0].lag;
        for (const auto& c : list)
            minLagInFrame = std::min (minLagInFrame, c.lag);
        return list[(size_t) s].cmndf + longerLagBias * std::log2 (list[(size_t) s].lag / minLagInFrame);
    };

    auto transition = [&] (int fiPrev, int sPrev, int fi, int s) -> float
    {
        const bool prevVoiced = sPrev < (int) cands[(size_t) fiPrev].size();
        const bool voiced = s < (int) cands[(size_t) fi].size();
        if (prevVoiced != voiced)
            return voicingSwitchCost;
        if (! voiced)
            return 0.0f;
        return semitoneJumpCost * std::abs (cands[(size_t) fi][(size_t) s].pitch - cands[(size_t) fiPrev][(size_t) sPrev].pitch);
    };

    for (int fi = 0; fi < count; ++fi)
    {
        const int k = (int) cands[(size_t) fi].size();
        cost[(size_t) fi].assign ((size_t) k + 1, inf);
        back[(size_t) fi].assign ((size_t) k + 1, -1);

        for (int s = 0; s <= k; ++s)
        {
            const float lc = localCost (fi, s);
            if (fi == 0)
            {
                cost[0][(size_t) s] = lc;
                continue;
            }

            const int kp = (int) cands[(size_t) fi - 1].size();
            for (int sp = 0; sp <= kp; ++sp)
            {
                const float c = cost[(size_t) fi - 1][(size_t) sp] + transition (fi - 1, sp, fi, s) + lc;
                if (c < cost[(size_t) fi][(size_t) s])
                {
                    cost[(size_t) fi][(size_t) s] = c;
                    back[(size_t) fi][(size_t) s] = sp;
                }
            }
        }
    }

    std::vector<int> path ((size_t) count, 0);
    {
        const auto& last = cost[(size_t) count - 1];
        path[(size_t) count - 1] = (int) (std::min_element (last.begin(), last.end()) - last.begin());
        for (int fi = count - 1; fi > 0; --fi)
            path[(size_t) fi - 1] = back[(size_t) fi][(size_t) path[(size_t) fi]];
    }

    for (int fi = 0; fi < count; ++fi)
    {
        auto& f = frames[(size_t) (firstFrame + fi)];
        const int s = path[(size_t) fi];
        const auto& list = cands[(size_t) fi];
        if (s < (int) list.size())
        {
            f.cls = FrameClass::Voiced;
            f.pitch = list[(size_t) s].pitch;
            f.confidence = 1.0f - list[(size_t) s].cmndf;
        }
        else
        {
            f.cls = audible[(size_t) fi] ? FrameClass::Unvoiced : FrameClass::Silence;
            f.pitch = std::numeric_limits<float>::quiet_NaN();
            f.confidence = 0.0f;
        }
    }

    // --- Clean-up -------------------------------------------------------------------
    auto isVoiced = [&] (int gi) { return frames[(size_t) gi].cls == FrameClass::Voiced; };

    // Bridge dropouts of up to 3 frames when the pitch on both sides agrees.
    for (int gi = firstFrame + 1; gi < lastFrame; ++gi)
    {
        if (isVoiced (gi) || ! isVoiced (gi - 1))
            continue;
        int g2 = gi;
        while (g2 <= lastFrame && ! isVoiced (g2) && g2 - gi < 4)
            ++g2;
        if (g2 <= lastFrame && isVoiced (g2) && g2 - gi <= 3
            && std::abs (frames[(size_t) g2].pitch - frames[(size_t) gi - 1].pitch) < 1.5f)
        {
            const float p0 = frames[(size_t) gi - 1].pitch, p1 = frames[(size_t) g2].pitch;
            for (int g = gi; g < g2; ++g)
            {
                const float t = (float) (g - gi + 1) / (float) (g2 - gi + 1);
                auto& f = frames[(size_t) g];
                f.cls = FrameClass::Voiced;
                f.pitch = p0 + (p1 - p0) * t;
                f.confidence = 0.3f;
                f.interpolated = true;
            }
        }
        gi = g2;
    }

    // Voiced runs shorter than 30 ms are not trustworthy -> unvoiced.
    const int minRun = juce::jmax (2, (int) std::lround (0.030 / hop));
    for (int gi = firstFrame; gi <= lastFrame;)
    {
        if (! isVoiced (gi)) { ++gi; continue; }
        int g2 = gi;
        while (g2 <= lastFrame && isVoiced (g2))
            ++g2;
        if (g2 - gi < minRun)
            for (int g = gi; g < g2; ++g)
            {
                auto& f = frames[(size_t) g];
                f.cls = f.levelDb > silenceGateDb ? FrameClass::Unvoiced : FrameClass::Silence;
                f.pitch = std::numeric_limits<float>::quiet_NaN();
                f.confidence = 0.0f;
                f.interpolated = false;
            }
        gi = g2;
    }

    // 3-point median inside voiced runs removes isolated one-frame spikes.
    std::vector<float> smoothed ((size_t) count);
    for (int fi = 0; fi < count; ++fi)
    {
        const int gi = firstFrame + fi;
        smoothed[(size_t) fi] = frames[(size_t) gi].pitch;
        if (gi > firstFrame && gi < lastFrame && isVoiced (gi - 1) && isVoiced (gi) && isVoiced (gi + 1))
        {
            float a = frames[(size_t) gi - 1].pitch, b = frames[(size_t) gi].pitch, c = frames[(size_t) gi + 1].pitch;
            smoothed[(size_t) fi] = std::max (std::min (a, b), std::min (std::max (a, b), c));
        }
    }
    for (int fi = 0; fi < count; ++fi)
        if (isVoiced (firstFrame + fi))
            frames[(size_t) (firstFrame + fi)].pitch = smoothed[(size_t) fi];
}

} // namespace moontune
