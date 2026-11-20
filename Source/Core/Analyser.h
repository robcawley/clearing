#pragma once
#include "Biquad.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace clr {

constexpr int kBands = 32;
constexpr int kBars = 256;   // timeline length (bars 1..256)
constexpr int kCoarse = 7;   // coarse bands for the timeline

inline float coarseEdge(int k)
{
    static const float e[kCoarse + 1] = { 20, 60, 150, 400, 1000, 2500, 6000, 18000 };
    return e[k];
}
inline const char* coarseName(int k)
{
    static const char* n[kCoarse] = { "sub", "low", "low-mid", "mid", "upper-mid", "presence", "air" };
    return n[k];
}
constexpr int kFft = 4096;
constexpr float kFloorDb = -120.0f;

// Centre frequency of analysis band b (log spaced, 25 Hz to 18 kHz).
inline float bandCentre(int b)
{
    const float lo = 25.0f, hi = 18000.0f;
    const float t = ((float) b + 0.5f) / (float) kBands;
    return lo * std::pow(hi / lo, t);
}
inline float bandEdge(int b)
{
    const float lo = 25.0f, hi = 18000.0f;
    return lo * std::pow(hi / lo, (float) b / (float) kBands);
}

inline void fft(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -6.28318530717958647692f / (float) len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Collects mono audio, runs a 4096-point FFT every 2048 samples, and keeps
// (a) a fast "live" band spectrum and (b) a cumulative "capture" spectrum that
// only counts frames above the activity gate, so silence never dilutes it.
// Levels are absolute (relative levels between tracks are preserved).
class Analyser {
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        window.resize(kFft);
        double sum = 0;
        for (int i = 0; i < kFft; ++i) {
            window[i] = 0.5f - 0.5f * std::cos(6.28318530717958647692f * (float) i / (float) kFft);
            sum += window[i];
        }
        norm = (float) (4.0 / (sum * sum));
        ring.assign(kFft, 0.0f);
        buf.assign(kFft, {});
        // bin ranges per band
        for (int b = 0; b < kBands; ++b) {
            const double lo = bandEdge(b), hi = bandEdge(b + 1);
            int k0 = (int) std::floor(lo * kFft / fs), k1 = (int) std::ceil(hi * kFft / fs);
            k0 = std::max(1, k0);
            k1 = std::min(kFft / 2 - 1, std::max(k0 + 1, k1));
            binLo[b] = k0; binHi[b] = k1;
        }
        for (int k = 0; k < kCoarse; ++k) {
            int k0 = (int) std::floor(coarseEdge(k) * kFft / fs), k1 = (int) std::floor(coarseEdge(k + 1) * kFft / fs);
            coarseLo[k] = std::max(1, k0); coarseHi[k] = std::min(kFft / 2, std::max(coarseLo[k] + 1, k1));
        }
        // Approximate BS.1770 K-weighting: high shelf (+4 dB around 1.7 kHz) and a 38 Hz high-pass.
        kShelf.set(makeCoeffs(BandType::HighShelf, fs, 1681.0, 4.0, 0.71));
        kHp.set(makeCoeffs(BandType::HighPass, fs, 38.0, 0.0, 0.5));
        reset();
        resetCapture();
    }

    // The bar the incoming audio belongs to (1-based), or 0 when unknown / not playing.
    void setBar(int b) { if (b != bar) { bar = b; inBar = 0; } }

    void reset()
    {
        std::fill(ring.begin(), ring.end(), 0.0f);
        pos = 0; sinceFft = 0;
        live.fill(kFloorDb);
    }

    void resetCapture()
    {
        capPower.fill(0.0);
        capFrames = 0;
        activeSeconds = 0;
        capEnergy = 0;
        capPeakDb.fill(kFloorDb);
        for (auto& h : hist) h.fill(0.0);
        histN = 0;
        for (auto& r : barCoarse) r.fill(0.0);
        barFrames.fill(0); barK.fill(0.0); barKN.fill(0);
        kShelf.reset(); kHp.reset();
        maxBar = 0;
    }

    void push(const float* x, int n, bool capturing)
    {
        const bool tl = capturing && bar >= 1 && bar <= kBars;
        for (int i = 0; i < n; ++i) {
            if (tl) {
                const double y = kHp.process(kShelf.process(x[i]));
                barK[(size_t) (bar - 1)] += y * y; ++barKN[(size_t) (bar - 1)];
            }
            ++inBar;
            ring[pos] = x[i];
            pos = (pos + 1) & (kFft - 1);
            if (++sinceFft >= kFft / 2) { sinceFft = 0; frame(capturing); }
        }
    }

    const std::array<float, kBands>& liveDb() const { return live; }
    std::array<float, kBands> captureDb() const
    {
        std::array<float, kBands> r;
        for (int b = 0; b < kBands; ++b)
            r[b] = capFrames > 0 ? toDb(capPower[b] / (double) capFrames) : kFloorDb;
        return r;
    }
    float rmsDb() const { return lastRmsDb; }
    // Timeline: mean energy per coarse band in bar b (1-based), dB; kFloorDb when not heard.
    float barDb(int b, int k) const
    {
        if (b < 1 || b > kBars || barFrames[(size_t) (b - 1)] == 0) return kFloorDb;
        return toDb(barCoarse[(size_t) (b - 1)][(size_t) k] / barFrames[(size_t) (b - 1)]);
    }
    // Approximate loudness of bar b (K-weighted, ungated, mono sum + 3 dB for a stereo pair), LUFS.
    float barLufs(int b) const
    {
        if (b < 1 || b > kBars || barKN[(size_t) (b - 1)] == 0) return kFloorDb;
        const double ms = barK[(size_t) (b - 1)] / (double) barKN[(size_t) (b - 1)];
        return ms <= 1e-12 ? kFloorDb : (float) (-0.691 + 10.0 * std::log10(ms) + 3.01);
    }
    int lastBar() const { return maxBar; }
    // Loudest short-term level per band while capturing (3 frames, ~130 ms at 48 kHz), dB.
    // Compared with captureDb() it shows which bands build up in bursts.
    const std::array<float, kBands>& capturePeakDb() const { return capPeakDb; }
    // Average level of the captured (audible) material, dB RMS.
    float captureRmsDb() const { return capFrames > 0 ? toDb(capEnergy / capFrames) : kFloorDb; }
    float activeSec() const { return activeSeconds; }
    int frames() const { return capFrames; }

private:
    static float toDb(double p) { return p <= 1e-12 ? kFloorDb : (float) (10.0 * std::log10(p)); }

    void frame(bool capturing)
    {
        double e = 0;
        for (int i = 0; i < kFft; ++i) {
            const float s = ring[(pos + i) & (kFft - 1)];
            buf[i] = { s * window[i], 0.0f };
            e += (double) s * s;
        }
        lastRmsDb = toDb(e / kFft);
        const double frameEnergy = e / kFft;
        fft(buf);
        std::array<double, kBands> p;
        for (int b = 0; b < kBands; ++b) {
            double s = 0;
            for (int k = binLo[b]; k < binHi[b]; ++k) s += std::norm(buf[k]) * norm;
            p[b] = s / (double) (binHi[b] - binLo[b]);
        }
        for (int b = 0; b < kBands; ++b) {
            const float d = toDb(p[b]);
            live[b] = live[b] * 0.7f + d * 0.3f;
        }
        // only frames lying wholly inside the bar, so a part that stops doesn't leak into the next bar
        if (capturing && bar >= 1 && bar <= kBars && inBar >= kFft) {
            auto& r = barCoarse[(size_t) (bar - 1)];
            for (int k = 0; k < kCoarse; ++k) {
                double s = 0;
                for (int j = coarseLo[k]; j < coarseHi[k]; ++j) s += std::norm(buf[(size_t) j]) * norm;
                r[(size_t) k] += s;
            }
            ++barFrames[(size_t) (bar - 1)];
            maxBar = std::max(maxBar, bar);
        }
        if (capturing && lastRmsDb > gateDb) {
            hist[(size_t) (histN % 3)] = p; ++histN;
            if (histN >= 3)
                for (int b = 0; b < kBands; ++b) {
                    const double s3 = (hist[0][(size_t) b] + hist[1][(size_t) b] + hist[2][(size_t) b]) / 3.0;
                    capPeakDb[(size_t) b] = std::fmax(capPeakDb[(size_t) b], toDb(s3));
                }
            for (int b = 0; b < kBands; ++b) capPower[b] += p[b];
            capEnergy += frameEnergy;
            ++capFrames;
            activeSeconds += (float) (kFft / 2) / (float) fs;
        }
    }

    double fs = 48000;
    std::vector<float> window, ring;
    std::vector<std::complex<float>> buf;
    float norm = 1;
    int pos = 0, sinceFft = 0;
    int binLo[kBands] {}, binHi[kBands] {};
    std::array<float, kBands> live {};
    std::array<double, kBands> capPower {};
    int capFrames = 0;
    double capEnergy = 0;
    int bar = 0, maxBar = 0;
    int64_t inBar = 0; // samples since the bar last changed
    int coarseLo[kCoarse] {}, coarseHi[kCoarse] {};
    std::array<std::array<double, kCoarse>, kBars> barCoarse {};
    std::array<int, kBars> barFrames {};
    std::array<double, kBars> barK {};
    std::array<long, kBars> barKN {};
    Biquad kShelf, kHp;
    std::array<float, kBands> capPeakDb {};
    std::array<std::array<double, kBands>, 3> hist {};
    int histN = 0;
    float activeSeconds = 0, lastRmsDb = kFloorDb;
    float gateDb = -60.0f;
};

} // namespace clr
