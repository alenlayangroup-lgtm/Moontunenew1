#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace moontune
{

// A held note's steady sound, like Melodyne's: one moment of the voice is analysed into harmonics
// (amplitude and phase) and then sustained without repetition. The oscillator starts phase-aligned
// with the analysed audio, so it can be crossfaded in from the real voice without a seam. When the
// pitch changes the harmonic amplitudes follow the analysed spectral envelope (formants stay).
// Real-time safe: no allocation, no locks.
class HeldTone
{
public:
    static constexpr int maxHarmonics = 96;
    static constexpr int maxWindow = 8192;

    // x[0..len): the voice just before the oscillator takes over (len >= ~2 periods).
    // Returns false if the moment is not periodic enough to hold (noise, silence).
    bool analyse (const float* x, int len, double f0Hz, double sampleRate) noexcept
    {
        ready = false;
        if (x == nullptr || len < 16 || f0Hz <= 20.0 || sampleRate <= 0.0)
            return false;
        if (len > maxWindow) { x += len - maxWindow; len = maxWindow; }   // keep the latest audio
        sr = sampleRate;
        f0a = f0Hz;
        const double centre = 0.5 * (double) (len - 1);
        double wSum = 0.0, energy = 0.0;
        for (int n = 0; n < len; ++n)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (n + 0.5) / len);
            wSum += w;
            energy += w * (double) x[n] * (double) x[n];
            wx[(size_t) n] = w * (double) x[n];
        }
        if (wSum <= 0.0 || energy <= 1.0e-12)
            return false;

        // The voice's actual pitch at this moment (vibrato, drift) can differ from the note's
        // pitch: find it within +-1 semitone (energy of the first harmonics), so the analysis and
        // the hand-over from the real voice are exact. The tone then glides to the asked pitch.
        {
            auto lowHarmonicEnergy = [&] (double f)
            {
                double e = 0.0;
                for (int k = 1; k <= 6 && k * f < 0.45 * sr; ++k)
                {
                    const double omega = 2.0 * pi * k * f / sr;
                    const std::complex<double> step = std::polar (1.0, -omega);
                    std::complex<double> turn = std::polar (1.0, omega * centre), acc { 0.0, 0.0 };
                    for (int n = 0; n < len; ++n) { acc += wx[(size_t) n] * turn; turn *= step; }
                    e += std::norm (acc);
                }
                return e;
            };
            double best = f0Hz, bestE = -1.0;
            for (int c = -100; c <= 100; c += 10)
            {
                const double f = f0Hz * std::pow (2.0, c / 1200.0);
                const double e = lowHarmonicEnergy (f);
                if (e > bestE) { bestE = e; best = f; }
            }
            const double coarse = best;
            for (int c = -8; c <= 8; c += 2)
            {
                const double f = coarse * std::pow (2.0, c / 1200.0);
                const double e = lowHarmonicEnergy (f);
                if (e > bestE) { bestE = e; best = f; }
            }
            f0a = best;
        }

        numAnalysed = 0;
        double harmonicEnergy = 0.0;
        for (int k = 1; k <= maxHarmonics; ++k)
        {
            const double omega = 2.0 * pi * k * f0a / sr;
            if (k * f0a > 0.45 * sr) break;
            // Projection on the harmonic (rotator: no trig per sample).
            const std::complex<double> step = std::polar (1.0, -omega);
            std::complex<double> turn = std::polar (1.0, omega * centre);   // e^{-i w (0 - centre)}
            std::complex<double> acc { 0.0, 0.0 };
            for (int n = 0; n < len; ++n)
            {
                acc += wx[(size_t) n] * turn;
                turn *= step;
            }
            acc *= 2.0 / wSum;
            amp[(size_t) k - 1] = std::abs (acc);
            phase[(size_t) k - 1] = std::arg (acc);
            harmonicEnergy += 0.5 * amp[(size_t) k - 1] * amp[(size_t) k - 1];
            numAnalysed = k;
        }
        // Periodicity check: harmonics must carry most of the (windowed, mean-square) energy.
        if (numAnalysed == 0 || harmonicEnergy < 0.35 * (energy / wSum))
            return false;

        // Oscillator phases at the first sample after the analysed audio.
        const double ahead = (double) len - centre;
        const double ph1 = phase[0] + 2.0 * pi * f0a / sr * ahead;
        for (int k = 1; k <= maxHarmonics; ++k)
        {
            const double p = k <= numAnalysed ? phase[(size_t) k - 1] + 2.0 * pi * k * f0a / sr * ahead
                                              : k * ph1;   // above the analysed range: locked to the fundamental
            z[(size_t) k - 1] = std::polar (1.0, p);
        }
        f0 = f0a;
        f0Target = f0Hz;   // glide from the voice's pitch at this moment to the asked one
        updateBank();
        ready = true;
        return true;
    }

    bool isReady() const noexcept { return ready; }
    void reset() noexcept { ready = false; }

    // New pitch (Hz); the tone glides there over ~20 ms keeping its formants.
    void setPitch (double f0Hz) noexcept { if (f0Hz > 20.0) f0Target = f0Hz; }

    // Adds nothing, writes `n` samples of the tone.
    void render (float* out, int n) noexcept
    {
        if (! ready) { for (int i = 0; i < n; ++i) out[i] = 0.0f; return; }
        int done = 0;
        while (done < n)
        {
            const int chunk = std::min (n - done, 32);
            if (std::abs (f0Target - f0) > 1.0e-6 * f0)
            {
                const double a = 1.0 - std::exp (-(double) chunk / (0.020 * sr));
                f0 += (f0Target - f0) * a;
                if (std::abs (f0Target - f0) < 1.0e-4 * f0) f0 = f0Target;
                updateBank();
            }
            for (int i = 0; i < chunk; ++i)
            {
                double s = 0.0;
                for (int k = 0; k < active; ++k)
                {
                    s += gain[(size_t) k] * z[(size_t) k].real();
                    z[(size_t) k] *= rot[(size_t) k];
                }
                out[done + i] = (float) s;
            }
            for (int k = 0; k < active; ++k)   // keep |z| = 1 (rounding)
                z[(size_t) k] /= std::abs (z[(size_t) k]);
            // Harmonics not sounding still advance, so they come in phase-aligned if the pitch drops.
            for (int k = active; k < maxHarmonics; ++k)
                z[(size_t) k] *= std::pow (rot[(size_t) k], chunk);
            done += chunk;
        }
    }

private:
    static constexpr double pi = 3.14159265358979323846;

    // Amplitude of harmonic k at the current pitch: the analysed envelope (log-amplitude, linear in
    // frequency) sampled at k * f0.
    double envelopeAt (double freq) const noexcept
    {
        if (numAnalysed == 0) return 0.0;
        const double pos = freq / f0a;   // in analysed harmonic numbers
        if (pos <= 1.0) return amp[0];
        if (pos >= (double) numAnalysed) return pos > numAnalysed + 0.5 ? 0.0 : amp[(size_t) numAnalysed - 1];
        const int i = (int) pos;
        const double t = pos - i;
        const double a = std::log (std::max (amp[(size_t) i - 1], 1.0e-9));
        const double b = std::log (std::max (amp[(size_t) i], 1.0e-9));
        return std::exp (a + (b - a) * t);
    }

    void updateBank() noexcept
    {
        active = 0;
        for (int k = 1; k <= maxHarmonics; ++k)
        {
            rot[(size_t) k - 1] = std::polar (1.0, 2.0 * pi * k * f0 / sr);
            if (k * f0 <= 0.45 * sr)
            {
                gain[(size_t) k - 1] = envelopeAt (k * f0);
                active = k;
            }
            else
                gain[(size_t) k - 1] = 0.0;
        }
    }

    bool ready = false;
    double sr = 48000.0, f0a = 200.0, f0 = 200.0, f0Target = 200.0;
    int numAnalysed = 0, active = 0;
    std::array<double, maxHarmonics> amp {}, phase {}, gain {};
    std::array<std::complex<double>, maxHarmonics> z {}, rot {};
    std::array<double, maxWindow> wx {};
};

} // namespace moontune
