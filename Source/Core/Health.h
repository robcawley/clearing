#pragma once
// Mix health on the stereo out: per bar and band, how mono-safe and how wide the mix is;
// true peak; and gated (BS.1770-style) loudness. Fed L/R by the master instance's analysis thread.
#include "Analyser.h"
#include "Biquad.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace clr {

constexpr int kHBands = 5; // sub, low, low-mid, mid, high
inline float healthEdge(int k)
{
    static const float e[kHBands + 1] = { 20, 60, 150, 400, 2500, 18000 };
    return e[k];
}
inline const char* healthName(int k)
{
    static const char* n[kHBands] = { "sub", "low", "low-mid", "mid", "high" };
    return n[k];
}

struct HealthBar {
    double ll[kHBands] {}, rr[kHBands] {}, lr[kHBands] {};
    float tpDb = kFloorDb;     // true peak in the bar (dBTP, approx.)
    int64_t n = 0;
};

class HealthAnalyser {
public:
    void prepare(double sampleRate)
    {
        fs = sampleRate;
        for (int k = 0; k < kHBands; ++k)
            for (int ch = 0; ch < 2; ++ch) {
                for (int s = 0; s < 2; ++s) {
                    hp[k][ch][s].set(makeCoeffs(BandType::HighPass, fs, healthEdge(k), 0, 0.7071));
                    lp[k][ch][s].set(makeCoeffs(BandType::LowPass, fs, std::min((double) healthEdge(k + 1), fs * 0.45), 0, 0.7071));
                }
            }
        for (int ch = 0; ch < 2; ++ch) {
            kShelf[ch].set(makeCoeffs(BandType::HighShelf, fs, 1681.0, 4.0, 0.71));
            kHp[ch].set(makeCoeffs(BandType::HighPass, fs, 38.0, 0.0, 0.5));
        }
        // 4x true-peak interpolator: windowed sinc, 12 taps per phase.
        for (int p = 0; p < 4; ++p)
            for (int t = 0; t < kTaps; ++t) {
                const double x = (double) (t - kTaps / 2 + 1) - (double) p / 4.0;
                const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(3.14159265358979 * x) / (3.14159265358979 * x);
                const double w = 0.5 + 0.5 * std::cos(3.14159265358979 * x / (kTaps / 2 + 1));
                fir[p][t] = (float) (sinc * w);
            }
        for (int p = 0; p < 4; ++p) { double s = 0; for (int t = 0; t < kTaps; ++t) s += fir[p][t]; for (int t = 0; t < kTaps; ++t) fir[p][t] = (float) (fir[p][t] / s); }
        subLen = std::max(1, (int) std::lround(fs * 0.1));
        reset();
    }

    void reset()
    {
        for (auto& b : bars) b = HealthBar {};
        for (auto& a : hp) for (auto& b : a) for (auto& f : b) f.reset();
        for (auto& a : lp) for (auto& b : a) for (auto& f : b) f.reset();
        for (int ch = 0; ch < 2; ++ch) { kShelf[ch].reset(); kHp[ch].reset(); hist[ch].fill(0.0f); }
        histPos = 0;
        subAcc = 0; subN = 0; subs.clear();
        tpMax = kFloorDb; spMax = kFloorDb; maxBar = 0;
    }

    void process(const float* L, const float* R, int n, int bar)
    {
        const bool tl = bar >= 1 && bar <= kBars;
        HealthBar* hb = tl ? &bars[(size_t) (bar - 1)] : nullptr;
        if (tl) maxBar = std::max(maxBar, bar);
        for (int i = 0; i < n; ++i) {
            const float l = L[i], r = R[i];
            // bands
            if (hb) {
                for (int k = 0; k < kHBands; ++k) {
                    float a = l, b = r;
                    for (int s = 0; s < 2; ++s) { a = hp[k][0][s].process(a); b = hp[k][1][s].process(b); }
                    for (int s = 0; s < 2; ++s) { a = lp[k][0][s].process(a); b = lp[k][1][s].process(b); }
                    hb->ll[k] += (double) a * a; hb->rr[k] += (double) b * b; hb->lr[k] += (double) a * b;
                }
                ++hb->n;
            }
            // peaks
            const float sp = std::max(std::fabs(l), std::fabs(r));
            float tp = sp;
            hist[0][(size_t) histPos] = l; hist[1][(size_t) histPos] = r;
            for (int ch = 0; ch < 2; ++ch)
                for (int p = 1; p < 4; ++p) {
                    double acc = 0;
                    for (int t = 0; t < kTaps; ++t) acc += fir[p][t] * hist[ch][(size_t) ((histPos - t + kTaps) % kTaps)];
                    tp = std::max(tp, (float) std::fabs(acc));
                }
            histPos = (histPos + 1) % kTaps;
            const float tpDb = tp > 1e-9f ? 20.0f * std::log10(tp) : kFloorDb;
            const float spDb = sp > 1e-9f ? 20.0f * std::log10(sp) : kFloorDb;
            tpMax = std::max(tpMax, tpDb); spMax = std::max(spMax, spDb);
            if (hb) hb->tpDb = std::max(hb->tpDb, tpDb);
            // loudness: K-weighted, both channels summed, 100 ms sub-blocks
            const double kl = kHp[0].process(kShelf[0].process(l)), kr = kHp[1].process(kShelf[1].process(r));
            subAcc += kl * kl + kr * kr;
            if (++subN >= subLen) { subs.push_back(subAcc / subN); subAcc = 0; subN = 0; }
        }
    }

    int lastBar() const { return maxBar; }
    const HealthBar& bar(int b) const { return bars[(size_t) (b - 1)]; }
    float truePeakDb() const { return tpMax; }
    float samplePeakDb() const { return spMax; }

    // Correlation (-1..1) of band k in bar b; 1 = mono, 0 = unrelated, -1 = out of phase. NaN-free.
    static float correlation(const HealthBar& h, int k)
    {
        const double d = std::sqrt(h.ll[k] * h.rr[k]);
        return d > 1e-18 ? (float) std::clamp(h.lr[k] / d, -1.0, 1.0) : 1.0f;
    }
    // How much of the band survives a mono fold-down (0 dB = all of it, -3 = uncorrelated, very negative = cancels).
    static float monoLossDb(const HealthBar& h, int k)
    {
        const double st = h.ll[k] + h.rr[k];
        if (st <= 1e-18) return 0.0f;
        const double mono = (h.ll[k] + h.rr[k] + 2.0 * h.lr[k]) / 2.0;
        return (float) std::max(-60.0, 10.0 * std::log10(std::max(mono, 1e-30) / st));
    }
    // Band energy (dB, both channels).
    static float energyDb(const HealthBar& h, int k)
    {
        if (h.n <= 0) return kFloorDb;
        const double e = (h.ll[k] + h.rr[k]) / (double) h.n;
        return e > 1e-14 ? (float) (10.0 * std::log10(e)) : kFloorDb;
    }

    // Gated integrated loudness (400 ms blocks, 75% overlap; -70 LUFS absolute and -10 LU relative gates).
    float integratedLufs() const
    {
        std::vector<double> blocks;
        for (size_t i = 3; i < subs.size(); ++i) blocks.push_back((subs[i] + subs[i - 1] + subs[i - 2] + subs[i - 3]) / 4.0);
        auto lufs = [](double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-30)); };
        double s = 0; int c = 0;
        for (double b : blocks) if (lufs(b) > -70.0) { s += b; ++c; }
        if (!c) return kFloorDb;
        const double rel = lufs(s / c) - 10.0;
        s = 0; c = 0;
        for (double b : blocks) if (lufs(b) > -70.0 && lufs(b) > rel) { s += b; ++c; }
        return c ? (float) lufs(s / c) : kFloorDb;
    }
    // Loudest 3 s window (short-term loudness).
    float maxShortTermLufs() const
    {
        if (subs.size() < 30) return kFloorDb;
        double win = 0; for (size_t i = 0; i < 30; ++i) win += subs[i];
        double best = win;
        for (size_t i = 30; i < subs.size(); ++i) { win += subs[i] - subs[i - 30]; best = std::max(best, win); }
        return (float) (-0.691 + 10.0 * std::log10(std::max(best / 30.0, 1e-30)));
    }

private:
    static constexpr int kTaps = 12;
    double fs = 48000;
    Biquad hp[kHBands][2][2], lp[kHBands][2][2];
    Biquad kShelf[2], kHp[2];
    float fir[4][kTaps] {};
    std::array<float, kTaps> hist[2] {};
    int histPos = 0;
    std::array<HealthBar, kBars> bars {};
    int subLen = 4800, subN = 0;
    double subAcc = 0;
    std::vector<double> subs;
    float tpMax = kFloorDb, spMax = kFloorDb;
    int maxBar = 0;
};

} // namespace clr
