#pragma once

#include <cmath>
#include <vector>

namespace moontune
{

// Band-limited sample-rate conversion (Kaiser-windowed sinc, 48 zero crossings each side, kernel
// table oversampled 1024x with linear interpolation). No delay: out[i] is the input evaluated at
// position (inStart + i * ratio), where ratio = input samples per output sample. Anti-aliasing
// cut-off at 0.96 x the lower Nyquist frequency. Stopband ~ -90 dB, passband flat to ~0.9 Nyquist.
class SincResampler
{
public:
    SincResampler()
    {
        table.resize ((size_t) (zeroCrossings * oversample + 2));
        const double beta = 9.0;
        const double i0b = besselI0 (beta);
        for (size_t k = 0; k < table.size(); ++k)
        {
            const double x = (double) k / oversample;   // 0 .. zeroCrossings
            const double r = x / zeroCrossings;
            const double w = r < 1.0 ? besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b : 0.0;
            const double s = x < 1.0e-12 ? 1.0 : std::sin (pi * x) / (pi * x);
            table[k] = (float) (w * s);
        }
    }

    static constexpr int zeroCrossings = 48;

    void process (const float* in, int inLen, double inStart, double ratio, float* out, int outLen) const
    {
        const double fc = 0.96 * std::min (1.0, 1.0 / ratio);   // cut-off relative to the input Nyquist
        const double span = zeroCrossings / fc;                 // kernel half-width in input samples
        for (int i = 0; i < outLen; ++i)
        {
            const double p = inStart + i * ratio;
            const int k0 = (int) std::ceil (p - span), k1 = (int) std::floor (p + span);
            double acc = 0.0;
            for (int k = std::max (0, k0); k <= std::min (inLen - 1, k1); ++k)
            {
                const double x = std::abs (p - k) * fc * oversample;
                const auto j = (size_t) x;
                if (j + 1 >= table.size()) continue;
                const double t = x - (double) j;
                acc += in[k] * (table[j] + (table[j + 1] - table[j]) * t);
            }
            out[i] = (float) (acc * fc);
        }
    }

private:
    static constexpr int oversample = 1024;
    static constexpr double pi = 3.14159265358979323846;
    std::vector<float> table;

    static double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1.0e-12 * sum) break;
        }
        return sum;
    }
};

} // namespace moontune
