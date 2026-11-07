#include "VocalRenderer.h"
#include "TimeMap.h"
#include "NoteEqFilter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <thread>

namespace moontune
{

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

    double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

    bool affectsPitch (const Note& n, const RenderOptions& o)
    {
        if (! o.pitch || ! n.isNote())
            return false;
        return std::abs (n.pitchShiftCents) > 0.01f || std::abs (n.pitchModulation - 1.0f) > 0.001f
            || std::abs (n.pitchDrift - 1.0f) > 0.001f || std::abs (n.pitchTilt) > 0.01f
            || n.transitionMs >= 0.0f || (o.scaleCorrection && n.hasCorrection()) || n.hasDrawing();
    }

    // Vibrato-free pitch inside a note: moving average (~155 ms) over its voiced frames.
    std::vector<float> smoothedPitch (const Note& n, const AnalysisResult& a, int& firstFrame)
    {
        const int f0 = std::max (0, a.frameIndexAt (n.startSeconds));
        const int f1 = std::min ((int) a.frames.size(), a.frameIndexAt (n.endSeconds) + 1);
        firstFrame = f0;
        std::vector<float> out ((size_t) std::max (0, f1 - f0), kNaN);
        const int half = 15;
        for (int i = f0; i < f1; ++i)
        {
            if (a.frames[(size_t) i].cls != FrameClass::Voiced)
                continue;
            double sum = 0.0;
            int count = 0;
            for (int j = std::max (f0, i - half); j < std::min (f1, i + half + 1); ++j)
                if (a.frames[(size_t) j].cls == FrameClass::Voiced && std::isfinite (a.frames[(size_t) j].pitch))
                {
                    sum += a.frames[(size_t) j].pitch;
                    ++count;
                }
            if (count > 0)
                out[(size_t) (i - f0)] = (float) (sum / count);
        }
        return out;
    }

    float correctionRamp (const Note& n, double outTime)
    {
        if (n.correctionSpeedMs <= 0.0f)
            return 1.0f;
        const double x = (outTime - n.outStart()) * 1000.0 / n.correctionSpeedMs;
        if (x >= 1.0) return 1.0f;
        if (x <= 0.0) return 0.0f;
        return (float) (0.5 - 0.5 * std::cos (pi * x));
    }

    float offsetCents (const Note& n, double outTime, const RenderOptions& o)
    {
        if (! o.pitch)
            return 0.0f;
        float v = n.pitchShiftCents;
        if (o.scaleCorrection && std::abs (n.correctionCents) > 0.0f)
            v += n.correctionCents * correctionRamp (n, outTime);
        return v;
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

    float interp (const std::vector<float>& v, double pos) noexcept
    {
        if (v.empty()) return 0.0f;
        if (pos <= 0.0) return v.front();
        const auto i = (size_t) pos;
        if (i + 1 >= v.size()) return v.back();
        const float t = (float) (pos - (double) i);
        return v[i] + (v[i + 1] - v[i]) * t;
    }

    float cubicAt (const std::vector<float>& x, double pos) noexcept
    {
        const auto i = (long) std::floor (pos);
        const float t = (float) (pos - (double) i);
        auto at = [&x] (long k) { return (k >= 0 && k < (long) x.size()) ? x[(size_t) k] : 0.0f; };
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        // Catmull-Rom
        return y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + t * (3.0f * (y1 - y2) + y3 - y0)));
    }

    double framePosStatic (juce::int64 rs, int i, double fs, double hop, int k0) noexcept
    {
        return ((double) rs + i) / fs / hop - k0;
    }

    using cf = std::complex<float>;
    constexpr double twoPi = 2.0 * pi;
    constexpr double maxHarmonicHz = 11000.0;

    // Frame-level pitch (gaps filled) and soft voicing weight over the whole analysis.
    struct FrameTrack
    {
        double hop = 0.005;
        std::vector<float> logF0, vw;

        explicit FrameTrack (const AnalysisResult& a) : hop (a.hopSeconds)
        {
            const size_t N = a.frames.size();
            logF0.assign (N, (float) std::log (200.0));
            std::vector<int> voiced;
            for (size_t i = 0; i < N; ++i)
                if (a.frames[i].cls == FrameClass::Voiced && std::isfinite (a.frames[i].pitch))
                    voiced.push_back ((int) i);
            if (! voiced.empty())
            {
                size_t v = 0;
                for (size_t i = 0; i < N; ++i)
                {
                    while (v + 1 < voiced.size() && voiced[v + 1] <= (int) i) ++v;
                    const int l = voiced[v];
                    const float ll = (float) std::log (midiToHz (a.frames[(size_t) l].pitch));
                    if ((int) i <= l || v + 1 >= voiced.size()) { logF0[i] = ll; continue; }
                    const int r = voiced[v + 1];
                    const float lr = (float) std::log (midiToHz (a.frames[(size_t) r].pitch));
                    logF0[i] = ll + (lr - ll) * (float) ((int) i - l) / (float) (r - l);
                }
            }
            // Voicing weight: voiced frames smoothed over +-2 frames (soft 20-25 ms edges).
            vw.assign (N, 0.0f);
            for (size_t i = 0; i < N; ++i)
            {
                int c = 0;
                for (int d = -2; d <= 2; ++d)
                {
                    const long j = (long) i + d;
                    if (j >= 0 && j < (long) N && a.frames[(size_t) j].cls == FrameClass::Voiced
                        && std::isfinite (a.frames[(size_t) j].pitch))
                        ++c;
                }
                vw[i] = (float) c / 5.0f;
            }
        }
        float lerpAt (const std::vector<float>& v, double sec) const noexcept
        {
            return interp (v, std::max (0.0, sec / hop));
        }
        float f0At (double sec) const noexcept { return std::exp (lerpAt (logF0, sec)); }
        float vwAt (double sec) const noexcept { return lerpAt (vw, sec); }
    };

    struct HFrame { float f0 = 0.0f; std::vector<cf> c; };   // harmonic coefficients (empty = unvoiced)

    // Source audio of one region with its phase-locked harmonic analysis.
    struct VoiceSpan
    {
        double fs = 44100.0;
        int s0 = 0, len = 0;                 // range-local source samples [s0, s0 + len)
        std::vector<float> x, f0, vw;
        std::vector<double> phi;             // integrated fundamental phase
        int hopS = 88;
        int j0 = 0;                          // frames[k] is frame j0 + k of the range (centre (j0+k)*hopS)
        using Frame = HFrame;
        std::vector<Frame> frames;
        std::vector<double> theta1;          // phase of the fundamental relative to phi (per sample)
        std::vector<float> attack;           // 1 = keep the original here (plosive bursts / attacks), 0 = process
        std::vector<float> harmonic;         // harmonic model at the original pitch

        double phiAt (double s) const noexcept
        {
            const double p = s - s0;
            if (p <= 0.0) return phi.front() + p * twoPi * f0.front() / fs;
            const auto i = (size_t) p;
            if (i + 1 >= phi.size()) return phi.back() + (p - (double) (phi.size() - 1)) * twoPi * f0.back() / fs;
            return phi[i] + (phi[i + 1] - phi[i]) * (p - (double) i);
        }
        template <typename V> static double at (const V& v, double p) noexcept
        {
            if (v.empty()) return 0.0;
            if (p <= 0.0) return v.front();
            const auto i = (size_t) p;
            if (i + 1 >= v.size()) return v.back();
            return v[i] + (v[i + 1] - v[i]) * (p - (double) i);
        }
        double f0At (double s) const noexcept { return at (f0, s - s0); }
        double vwAt (double s) const noexcept { return at (vw, s - s0); }
        double theta1At (double s) const noexcept { return at (theta1, s - s0); }
        float sample (long s) const noexcept { const long i = s - s0; return (i >= 0 && i < len) ? x[(size_t) i] : 0.0f; }
    };

    // out[i] += Re( sum_k (A_k + (B_k - A_k) tt[i]) e^{j (k+1) phase[i]} ), harmonic-major so the
    // inner loops run over samples and vectorise.
    void addHarmonics (const double* phase, const float* tt, int n, const std::vector<cf>& A,
                       const std::vector<cf>& B, float* out)
    {
        if (n <= 0) return;
        const size_t K = std::max (A.size(), B.size());
        if (K == 0) return;
        thread_local std::vector<float> zr, zi, pr, pi_;
        zr.resize ((size_t) n); zi.resize ((size_t) n); pr.resize ((size_t) n); pi_.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            zr[(size_t) i] = pr[(size_t) i] = (float) std::cos (phase[i]);
            zi[(size_t) i] = pi_[(size_t) i] = (float) std::sin (phase[i]);
        }
        for (size_t k = 0; k < K; ++k)
        {
            const cf a = k < A.size() ? A[k] : cf(), b = k < B.size() ? B[k] : cf();
            const float ar = a.real(), ai = a.imag(), dr = b.real() - ar, di = b.imag() - ai;
            float* P = pr.data(); float* Q = pi_.data();
            const float* Z = zr.data(); const float* W = zi.data();
            for (int i = 0; i < n; ++i)
            {
                const float cr = ar + dr * tt[i], ci = ai + di * tt[i];
                out[i] += cr * P[i] - ci * Q[i];
                const float nr = P[i] * Z[i] - Q[i] * W[i];
                Q[i] = P[i] * W[i] + Q[i] * Z[i];
                P[i] = nr;
            }
        }
    }

    // Runs fn(0..count-1) on up to hardware_concurrency threads (render thread included).
    template <typename Fn>
    void parallelFor (int count, Fn&& fn)
    {
        const int nThreads = juce::jlimit (1, 16, (int) std::thread::hardware_concurrency());
        if (nThreads <= 1 || count < 64) { for (int i = 0; i < count; ++i) fn (i); return; }
        std::atomic<int> next { 0 };
        auto worker = [&] { for (int i; (i = next.fetch_add (16)) < count;) for (int e = std::min (count, i + 16); i < e; ++i) fn (i); };
        std::vector<std::thread> pool;
        for (int t = 1; t < nThreads; ++t) pool.emplace_back (worker);
        worker();
        for (auto& t : pool) t.join();
    }

    int floorDiv (int a, int b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}

// Analysis of the voice that does not depend on the edits (fundamental phase, harmonic frames),
// kept between renders so an edit only costs the re-synthesis.
struct VoiceModelCache::Impl
{
    struct Range
    {
        int n = 0, hopS = 0;
        std::vector<double> phiCoarse;   // fundamental phase at every 1024th sample of the range
        std::vector<HFrame> frames;
        std::vector<char> computed;
    };
    const void* takeKey = nullptr;
    size_t analysisFrames = 0;
    double fs = 0.0;
    std::map<juce::int64, Range> ranges;

    // Rendered regions by a hash of everything that shaped them: an edit re-renders only the
    // phrases it touches.
    struct Region { std::vector<float> y; juce::uint64 lastUse = 0; };
    std::map<juce::uint64, Region> regions;
    juce::uint64 useCounter = 0;
    size_t regionSamples = 0;

    void trimRegions (size_t budget)
    {
        while (regionSamples > budget && ! regions.empty())
        {
            auto oldest = regions.begin();
            for (auto it = regions.begin(); it != regions.end(); ++it)
                if (it->second.lastUse < oldest->second.lastUse) oldest = it;
            regionSamples -= oldest->second.y.size();
            regions.erase (oldest);
        }
    }
};

VoiceModelCache::VoiceModelCache() : impl (std::make_unique<Impl>()) {}
VoiceModelCache::~VoiceModelCache() = default;
void VoiceModelCache::clear() { impl->ranges.clear(); impl->regions.clear(); impl->regionSamples = 0; impl->takeKey = nullptr; }

namespace
{
    using RangeModel = VoiceModelCache::Impl::Range;
    constexpr int coarseStep = 1024;

    float f0Sample (const FrameTrack& ft, juce::int64 rs, int n, double fs, long s) noexcept
    {
        s = juce::jlimit (0L, (long) std::max (0, n - 1), s);
        return juce::jlimit (50.0f, 1500.0f, ft.f0At (((double) rs + (double) s) / fs));
    }

    void prepareRange (RangeModel& rm, const FrameTrack& ft, juce::int64 rs, int n, double fs)
    {
        if (rm.hopS > 0 && rm.n == n)
            return;
        rm = {};
        rm.n = n;
        // Analysis hop ~0.6 period of the typical pitch of this range (1.5 .. 4 ms).
        std::vector<float> vf;
        const int f0i = std::max (0, (int) std::floor ((double) rs / fs / ft.hop));
        const int f1i = std::min ((int) ft.vw.size(), (int) std::ceil (((double) rs + n) / fs / ft.hop) + 1);
        for (int i = f0i; i < f1i; ++i) if (ft.vw[(size_t) i] >= 1.0f) vf.push_back (std::exp (ft.logF0[(size_t) i]));
        double med = 250.0;
        if (! vf.empty()) { std::nth_element (vf.begin(), vf.begin() + (long) vf.size() / 2, vf.end()); med = vf[vf.size() / 2]; }
        rm.hopS = std::max (8, juce::jlimit ((int) (0.0015 * fs), (int) (0.004 * fs), (int) std::lround (0.6 * fs / med)));
        rm.phiCoarse.assign ((size_t) (n / coarseStep + 2), 0.0);
        double phi = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (i % coarseStep == 0) rm.phiCoarse[(size_t) (i / coarseStep)] = phi;
            phi += twoPi * f0Sample (ft, rs, n, fs, i) / fs;
        }
        rm.phiCoarse[(size_t) (n / coarseStep) + 1] = phi;
        const size_t nF = (size_t) (n / rm.hopS + 2);
        rm.frames.assign (nF, {});
        rm.computed.assign (nF, 0);
    }

    double phiStart (const RangeModel& rm, const FrameTrack& ft, juce::int64 rs, double fs, int s0)
    {
        if (s0 <= 0)
            return s0 * twoPi * f0Sample (ft, rs, rm.n, fs, 0) / fs;
        const int s = std::min (s0, rm.n);
        const int k = s / coarseStep;
        double phi = rm.phiCoarse[(size_t) k];
        for (int i = k * coarseStep; i < s; ++i)
            phi += twoPi * f0Sample (ft, rs, rm.n, fs, i) / fs;
        if (s0 > rm.n)
            phi += (s0 - rm.n) * twoPi * f0Sample (ft, rs, rm.n, fs, rm.n - 1) / fs;
        return phi;
    }

    void buildSpan (VoiceSpan& v, const std::vector<float>& src, const FrameTrack& ft, juce::int64 rs,
                    int s0, int s1, double fs, bool wantHarmonics, RangeModel& rm)
    {
        v.fs = fs; v.s0 = s0; v.len = std::max (1, s1 - s0);
        const auto L = (size_t) v.len;
        const int n = rm.n;
        v.x.assign (L, 0.0f); v.f0.resize (L); v.vw.resize (L); v.phi.resize (L + 1);
        for (size_t i = 0; i < L; ++i)
        {
            const long s = s0 + (long) i;
            v.x[i] = (s >= 0 && s < (long) src.size()) ? src[(size_t) s] : 0.0f;
            v.f0[i] = f0Sample (ft, rs, n, fs, s);
            v.vw[i] = (s >= 0 && s < n) ? ft.vwAt (((double) rs + (double) s) / fs) : 0.0f;
        }
        v.phi[0] = phiStart (rm, ft, rs, fs, s0);
        for (size_t i = 0; i < L; ++i)
            v.phi[i + 1] = v.phi[i] + twoPi * v.f0[i] / fs;

        // Attack protection: the burst of b / p / d / t / k and other sharp onsets lasts a few ms and
        // would be smeared by any period-based processing. Where the 2 ms level jumps >= 9 dB above
        // the preceding 15 ms, the original is kept for 15 ms (5 ms fades in and out).
        {
            v.attack.assign (L, 0.0f);
            std::vector<double> sq (L + 1, 0.0);
            for (size_t i = 0; i < L; ++i) sq[i + 1] = sq[i] + (double) v.x[i] * v.x[i];
            const int w = std::max (1, (int) (0.002 * fs)), back = std::max (1, (int) (0.015 * fs));
            const int hold = (int) (0.015 * fs), fade = std::max (1, (int) (0.005 * fs)), pre = (int) (0.002 * fs);
            const double floorE = 1.0e-8;   // ignore onsets out of near-silence noise
            for (int i = back + w; i + w < v.len; i += std::max (1, w / 2))
            {
                const double eNow = (sq[(size_t) (i + w)] - sq[(size_t) i]) / w;
                const double ePrev = (sq[(size_t) (i - w)] - sq[(size_t) (i - w - back)]) / back;
                if (eNow < floorE || eNow < ePrev * 7.94)   // 9 dB
                    continue;
                for (int k = i - pre - fade; k < i + hold + fade; ++k)
                {
                    if (k < 0 || k >= v.len) continue;
                    float a = 1.0f;
                    if (k < i - pre) a = (float) (k - (i - pre - fade)) / (float) fade;
                    else if (k >= i + hold) a = 1.0f - (float) (k - (i + hold)) / (float) fade;
                    v.attack[(size_t) k] = std::max (v.attack[(size_t) k], juce::jlimit (0.0f, 1.0f, a));
                }
                i += hold;   // one attack at a time
            }
        }

        v.hopS = rm.hopS;
        v.j0 = floorDiv (s0, v.hopS);
        const int nF = floorDiv (s1, v.hopS) + 2 - v.j0;
        v.frames.assign ((size_t) nF, {});

        // Frames are independent: take them from the cache or analyse them on several threads.
        auto analyseFrame = [&v, &rm, fs] (int k)
        {
            const int j = v.j0 + k;
            if (j >= 0 && j < (int) rm.computed.size() && rm.computed[(size_t) j])
            {
                v.frames[(size_t) k] = rm.frames[(size_t) j];
                return;
            }
            const int c = j * v.hopS - v.s0;   // span-local centre
            if (c < 0 || c >= v.len) return;
            const bool cacheable = j >= 0 && j < (int) rm.computed.size();
            if (v.vw[(size_t) c] <= 0.0f)
            {
                if (cacheable) { rm.frames[(size_t) j] = {}; rm.computed[(size_t) j] = 1; }
                return;
            }
            const double f = v.f0[(size_t) c];
            const int K = std::max (1, (int) (std::min (maxHarmonicHz, 0.45 * fs) / f));
            const int Lw = ((int) (3.0 * fs / f)) | 1, h = Lw / 2;
            const int a = std::max (0, c - h), b = std::min (v.len, c + h + 1);
            if (b - a < 16) return;
            const bool clipped = a != c - h || b != c + h + 1;
            const int m = b - a;
            std::vector<float> w2 ((size_t) m), xs ((size_t) m), zr ((size_t) m), zi ((size_t) m);
            double sw = 0.0;
            for (int i = 0; i < m; ++i)
            {
                const int u = a + i - c;   // -h..h
                const double w = 0.5 - 0.5 * std::cos (twoPi * (u + h + 1) / (double) (Lw + 1));
                w2[(size_t) i] = (float) (w * w);
                sw += w * w;
                xs[(size_t) i] = v.x[(size_t) (a + i)];
                const double p = v.phi[(size_t) (a + i)];
                zr[(size_t) i] = (float) std::cos (p); zi[(size_t) i] = (float) std::sin (p);
            }
            // Harmonic phasors e^{j k phi}, harmonic-major (P[k*m + i]) so every loop below runs
            // over samples and vectorises.
            std::vector<float> Pr ((size_t) m * (size_t) K), Pi ((size_t) m * (size_t) K);
            std::copy (zr.begin(), zr.end(), Pr.begin());
            std::copy (zi.begin(), zi.end(), Pi.begin());
            for (int kk = 1; kk < K; ++kk)
            {
                const float* pr0 = Pr.data() + (size_t) (kk - 1) * (size_t) m;
                const float* pi0 = Pi.data() + (size_t) (kk - 1) * (size_t) m;
                float* pr1 = Pr.data() + (size_t) kk * (size_t) m;
                float* pi1 = Pi.data() + (size_t) kk * (size_t) m;
                for (int i = 0; i < m; ++i)
                {
                    pr1[i] = pr0[i] * zr[(size_t) i] - pi0[i] * zi[(size_t) i];
                    pi1[i] = pr0[i] * zi[(size_t) i] + pi0[i] * zr[(size_t) i];
                }
            }
            std::vector<float> cr ((size_t) K, 0.0f), ci ((size_t) K, 0.0f), wx ((size_t) m);
            const float g = (float) (2.0 / sw);
            // c_k += 2/sw * sum w^2 sig e^{-j k phi}
            auto project = [&] (const std::vector<float>& sig)
            {
                for (int i = 0; i < m; ++i) wx[(size_t) i] = w2[(size_t) i] * sig[(size_t) i];
                for (int kk = 0; kk < K; ++kk)
                {
                    const float* pr = Pr.data() + (size_t) kk * (size_t) m;
                    const float* pi_ = Pi.data() + (size_t) kk * (size_t) m;
                    float ar = 0.0f, ai = 0.0f;
                    for (int i = 0; i < m; ++i) { ar += wx[(size_t) i] * pr[i]; ai += wx[(size_t) i] * pi_[i]; }
                    cr[(size_t) kk] += g * ar; ci[(size_t) kk] -= g * ai;
                }
            };
            project (xs);
            // One refinement step = least squares (the basis is nearly orthogonal over 3 periods).
            std::vector<float> r (xs);
            for (int kk = 0; kk < K; ++kk)
            {
                const float* pr = Pr.data() + (size_t) kk * (size_t) m;
                const float* pi_ = Pi.data() + (size_t) kk * (size_t) m;
                const float a_ = cr[(size_t) kk], b_ = ci[(size_t) kk];
                for (int i = 0; i < m; ++i) r[(size_t) i] -= a_ * pr[i] - b_ * pi_[i];
            }
            project (r);
            HFrame fr;
            fr.f0 = (float) f;
            fr.c.resize ((size_t) K);
            for (int kk = 0; kk < K; ++kk) fr.c[(size_t) kk] = cf (cr[(size_t) kk], ci[(size_t) kk]);
            if (cacheable && ! clipped) { rm.frames[(size_t) j] = fr; rm.computed[(size_t) j] = 1; }
            v.frames[(size_t) k] = std::move (fr);
        };
        parallelFor (nF, analyseFrame);

        auto centre = [&v] (int k) { return (v.j0 + k) * v.hopS - v.s0; };

        // Fundamental phase offset (unwrapped over consecutive voiced frames), per sample.
        v.theta1.assign (L, 0.0);
        {
            std::vector<double> th ((size_t) nF, 0.0);
            std::vector<bool> ok ((size_t) nF, false);
            double prev = 0.0; bool have = false;
            for (int k = 0; k < nF; ++k)
            {
                const auto& fr = v.frames[(size_t) k];
                if (fr.c.empty()) continue;
                double t = std::arg (fr.c[0]);
                if (have) t = prev + std::remainder (t - prev, twoPi);
                th[(size_t) k] = prev = t; ok[(size_t) k] = have = true;
            }
            int last = -1;
            for (int k = 0; k < nF; ++k)
            {
                if (! ok[(size_t) k]) continue;
                const int c = centre (k);
                if (last < 0)
                    for (int i = 0; i < std::min (c, v.len); ++i) v.theta1[(size_t) i] = th[(size_t) k];
                else
                {
                    const int c0 = centre (last);
                    for (int i = std::max (0, c0); i < std::min (c, v.len); ++i)
                        v.theta1[(size_t) i] = th[(size_t) last] + (th[(size_t) k] - th[(size_t) last]) * (i - c0) / (double) (c - c0);
                }
                last = k;
            }
            if (last >= 0)
                for (int i = std::max (0, centre (last)); i < v.len; ++i) v.theta1[(size_t) i] = th[(size_t) last];
        }

        if (! wantHarmonics)
            return;

        // Harmonic model at the original pitch (coefficients interpolated between frames).
        v.harmonic.assign (L, 0.0f);
        parallelFor (nF - 1, [&v, &centre] (int k)
        {
            const auto& A = v.frames[(size_t) k].c;
            const auto& B = v.frames[(size_t) k + 1].c;
            if (A.empty() && B.empty()) return;
            const int c0 = centre (k);
            const int i0 = std::max (0, c0), i1 = std::min (v.len, c0 + v.hopS);
            if (i1 <= i0) return;
            std::vector<float> tt ((size_t) (i1 - i0));
            for (int i = i0; i < i1; ++i) tt[(size_t) (i - i0)] = (float) (i - c0) / (float) v.hopS;
            addHarmonics (v.phi.data() + i0, tt.data(), i1 - i0, A, B, v.harmonic.data() + i0);
        });
    }

    // Least-squares fit (projection + one refinement) of harmonics locked to `phi` around sample c
    // of x (3 periods of f, Hann window clipped to [0, len)).
    HFrame fitHarmonics (const float* x, const double* phi, int len, int c, double f, double fs)
    {
        HFrame fr;
        if (f <= 0.0) return fr;
        const int K = std::max (1, (int) (std::min (maxHarmonicHz, 0.45 * fs) / f));
        const int Lw = ((int) (3.0 * fs / f)) | 1, h = Lw / 2;
        const int a = std::max (0, c - h), b = std::min (len, c + h + 1);
        if (b - a < 16) return fr;
        const int m = b - a;
        std::vector<float> w2 ((size_t) m), xs ((size_t) m), zr ((size_t) m), zi ((size_t) m);
        double sw = 0.0;
        for (int i = 0; i < m; ++i)
        {
            const int u = a + i - c;
            const double w = 0.5 - 0.5 * std::cos (twoPi * (u + h + 1) / (double) (Lw + 1));
            w2[(size_t) i] = (float) (w * w);
            sw += w * w;
            xs[(size_t) i] = x[a + i];
            zr[(size_t) i] = (float) std::cos (phi[a + i]); zi[(size_t) i] = (float) std::sin (phi[a + i]);
        }
        std::vector<float> Pr ((size_t) m * (size_t) K), Pi ((size_t) m * (size_t) K);
        std::copy (zr.begin(), zr.end(), Pr.begin());
        std::copy (zi.begin(), zi.end(), Pi.begin());
        for (int k = 1; k < K; ++k)
            for (int i = 0; i < m; ++i)
            {
                const float r0 = Pr[(size_t) (k - 1) * (size_t) m + (size_t) i], i0 = Pi[(size_t) (k - 1) * (size_t) m + (size_t) i];
                Pr[(size_t) k * (size_t) m + (size_t) i] = r0 * zr[(size_t) i] - i0 * zi[(size_t) i];
                Pi[(size_t) k * (size_t) m + (size_t) i] = r0 * zi[(size_t) i] + i0 * zr[(size_t) i];
            }
        std::vector<float> cr ((size_t) K, 0.0f), ci ((size_t) K, 0.0f), wx ((size_t) m);
        const float g = (float) (2.0 / sw);
        auto project = [&] (const std::vector<float>& sig)
        {
            for (int i = 0; i < m; ++i) wx[(size_t) i] = w2[(size_t) i] * sig[(size_t) i];
            for (int k = 0; k < K; ++k)
            {
                float ar = 0.0f, ai = 0.0f;
                const float* pr = Pr.data() + (size_t) k * (size_t) m;
                const float* pi_ = Pi.data() + (size_t) k * (size_t) m;
                for (int i = 0; i < m; ++i) { ar += wx[(size_t) i] * pr[i]; ai += wx[(size_t) i] * pi_[i]; }
                cr[(size_t) k] += g * ar; ci[(size_t) k] -= g * ai;
            }
        };
        project (xs);
        std::vector<float> r (xs);
        for (int k = 0; k < K; ++k)
        {
            const float* pr = Pr.data() + (size_t) k * (size_t) m;
            const float* pi_ = Pi.data() + (size_t) k * (size_t) m;
            for (int i = 0; i < m; ++i) r[(size_t) i] -= cr[(size_t) k] * pr[i] - ci[(size_t) k] * pi_[i];
        }
        project (r);
        fr.f0 = (float) f;
        fr.c.resize ((size_t) K);
        for (int k = 0; k < K; ++k) fr.c[(size_t) k] = cf (cr[(size_t) k], ci[(size_t) k]);
        return fr;
    }

    // Harmonic coefficients at a new fundamental: the spectral envelope (log-amplitude linear in
    // frequency, phase by phasor interpolation) is sampled at the new harmonic frequencies
    // divided by the formant ratio; the total power of the frame is kept.
    void resampleEnvelope (const VoiceSpan::Frame& fr, double ratio, double rf, double fs, std::vector<cf>& out)
    {
        out.clear();
        const auto& c = fr.c;
        if (c.empty()) return;
        if (std::abs (ratio - 1.0) < 1.0e-9 && std::abs (rf - 1.0) < 1.0e-9) { out = c; return; }
        const int K = (int) c.size();
        const double f0 = fr.f0, fn = f0 * ratio;
        const int Kn = std::max (1, (int) (std::min (maxHarmonicHz, 0.45 * fs) / fn));
        out.resize ((size_t) Kn);
        double p0 = 0.0, p1 = 0.0;
        for (const auto& v : c) p0 += std::norm (v);
        for (int j = 0; j < Kn; ++j)
        {
            const double pos = fn * (j + 1) / rf / f0 - 1.0;   // fractional harmonic index of the envelope
            double la, ur, ui;
            if (pos <= 0.0)
            {
                la = std::log (std::abs (c[0]) + 1.0e-9);
                ur = std::cos (std::arg (c[0])); ui = std::sin (std::arg (c[0]));
            }
            else if (pos >= K - 1)
            {
                la = std::log (std::abs (c[(size_t) K - 1]) + 1.0e-9) - (pos - (K - 1)) * f0 / 1000.0 * 2.3;
                ur = std::cos (std::arg (c[(size_t) K - 1])); ui = std::sin (std::arg (c[(size_t) K - 1]));
            }
            else
            {
                const int i = (int) pos; const double t = pos - i;
                const double l0 = std::log (std::abs (c[(size_t) i]) + 1.0e-9), l1 = std::log (std::abs (c[(size_t) i + 1]) + 1.0e-9);
                la = l0 + (l1 - l0) * t;
                const double a0 = std::arg (c[(size_t) i]), a1 = std::arg (c[(size_t) i + 1]);
                ur = (1 - t) * std::cos (a0) + t * std::cos (a1);
                ui = (1 - t) * std::sin (a0) + t * std::sin (a1);
            }
            const double ph = std::atan2 (ui, ur);
            const double amp = std::exp (la);
            out[(size_t) j] = cf ((float) (amp * std::cos (ph)), (float) (amp * std::sin (ph)));
            p1 += amp * amp;
        }
        if (p1 > 0.0)
        {
            const float g = (float) std::sqrt (p0 / p1);
            for (auto& v : out) v *= g;
        }
    }
}

bool VocalRenderer::needsRender (const std::vector<Note>& notes, const RenderOptions& o)
{
    for (const auto& n : notes)
    {
        if (affectsPitch (n, o) || (n.isNote() && std::abs (n.formantCents) > 0.01f) || std::abs (n.gainDb) > 0.001f)
            return true;
        if (o.time && n.hasTimeEdit())
            return true;
        if (o.eq && n.eq.isActive())
            return true;
    }
    return false;
}

std::vector<float> VocalRenderer::notePitchCurve (const Note& n, const AnalysisResult& a, const RenderOptions& o)
{
    int f0 = 0;
    const auto sm = smoothedPitch (n, a, f0);
    std::vector<float> out (sm.size(), kNaN);
    if (! n.isNote())
        return out;
    const float drift = n.pitchDrift * (1.0f - ((o.pitch && o.scaleCorrection) ? n.correctionDrift : 0.0f));
    const float mod = n.pitchModulation;
    const float offset = offsetCents (n, n.outEnd(), o) / 100.0f;   // fully applied correction
    for (size_t i = 0; i < sm.size(); ++i)
    {
        const auto& f = a.frames[(size_t) f0 + i];
        if (f.cls != FrameClass::Voiced || ! std::isfinite (f.pitch) || ! std::isfinite (sm[i]))
            continue;
        float p = f.pitch;
        if (o.pitch)
        {
            p = n.detectedPitch + drift * (sm[i] - n.detectedPitch) + mod * (f.pitch - sm[i]);
            const double frac = sm.size() > 1 ? (double) i / (double) (sm.size() - 1) : 0.5;
            p += (float) ((frac - 0.5) * n.pitchTilt / 100.0);
        }
        out[i] = p + offset;
        if (o.pitch && ! n.drawnCents.empty())
        {
            const double ts = (double) (f0 + (int) i) * a.hopSeconds;
            const float d = n.drawnAt (ts);
            if (std::isfinite (d))
            {
                const float w = n.drawnWeight (ts);
                out[i] += (n.detectedPitch + offset + d / 100.0f - out[i]) * w;
            }
        }
    }
    return out;
}

std::shared_ptr<RenderedAudio> VocalRenderer::render (const TakeAudio& take, const AnalysisResult& analysis,
                                                      const std::vector<Note>& notesIn, const RenderOptions& options,
                                                      const std::atomic<bool>* cancel, VoiceModelCache* cache)
{
    VoiceModelCache localCache;
    if (cache == nullptr) cache = &localCache;
    auto& cimpl = *cache->impl;
    if (cimpl.takeKey != (const void*) &take || cimpl.analysisFrames != analysis.frames.size()
        || ! juce::exactlyEqual (cimpl.fs, take.getSampleRate()))
    {
        cimpl.ranges.clear();
        cimpl.regions.clear();
        cimpl.regionSamples = 0;
        cimpl.takeKey = &take;
        cimpl.analysisFrames = analysis.frames.size();
        cimpl.fs = take.getSampleRate();
    }

    if (! needsRender (notesIn, options) || analysis.frames.empty())
        return nullptr;

    const double fs = take.getSampleRate();
    const double hop = analysis.hopSeconds;
    auto result = std::make_shared<RenderedAudio>();
    result->sampleRate = fs;
    result->ranges = take.getRanges();
    result->samples.assign ((size_t) take.getEndSample(), 0.0f);

    std::vector<Note> notes = notesIn;
    std::sort (notes.begin(), notes.end(), [] (const Note& x, const Note& y) { return x.startSeconds < y.startSeconds; });

    // Per-frame vibrato-free pitch for every note (source frame index -> value).
    std::vector<float> smoothAll (analysis.frames.size(), kNaN);
    for (const auto& n : notes)
    {
        int f0 = 0;
        const auto sm = smoothedPitch (n, analysis, f0);
        for (size_t i = 0; i < sm.size(); ++i)
            smoothAll[(size_t) f0 + i] = sm[i];
    }

    const FrameTrack track (analysis);

    for (const auto& range : result->ranges)
    {
        if (cancel != nullptr && cancel->load())
            return nullptr;

        const juce::int64 rs = range.start;
        const int n = (int) range.length();
        std::vector<float> src ((size_t) n);
        take.read (rs, src.data(), n);
        float* out = result->samples.data() + rs;
        std::copy (src.begin(), src.end(), out);

        const double rsSec = (double) rs / fs, reSec = (double) range.end / fs;
        const TimeMap map = options.time ? TimeMap::build (notes, &analysis, rsSec, reSec, options.protectConsonants)
                                         : TimeMap();

        // Notes relevant to this range, ordered by output start. Breaths / consonants (rnNoise) only
        // carry a level; they never take part in pitch.
        std::vector<const Note*> rn, rnNoise;
        for (const auto& nt : notes)
            if (std::max (nt.endSeconds, nt.outEnd()) > rsSec && std::min (nt.startSeconds, nt.outStart()) < reSec)
                (nt.isNote() ? rn : rnNoise).push_back (&nt);
        std::sort (rn.begin(), rn.end(), [] (const Note* x, const Note* y) { return x->outStart() < y->outStart(); });

        // --- Control curves on the output frame grid --------------------------------
        const int k0 = (int) std::floor (rsSec / hop) - 1;
        const int K = (int) std::ceil (reSec / hop) + 2 - k0;
        std::vector<float> offset ((size_t) K, 0.0f), extra ((size_t) K, 0.0f), formant ((size_t) K, 0.0f),
                           gain ((size_t) K, 0.0f), deviation ((size_t) K, 0.0f);
        // Pencil: shift that puts the voice exactly on the drawn pitch, and its weight (0 = none).
        std::vector<float> drawShift ((size_t) K, 0.0f), drawW ((size_t) K, 0.0f);

        for (int k = 0; k < K; ++k)
        {
            const double t = (k0 + k) * hop;
            const double ts = map.toSource (t);
            deviation[(size_t) k] = (float) (std::abs (ts - t) * fs + std::abs (map.rateAt (t) - 1.0) * 1.0e6);

            // Owner note (output time) or neighbours.
            auto it = std::upper_bound (rn.begin(), rn.end(), t, [] (double v, const Note* x) { return v < x->outStart(); });
            const Note* owner = nullptr;
            const Note* prev = nullptr;
            const Note* next = it != rn.end() ? *it : nullptr;
            if (it != rn.begin())
            {
                const Note* cand = *(it - 1);
                if (t < cand->outEnd()) owner = cand; else prev = cand;
            }

            if (owner != nullptr)
            {
                offset[(size_t) k] = offsetCents (*owner, t, options);
                formant[(size_t) k] = owner->formantCents;
                gain[(size_t) k] = owner->gainDb;

                const float drift = owner->pitchDrift * (1.0f - ((options.pitch && options.scaleCorrection) ? owner->correctionDrift : 0.0f));
                const float mod = owner->pitchModulation;
                if (options.pitch && ! owner->drawnCents.empty())
                {
                    const float d = owner->drawnAt (ts);
                    const double tl = ts;
                    const int fa = (int) std::floor (tl / analysis.hopSeconds);
                    if (std::isfinite (d) && fa >= 0 && fa + 1 < (int) analysis.frames.size())
                    {
                        const auto& f = analysis.frames[(size_t) fa];
                        const auto& g2 = analysis.frames[(size_t) fa + 1];
                        if (f.cls == FrameClass::Voiced && std::isfinite (f.pitch) && g2.cls == FrameClass::Voiced && std::isfinite (g2.pitch))
                        {
                            const float x = (float) (tl / analysis.hopSeconds - fa);
                            const float sung = f.pitch + (g2.pitch - f.pitch) * x;   // sung pitch at ts
                            // Target = note centre (with its move) + drawn cents; shift = target - sung.
                            drawShift[(size_t) k] = offset[(size_t) k] + d - 100.0f * (sung - owner->detectedPitch);
                            drawW[(size_t) k] = owner->drawnWeight (ts);
                        }
                    }
                }
                if (options.pitch && std::abs (owner->pitchTilt) > 0.01f)
                {
                    // Tilt: a straight slope across the note, faded in/out over 30 ms at its edges.
                    const double len = std::max (1.0e-3, owner->outEnd() - owner->outStart());
                    const double frac = (t - owner->outStart()) / len;
                    const double edge = std::min ({ 1.0, (t - owner->outStart()) / 0.03, (owner->outEnd() - t) / 0.03 });
                    extra[(size_t) k] += (float) ((frac - 0.5) * owner->pitchTilt * std::max (0.0, edge));
                }
                if (options.pitch && (std::abs (drift - 1.0f) > 0.001f || std::abs (mod - 1.0f) > 0.001f))
                {
                    const int fi = analysis.frameIndexAt (ts);
                    if (fi >= 0 && fi < (int) analysis.frames.size())
                    {
                        const auto& f = analysis.frames[(size_t) fi];
                        const float sm = smoothAll[(size_t) fi];
                        if (f.cls == FrameClass::Voiced && std::isfinite (f.pitch) && std::isfinite (sm))
                        {
                            const double edge = std::min ({ 1.0, (t - owner->outStart()) / 0.03, (owner->outEnd() - t) / 0.03 });
                            const float e = (drift - 1.0f) * (sm - owner->detectedPitch) + (mod - 1.0f) * (f.pitch - sm);
                            extra[(size_t) k] += (float) (100.0 * e * std::max (0.0, edge));
                        }
                    }
                }
            }
            else
            {
                // Between notes: glide from the previous note's offset to the next one's.
                auto blend = [&] (float a, float b)
                {
                    if (prev == nullptr && next == nullptr) return 0.0f;
                    if (prev == nullptr) return (float) (b * std::max (0.0, 1.0 - (next->outStart() - t) / 0.3));
                    if (next == nullptr) return (float) (a * std::max (0.0, 1.0 - (t - prev->outEnd()) / 0.3));
                    const double gap = next->outStart() - prev->outEnd();
                    if (gap <= 0.6)
                    {
                        const double x = gap > 0.0 ? (t - prev->outEnd()) / gap : 0.5;
                        return (float) (a + (b - a) * x);
                    }
                    return (float) (a * std::max (0.0, 1.0 - (t - prev->outEnd()) / 0.3)
                                  + b * std::max (0.0, 1.0 - (next->outStart() - t) / 0.3));
                };
                offset[(size_t) k] = blend (prev ? offsetCents (*prev, prev->outEnd(), options) : 0.0f,
                                            next ? offsetCents (*next, next->outStart(), options) : 0.0f);
                formant[(size_t) k] = blend (prev ? prev->formantCents : 0.0f, next ? next->formantCents : 0.0f);
            }
        }

        // Note-to-note transitions: a raised-cosine glide centred between two neighbouring notes.
        // Length = the later note's transition (default 45 ms, the natural speed of a singer's
        // glide; the sung glide itself is kept, the offset only moves along with it).
        for (size_t i = 1; i < rn.size(); ++i)
        {
            const Note* a = rn[i - 1];
            const Note* b = rn[i];
            const double gap = b->outStart() - a->outEnd();
            if (gap > 0.6)
                continue;
            const double T = (b->transitionMs >= 0.0f ? b->transitionMs : 45.0f) / 1000.0;
            const double c = 0.5 * (a->outEnd() + b->outStart());
            const double h = std::max ({ 0.5 * T, 0.5 * std::max (0.0, gap), 0.5 * hop });
            const float va = offsetCents (*a, a->outEnd(), options), vb = offsetCents (*b, b->outStart(), options);
            const int ka = std::max (0, (int) std::floor ((c - h) / hop) - k0), kb = std::min (K - 1, (int) std::ceil ((c + h) / hop) - k0);
            for (int k = ka; k <= kb; ++k)
            {
                const double x = juce::jlimit (0.0, 1.0, ((k0 + k) * hop - (c - h)) / (2.0 * h));
                offset[(size_t) k] = (float) (va + (vb - va) * (0.5 - 0.5 * std::cos (pi * x)));
            }
        }
        offset = movingAverage (offset, 3);
        formant = movingAverage (formant, 9);

        // Breaths / consonants: their own level (silent at -60 dB).
        for (const Note* nz : rnNoise)
        {
            if (std::abs (nz->gainDb) < 0.001f)
                continue;
            const int ka = std::max (0, (int) std::floor (nz->outStart() / hop) - k0);
            const int kb = std::min (K - 1, (int) std::ceil (nz->outEnd() / hop) - k0);
            for (int k = ka; k <= kb; ++k)
                gain[(size_t) k] = nz->gainDb;
        }
        gain = movingAverage (gain, 3);   // ~15 ms fades: no clicks

        std::vector<float> shift ((size_t) K), formantTotal ((size_t) K);
        std::vector<bool> active ((size_t) K, false);
        bool anyActive = false, anyGain = false;
        for (int k = 0; k < K; ++k)
        {
            shift[(size_t) k] = offset[(size_t) k] + extra[(size_t) k];
            if (drawW[(size_t) k] > 0.0f)
                shift[(size_t) k] += (drawShift[(size_t) k] - shift[(size_t) k]) * drawW[(size_t) k];
            formantTotal[(size_t) k] = formant[(size_t) k] + (options.preserveFormants ? 0.0f : shift[(size_t) k]);
            active[(size_t) k] = std::abs (shift[(size_t) k]) > 0.5f || std::abs (formantTotal[(size_t) k]) > 0.5f
                              || deviation[(size_t) k] > 0.5f;
            anyActive = anyActive || active[(size_t) k];
            anyGain = anyGain || std::abs (gain[(size_t) k]) > 0.001f;
        }

        // --- Phase offset of the edited voice (frame grid) ------------------------------
        // The edited voice runs ahead of / behind the original's fundamental phase. Outside
        // edits that difference is drained to a whole cycle (<= 0.4 % of the frequency, inaudible)
        // and reset in unvoiced gaps; until then the region stays active.
        std::vector<bool> active2 = active;
        if (anyActive)
        {
            constexpr double drain = 0.004;
            double D = 0.0;
            for (int k = 0; k < K; ++k)
            {
                const double t = (k0 + k) * hop;
                const double ts = map.toSource (t);
                const double f = track.f0At (ts);
                const bool voiced = track.vwAt (ts) > 0.0f;
                const double ratio = std::pow (2.0, shift[(size_t) k] / 1200.0);
                const double rate = map.rateAt (t);
                if (active[(size_t) k]) D += twoPi * f * (ratio - rate) * hop;
                else if (! voiced) D = 0.0;
                else
                {
                    const double tgt = std::round (D / twoPi) * twoPi, step = twoPi * f * drain * hop;
                    D = std::abs (D - tgt) <= step ? 0.0 : D + (tgt > D ? step : -step);
                    if (std::abs (D) > 0.0) active2[(size_t) k] = true;
                }
            }
        }

        // --- Engines on the active regions ------------------------------------------------
        if (anyActive)
        {
            auto framePos = [&] (double localSample) { return ((double) rs + localSample) / fs / hop - k0; };

            // Active intervals (local samples), dilated so that every splice lies in unedited audio.
            std::vector<std::pair<int, int>> regions;
            const int dilate = (int) (0.03 * fs);
            for (int k = 0; k < K; ++k)
            {
                if (! active2[(size_t) k]) continue;
                int ra = (int) std::lround (((k0 + k) - 0.5) * hop * fs) - (int) rs - dilate;
                int rb = (int) std::lround (((k0 + k) + 0.5) * hop * fs) - (int) rs + dilate;
                ra = std::max (0, ra); rb = std::min (n, rb);
                if (rb <= ra) continue;
                if (! regions.empty() && ra <= regions.back().second) regions.back().second = std::max (regions.back().second, rb);
                else regions.emplace_back (ra, rb);
            }
            std::vector<float> editMask ((size_t) K);
            for (int k = 0; k < K; ++k) editMask[(size_t) k] = active2[(size_t) k] ? 1.0f : 0.0f;

            const int fade = std::max (1, (int) (0.010 * fs));
            const bool clean = options.engine != Engine::trueVoice;   // needs the harmonic model
            const int H = std::max (8, (int) std::lround (0.005 * fs));   // unvoiced grain hop

            // Renders output samples [A, B) of an active region (with everything it needs around
            // it) -> y. Results are cached by a hash of their inputs.
            auto renderPiece = [&] (const int A, const int B) -> std::vector<float>
            {
                const int len = B - A;

                // Same inputs as an earlier render -> reuse it. The key covers everything the
                // per-sample controls are interpolated from (frame-grid curves + time map), so an
                // unchanged phrase costs almost nothing.
                juce::uint64 key = 1469598103934665603ULL;
                auto mix = [&key] (const void* data, size_t bytes)
                {
                    const auto* p = static_cast<const juce::uint8*> (data);
                    for (size_t b = 0; b < bytes; ++b) { key ^= p[b]; key *= 1099511628211ULL; }
                };
                {
                    const juce::int64 hdr[] = { rs, (juce::int64) A, (juce::int64) B, (juce::int64) options.engine,
                                                (juce::int64) options.protectConsonants };
                    mix (hdr, sizeof (hdr));
                    const int k0f = std::max (0, (int) std::floor (framePos (A)) - 1);
                    const int k1f = std::min (K - 1, (int) std::ceil (framePos (B)) + 1);
                    for (int k = k0f; k <= k1f; ++k)
                    {
                        const float vals[] = { shift[(size_t) k], formantTotal[(size_t) k], editMask[(size_t) k] };
                        mix (vals, sizeof (vals));
                        const double ts = map.toSource ((k0 + k) * hop);
                        mix (&ts, sizeof (ts));
                    }
                    // Time map breakpoints inside the piece (the map is piecewise linear).
                    const double t0s = ((double) rs + A) / fs, t1s = ((double) rs + B) / fs;
                    for (double t = t0s; t <= t1s; t += hop * 0.25)
                    {
                        const double ts = map.toSource (t);
                        mix (&ts, sizeof (ts));
                    }
                }
                if (auto it = cimpl.regions.find (key); it != cimpl.regions.end() && it->second.y.size() == (size_t) len)
                {
                    it->second.lastUse = ++cimpl.useCounter;
                    return it->second.y;
                }

                // Per output sample: source position, pitch ratio, formant ratio, edit weight.
                std::vector<double> sPos ((size_t) len);
                std::vector<float> ratioV ((size_t) len), rfV ((size_t) len), editV ((size_t) len);
                for (int i = 0; i < len; ++i)
                {
                    const double t = A + i;
                    sPos[(size_t) i] = map.toSource (((double) rs + t) / fs) * fs - (double) rs;
                    const double fp = framePos (t);
                    ratioV[(size_t) i] = (float) std::pow (2.0, interp (shift, fp) / 1200.0);
                    rfV[(size_t) i] = (float) std::pow (2.0, interp (formantTotal, fp) / 1200.0);
                    editV[(size_t) i] = interp (editMask, fp);
                }

                const double sMin = std::min (sPos.front(), (double) A), sMax = std::max (sPos.back(), (double) B);
                const int margin = (int) (0.06 * fs);
                VoiceSpan v;
                auto& rm = cimpl.ranges[rs];
                prepareRange (rm, track, rs, n, fs);
                buildSpan (v, src, track, rs, (int) std::floor (sMin) - margin, (int) std::ceil (sMax) + margin, fs, clean, rm);


                // 2) Phase offset per sample (same rule as on the frame grid).
                std::vector<double> Dv ((size_t) len, 0.0);
                {
                    double D = 0.0;
                    for (int i = 0; i < len; ++i)
                    {
                        const double s = sPos[(size_t) i];
                        const double f = v.f0At (s);
                        const double rate = i + 1 < len ? sPos[(size_t) i + 1] - s : (i > 0 ? s - sPos[(size_t) i - 1] : 1.0);
                        const bool voiced = v.vwAt (s) > 0.0;
                        if (editV[(size_t) i] > 0.0f && (std::abs (ratioV[(size_t) i] - 1.0f) > 1.0e-7f || std::abs (rate - 1.0) > 1.0e-9 || std::abs (D) > 0.0))
                        {
                            const bool editing = std::abs (ratioV[(size_t) i] - 1.0f) > 1.0e-7f || std::abs (rate - 1.0) > 1.0e-9;
                            if (editing) D += twoPi * f * (ratioV[(size_t) i] - rate) / fs;
                            else if (! voiced) D = 0.0;
                            else
                            {
                                const double tgt = std::round (D / twoPi) * twoPi, step = twoPi * f * 0.004 / fs;
                                D = std::abs (D - tgt) <= step ? 0.0 : D + (tgt > D ? step : -step);
                            }
                        }
                        else if (! voiced) D = 0.0;
                        Dv[(size_t) i] = D;
                    }
                }


                // PSOLA path for one signal (the voice or its residual): time-mapped baseline plus
                // phase-locked PSOLA where the voice is edited. gOut = edit weight (5 ms smoothed).
                auto psolaPath = [&] (const std::vector<float>& in, std::vector<float>& gOut) -> std::vector<float>
                {
                    auto inAt = [&] (long s) -> float { const long i = s - v.s0; return (i >= 0 && i < v.len) ? in[(size_t) i] : 0.0f; };
                    auto inCubic = [&] (double s) -> float { return cubicAt (in, s - v.s0); };

                    // 1) Baseline: time-mapped copy (plain overlap-add, sums to exactly 1; exact copy at rate 1).
                    std::vector<float> base ((size_t) len, 0.0f);
                    for (int c = -H; c < len + H; c += H)
                    {
                        const int ci = juce::jlimit (0, len - 1, c);
                        const double sc = sPos[(size_t) ci] + (c - ci);
                        const long S = std::lround (sc);
                        const double rfU = options.protectConsonants ? 1.0 : (double) ratioV[(size_t) ci];
                        const bool exact = std::abs (rfU - 1.0) < 1.0e-6;
                        for (int u = -H + 1; u < H; ++u)
                        {
                            const int idx = c + u;
                            if (idx < 0 || idx >= len) continue;
                            const float w = (float) (0.5 + 0.5 * std::cos (pi * u / H));
                            base[(size_t) idx] += w * (exact ? inAt (S + u) : inCubic ((double) S + u * rfU));
                        }
                    }

                    // 3) Phase-locked PSOLA (voiced, edited parts).
                    std::vector<float> acc ((size_t) len, 0.0f), ws ((size_t) len, 0.0f);
                    {
                        std::vector<int> marks;   // one per period where phi + theta1 crosses a whole cycle
                        for (int i = 1; i < v.len; ++i)
                        {
                            if (v.vw[(size_t) i] <= 0.0f) continue;
                            const double p0 = v.phi[(size_t) i - 1] + v.theta1[(size_t) i - 1], p1 = v.phi[(size_t) i] + v.theta1[(size_t) i];
                            if (std::floor (p1 / twoPi) > std::floor (p0 / twoPi)) marks.push_back (v.s0 + i);
                        }
                        double prevPsi = 0.0;
                        size_t mi = 0;
                        for (int i = 0; i < len; ++i)
                        {
                            const double s = sPos[(size_t) i];
                            const double psi = v.phiAt (s) + Dv[(size_t) i] + v.theta1At (s);
                            const bool cross = i > 0 && std::floor (psi / twoPi) > std::floor (prevPsi / twoPi);
                            prevPsi = psi;
                            if (! cross || marks.empty() || editV[(size_t) i] <= 0.0f || v.vwAt (s) <= 0.0) continue;
                            while (mi + 1 < marks.size() && std::abs (marks[mi + 1] - s) <= std::abs (marks[mi] - s)) ++mi;
                            while (mi > 0 && std::abs (marks[mi - 1] - s) < std::abs (marks[mi] - s)) --mi;
                            const int m = marks[mi];
                            const double P = fs / v.f0At (m), Ps = fs / (v.f0At (s) * ratioV[(size_t) i]);
                            if (std::abs (m - s) > 1.5 * P) continue;
                            const double rf = rfV[(size_t) i];
                            const double half = std::max (P / rf, Ps);
                            const int hi = (int) std::ceil (half);
                            const bool exact = std::abs (rf - 1.0) < 1.0e-6;
                            for (int u = -hi + 1; u < hi; ++u)
                            {
                                const int idx = i + u;
                                if (idx < 0 || idx >= len) continue;
                                const float w = (float) (0.5 + 0.5 * std::cos (pi * u / half));
                                if (w <= 0.0f) continue;
                                acc[(size_t) idx] += w * (exact ? inAt (m + u) : inCubic (m + u * rf));
                                ws[(size_t) idx] += w;
                            }
                        }
                    }
                    gOut.assign ((size_t) len, 0.0f);
                    for (int i = 0; i < len; ++i)
                        gOut[(size_t) i] = juce::jlimit (0.0f, 1.0f, ws[(size_t) i] / 0.5f) * (float) v.vwAt (sPos[(size_t) i]) * (editV[(size_t) i] > 0.0f ? 1.0f : 0.0f);
                    gOut = movingAverage (gOut, std::max (1, (int) (0.005 * fs)) | 1);
                    std::vector<float> y ((size_t) len);
                    for (int i = 0; i < len; ++i)
                    {
                        const float p = ws[(size_t) i] > 1.0e-3f ? acc[(size_t) i] / std::max (ws[(size_t) i], 0.5f) : 0.0f;
                        y[(size_t) i] = base[(size_t) i] + gOut[(size_t) i] * (p - base[(size_t) i]);
                    }
                    return y;
                };

                // Harmony Clean: harmonics re-synthesised at the new pitch. Each analysis frame is
                // resampled once (at the pitch/formant ratio where it is first used), so the
                // coefficients are continuous from frame to frame.
                auto addShiftedHarmonics = [&] (std::vector<float>& y)
                {
                    const int nF = (int) v.frames.size();
                    struct Run { int i0, i1, j; };   // output samples using the same pair of frames
                    std::vector<Run> runs;
                    std::vector<double> phase ((size_t) len);
                    std::vector<float> tts ((size_t) len);
                    for (int i = 0; i < len; ++i)
                    {
                        const double s = sPos[(size_t) i];
                        const double q = s / v.hopS - v.j0;
                        if (q < 0.0 || q >= nF - 1) continue;
                        const int j = (int) q;
                        tts[(size_t) i] = (float) (q - j);
                        phase[(size_t) i] = v.phiAt (s) + Dv[(size_t) i];
                        if (! runs.empty() && runs.back().j == j && runs.back().i1 == i) runs.back().i1 = i + 1;
                        else runs.push_back ({ i, i + 1, j });
                    }
                    std::vector<std::vector<cf>> res ((size_t) nF);
                    std::vector<bool> done ((size_t) nF, false);
                    for (const auto& r : runs)
                        for (int j : { r.j, r.j + 1 })
                            if (! done[(size_t) j])
                            {
                                resampleEnvelope (v.frames[(size_t) j], ratioV[(size_t) r.i0], rfV[(size_t) r.i0], fs, res[(size_t) j]);
                                done[(size_t) j] = true;
                            }
                    parallelFor ((int) runs.size(), [&] (int ri)
                    {
                        const auto& r = runs[(size_t) ri];
                        addHarmonics (phase.data() + r.i0, tts.data() + r.i0, r.i1 - r.i0,
                                      res[(size_t) r.j], res[(size_t) r.j + 1], y.data() + r.i0);
                    });
                };

                // The level follows the original (30 ms RMS, edited parts only).
                auto matchLoudness = [&] (std::vector<float>& y)
                {
                    const int win = std::max (8, (int) (0.015 * fs));
                    std::vector<double> sqY ((size_t) len + 1, 0.0), sqX ((size_t) v.len + 1, 0.0);
                    for (int i = 0; i < len; ++i) sqY[(size_t) i + 1] = sqY[(size_t) i] + (double) y[(size_t) i] * y[(size_t) i];
                    for (int i = 0; i < v.len; ++i) sqX[(size_t) i + 1] = sqX[(size_t) i] + (double) v.x[(size_t) i] * v.x[(size_t) i];
                    auto rms = [] (const std::vector<double>& sq, long c, int w)
                    {
                        const long lo = std::max (0L, c - w), hi = std::min ((long) sq.size() - 1, c + w);
                        return hi > lo ? std::sqrt ((sq[(size_t) hi] - sq[(size_t) lo]) / (double) (hi - lo)) : 0.0;
                    };
                    const int step = std::max (1, (int) (0.005 * fs));
                    std::vector<float> gg;
                    for (int i = 0; i < len; i += step)
                    {
                        float val = 1.0f;
                        if (editV[(size_t) i] > 0.0f)
                        {
                            const double ry = rms (sqY, i, win), rx = rms (sqX, std::lround (sPos[(size_t) i]) - v.s0, win);
                            if (ry > 1.0e-7 && rx > 1.0e-7) val = (float) juce::jlimit (0.5, 2.0, rx / ry);
                        }
                        gg.push_back (val);
                    }
                    gg = movingAverage (gg, 11);   // ~50 ms
                    for (int i = 0; i < len; ++i)
                        y[(size_t) i] *= interp (gg, (double) i / step);
                };

                std::vector<float> y, gEdit;
                if (options.engine == Engine::trueVoice)
                {
                    y = psolaPath (v.x, gEdit);
                    matchLoudness (y);
                }
                else
                {
                    std::vector<float> residual ((size_t) v.len);
                    for (int i = 0; i < v.len; ++i)
                        residual[(size_t) i] = v.x[(size_t) i] - v.harmonic[(size_t) i];
                    const auto resMoved = psolaPath (residual, gEdit);
                    y = resMoved;
                    addShiftedHarmonics (y);

                    if (options.engine == Engine::hybrid)
                    {
                        // Hybrid: the True Voice result (natural waveform), cleaned by keeping only its
                        // harmonics at the new pitch, plus the original breath / noise layer moved with it.
                        std::vector<float> gT;
                        const auto yT = psolaPath (v.x, gT);
                        std::vector<double> phaseOut ((size_t) len);
                        std::vector<float> f0Out ((size_t) len), vwOut ((size_t) len);
                        for (int i = 0; i < len; ++i)
                        {
                            const double s = sPos[(size_t) i];
                            phaseOut[(size_t) i] = v.phiAt (s) + Dv[(size_t) i];
                            f0Out[(size_t) i] = (float) (v.f0At (s) * ratioV[(size_t) i]);
                            vwOut[(size_t) i] = gT[(size_t) i];
                        }
                        const int hopO = v.hopS;
                        const int nO = len / hopO + 2;
                        std::vector<HFrame> fr ((size_t) nO);
                        parallelFor (nO, [&] (int j)
                        {
                            const int c = j * hopO;
                            if (c < len && vwOut[(size_t) c] > 0.01f)
                                fr[(size_t) j] = fitHarmonics (yT.data(), phaseOut.data(), len, c, f0Out[(size_t) c], fs);
                        });
                        std::vector<float> hT ((size_t) len, 0.0f);
                        parallelFor (nO - 1, [&] (int j)
                        {
                            const int c0 = j * hopO, c1 = std::min (len, c0 + hopO);
                            if (c1 <= c0 || (fr[(size_t) j].c.empty() && fr[(size_t) j + 1].c.empty())) return;
                            std::vector<float> tt ((size_t) (c1 - c0));
                            for (int i = c0; i < c1; ++i) tt[(size_t) (i - c0)] = (float) (i - c0) / (float) hopO;
                            addHarmonics (phaseOut.data() + c0, tt.data(), c1 - c0, fr[(size_t) j].c, fr[(size_t) j + 1].c, hT.data() + c0);
                        });
                        for (int i = 0; i < len; ++i)
                        {
                            const float e = gT[(size_t) i];
                            y[(size_t) i] += e * ((hT[(size_t) i] + resMoved[(size_t) i]) - y[(size_t) i]);
                        }
                        matchLoudness (y);
                    }
                }

                // Attacks (b / p / d / t / k bursts, note onsets): the original, unprocessed.
                if (options.protectConsonants)
                {
                    bool any = false;
                    for (int i = 0; i < len && ! any; i += 32) any = v.attack[(size_t) juce::jlimit (0, v.len - 1, (int) std::lround (sPos[(size_t) i]) - v.s0)] > 0.0f;
                    if (any)
                    {
                        // Original, time-mapped (exact copy where time is unchanged).
                        std::vector<float> ox ((size_t) len, 0.0f);
                        for (int i = 0; i < len; ++i)
                        {
                            const double sp = sPos[(size_t) i];
                            const long si = std::lround (sp) - v.s0;
                            if (si < 0 || si >= v.len) continue;
                            ox[(size_t) i] = std::abs (sp - std::round (sp)) < 1.0e-6 ? v.x[(size_t) si] : cubicAt (v.x, sp - v.s0);
                        }
                        for (int i = 0; i < len; ++i)
                        {
                            const long si = std::lround (sPos[(size_t) i]) - v.s0;
                            if (si < 0 || si >= v.len || editV[(size_t) i] <= 0.0f) continue;
                            const float a = v.attack[(size_t) si];
                            if (a > 0.0f)
                                y[(size_t) i] += a * (ox[(size_t) i] - y[(size_t) i]);
                        }
                    }
                }

                auto& slot = cimpl.regions[key];
                cimpl.regionSamples += y.size() - slot.y.size();
                slot.y = y;
                slot.lastUse = ++cimpl.useCounter;
                return y;
            };

            for (const auto& [ra, rb] : regions)
            {
                if (cancel != nullptr && cancel->load())
                    return nullptr;
                const int A = std::max (0, ra - fade), B = std::min (n, rb + fade);

                // Independent pieces: the voice's phase restarts in every unvoiced gap and all other
                // processing is local (< 60 ms), so the region is cut in the middle of gaps of at
                // least 20 ms and each piece is rendered with 100 ms of context on both sides.
                std::vector<int> cuts { A };
                {
                    const int step = std::max (1, (int) (0.0025 * fs));
                    int runStart = -1;
                    for (int t = A; t < B; t += step)
                    {
                        const double ts = map.toSource (((double) rs + t) / fs);
                        const bool unvoiced = track.vwAt (ts) <= 0.0f;
                        if (unvoiced && runStart < 0) runStart = t;
                        if (! unvoiced && runStart >= 0)
                        {
                            const int mid = (runStart + t) / 2;
                            if (t - runStart >= (int) (0.02 * fs) && mid - cuts.back() > (int) (0.5 * fs) && B - mid > (int) (0.5 * fs))
                                cuts.push_back (mid);
                            runStart = -1;
                        }
                    }
                }
                cuts.push_back (B);

                std::vector<float> yRegion ((size_t) (B - A));
                const int ctx = (int) (0.1 * fs);
                for (size_t c = 0; c + 1 < cuts.size(); ++c)
                {
                    if (cancel != nullptr && cancel->load())
                        return nullptr;
                    const int P0 = std::max (A, cuts[c] - ctx), P1 = std::min (B, cuts[c + 1] + ctx);
                    const auto yp = renderPiece (P0, P1);
                    std::copy (yp.begin() + (cuts[c] - P0), yp.begin() + (cuts[c + 1] - P0), yRegion.begin() + (cuts[c] - A));
                }

                // Splice into the output with short crossfades inside unedited audio.
                for (int i = A; i < B; ++i)
                {
                    const float yy = yRegion[(size_t) (i - A)];
                    float w = 1.0f;
                    if (i < ra) w = (float) (0.5 - 0.5 * std::cos (pi * (i - A) / (double) std::max (1, ra - A)));
                    else if (i >= rb) w = (float) (0.5 + 0.5 * std::cos (pi * (i - rb) / (double) std::max (1, B - rb)));
                    out[i] = src[(size_t) i] + (yy - src[(size_t) i]) * w;
                }
            }
            cimpl.trimRegions ((size_t) (4 * take.getEndSample()) + (size_t) (60.0 * fs));
        }

        // --- Note level -------------------------------------------------------------
        if (anyGain)
            for (int i = 0; i < n; ++i)
            {
                const float g = interp (gain, framePosStatic (rs, i, fs, hop, k0));
                if (g <= Note::silentDb + 0.01f)
                    out[i] = 0.0f;
                else if (std::abs (g) > 0.0005f)
                    out[i] *= std::pow (10.0f, g / 20.0f);
            }

        // --- Per-note EQ (only on the note, with smooth edges) ----------------------
        if (options.eq)
        {
            const int edge = (int) (0.015 * fs), warm = (int) (0.05 * fs);
            for (const Note* nt : rn)
            {
                if (! nt->eq.isActive())
                    continue;
                const int o0 = (int) std::lround (nt->outStart() * fs) - (int) rs;
                const int o1 = (int) std::lround (nt->outEnd() * fs) - (int) rs;
                const int r0 = std::max (0, o0 - edge), r1 = std::min (n, o1 + edge);
                if (r1 <= r0) continue;
                NoteEqChain chain (nt->eq, fs);
                const int w0 = std::max (0, r0 - warm);
                std::vector<float> filtered ((size_t) (r1 - r0));
                for (int i = w0; i < r1; ++i)
                {
                    const float y = (float) chain.process (out[i]);
                    if (i >= r0) filtered[(size_t) (i - r0)] = y;
                }
                for (int i = r0; i < r1; ++i)
                {
                    float w = 1.0f;
                    if (i < o0) w = (float) (0.5 - 0.5 * std::cos (pi * (i - r0) / (double) std::max (1, o0 - r0)));
                    else if (i >= o1) w = (float) (0.5 + 0.5 * std::cos (pi * (i - o1) / (double) std::max (1, r1 - o1)));
                    out[i] += (filtered[(size_t) (i - r0)] - out[i]) * w;
                }
            }
        }
    }

    return result;
}

} // namespace moontune
