#pragma once
#include <cmath>

namespace clr {

enum class BandType : int { Bell = 0, LowShelf = 1, HighShelf = 2, HighPass = 3, BandPass = 4, LowPass = 5 };

struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

inline Coeffs makeCoeffs(BandType t, double fs, double f, double gainDb, double q)
{
    const double pi = 3.14159265358979323846;
    f = std::fmin(std::fmax(f, 10.0), fs * 0.45);
    q = std::fmax(q, 0.1);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * pi * f / fs, cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    switch (t) {
    case BandType::Bell:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case BandType::LowShelf: {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - s;
        break; }
    case BandType::HighShelf: {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - s;
        break; }
    case BandType::BandPass: // constant 0 dB peak gain (detectors)
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case BandType::LowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    default: // HighPass
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    }
    Coeffs c;
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

// Magnitude response in dB at frequency f.
inline double magnitudeDb(const Coeffs& c, double fs, double f)
{
    const double pi = 3.14159265358979323846;
    const double w = 2.0 * pi * f / fs;
    const double cw = std::cos(w), sw = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = c.b0 + c.b1 * cw + c.b2 * c2, ni = -(c.b1 * sw + c.b2 * s2);
    const double dr = 1 + c.a1 * cw + c.a2 * c2, di = -(c.a1 * sw + c.a2 * s2);
    const double num = nr * nr + ni * ni, den = dr * dr + di * di;
    return 10.0 * std::log10((num + 1e-30) / (den + 1e-30));
}

// Transposed direct form II, one channel.
struct Biquad {
    Coeffs c;
    double z1 = 0, z2 = 0;
    void set(const Coeffs& n) { c = n; }
    void reset() { z1 = z2 = 0; }
    inline float process(float x)
    {
        const double in = x;
        const double out = c.b0 * in + z1;
        z1 = c.b1 * in - c.a1 * out + z2;
        z2 = c.b2 * in - c.a2 * out;
        return (float) out;
    }
};

} // namespace clr
