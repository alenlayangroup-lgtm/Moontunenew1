#pragma once

#include <array>
#include <cmath>
#include <complex>
#include "Model/NoteModel.h"

namespace moontune
{

// RBJ "Audio EQ Cookbook" biquads (public-domain formulas), double precision.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    static Biquad design (const EqBand& band, double sampleRate) noexcept
    {
        Biquad f;
        const double fs = sampleRate;
        const double freq = std::min ((double) band.frequency, fs * 0.45);
        const double w0 = 2.0 * 3.14159265358979323846 * freq / fs;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double q = std::max (0.1, (double) band.q);
        const double alpha = sw / (2.0 * q);
        const double A = std::pow (10.0, band.gainDb / 40.0);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (band.type)
        {
            case EqBand::peak:
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
                break;
            case EqBand::lowShelf:
            {
                const double s = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - s);
                a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case EqBand::highShelf:
            {
                const double s = 2 * std::sqrt (A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - s);
                a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
            case EqBand::lowCut:   // 12 dB/oct high-pass
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case EqBand::highCut:  // 12 dB/oct low-pass
            default:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
        }

        f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
        return f;
    }

    double process (double x) noexcept   // transposed direct form II
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    double magnitudeAt (double freq, double sampleRate) const noexcept
    {
        const double w = 2.0 * 3.14159265358979323846 * freq / sampleRate;
        const std::complex<double> z1c = std::polar (1.0, -w), z2c = std::polar (1.0, -2.0 * w);
        const auto num = b0 + b1 * z1c + b2 * z2c;
        const auto den = 1.0 + a1 * z1c + a2 * z2c;
        return std::abs (num / den);
    }
};

// The active bands of one note's EQ as a filter chain.
struct NoteEqChain
{
    std::array<Biquad, 4> filters;
    std::array<bool, 4> active { false, false, false, false };

    NoteEqChain (const NoteEq& eq, double sampleRate)
    {
        for (size_t i = 0; i < 4; ++i)
        {
            active[i] = eq.enabled && eq.bands[i].isActive();
            if (active[i])
                filters[i] = Biquad::design (eq.bands[i], sampleRate);
        }
    }

    double process (double x) noexcept
    {
        for (size_t i = 0; i < 4; ++i)
            if (active[i])
                x = filters[i].process (x);
        return x;
    }

    double magnitudeDb (double freq, double sampleRate) const noexcept
    {
        double m = 1.0;
        for (size_t i = 0; i < 4; ++i)
            if (active[i])
                m *= filters[i].magnitudeAt (freq, sampleRate);
        return 20.0 * std::log10 (std::max (m, 1.0e-9));
    }
};

} // namespace moontune
