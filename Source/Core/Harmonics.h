#pragma once
// Surgical saturation: add harmonics of one narrow band, not level.
//  - Harmonic bass: saturate only the fundamental region (e.g. 30-90 Hz) and keep only the
//    harmonics it makes (above hp, below lp), so the low end reads on small speakers.
//    Optionally lower the sub itself (a low shelf).
//  - Presence fill: saturate a narrow band around a part's presence (e.g. 2.5 kHz) and add only
//    the harmonics above it, so the part reads clearer without getting louder.
// The harmonic level follows the band's own level (envelope-normalised drive), so the amount
// added is a fixed ratio to the fundamental, whatever the playing dynamics.
#include "Analyser.h"
#include "Biquad.h"
#include "Planner.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace clr {

struct HarmPlan {
    int32_t bassOn = 0;
    float bassLo = 30, bassHi = 90;   // band that drives the harmonics
    float bassDrive = 0.5f;           // 0..1
    float bassMixDb = -12;            // harmonics' level relative to the band (approx.)
    float bassHp = 100, bassLp = 400; // keep only harmonics in this range
    float subDb = 0;                  // optional: lower the sub (low shelf at bassHi), <= 0
    int32_t presOn = 0;
    float presFreq = 2500, presQ = 1.2f;
    float presDrive = 0.4f;
    float presMixDb = -15;
};

class HarmonicStage {
public:
    void prepare(double sampleRate) { fs = sampleRate; applied = HarmPlan {}; applied.bassOn = -1; reset(); }
    void reset()
    {
        for (auto& c : ch) { for (auto& f : c.f) f.reset(); c.envB = c.envP = 0; }
        mixB = mixP = 0;
    }
    // Call when the plan changes (cheap; recomputes coefficients).
    void set(const HarmPlan& p)
    {
        if (std::memcmp(&p, &applied, sizeof p) == 0) return;
        applied = p;
        const double ny = fs * 0.45;
        for (auto& c : ch) {
            c.f[0].set(makeCoeffs(BandType::HighPass, fs, p.bassLo, 0, 0.7071));
            c.f[1].set(makeCoeffs(BandType::LowPass, fs, std::min((double) p.bassHi, ny), 0, 0.7071));
            c.f[2].set(makeCoeffs(BandType::LowPass, fs, std::min((double) p.bassHi, ny), 0, 0.7071));
            c.f[3].set(makeCoeffs(BandType::HighPass, fs, p.bassHp, 0, 0.7071));
            c.f[4].set(makeCoeffs(BandType::HighPass, fs, p.bassHp, 0, 0.7071));
            c.f[5].set(makeCoeffs(BandType::LowPass, fs, std::min((double) p.bassLp, ny), 0, 0.7071));
            c.f[6].set(makeCoeffs(BandType::LowShelf, fs, p.bassHi, p.subDb, 0.7071));
            c.f[7].set(makeCoeffs(BandType::BandPass, fs, p.presFreq, 0, p.presQ));
            c.f[8].set(makeCoeffs(BandType::HighPass, fs, std::min((double) p.presFreq * 1.4, ny), 0, 0.7071));
            c.f[9].set(makeCoeffs(BandType::HighPass, fs, std::min((double) p.presFreq * 1.4, ny), 0, 0.7071));
            c.f[10].set(makeCoeffs(BandType::LowPass, fs, std::min(16000.0, ny), 0, 0.7071));
            c.f[12].set(makeCoeffs(BandType::HighPass, fs, std::min((double) p.presFreq * 1.4, ny), 0, 0.7071));
            c.f[11].set(makeCoeffs(BandType::HighPass, fs, p.bassHp, 0, 0.7071)); // 36 dB/oct: none of the fundamental gets back in
        }
        aEnv = 1.0f - std::exp(-1.0f / (0.015f * (float) fs));
        rEnv = 1.0f - std::exp(-1.0f / (0.120f * (float) fs));
    }
    bool active() const { return applied.bassOn > 0 || applied.presOn > 0 || mixB > 1e-6f || mixP > 1e-6f; }

    // In place. amount 0..1 scales the added harmonics (and the sub shelf); off = 0 glides them out.
    void process(float* const* data, int numCh, int n, float amount)
    {
        const float targB = applied.bassOn > 0 ? amount * std::pow(10.0f, applied.bassMixDb / 20.0f) : 0.0f;
        const float targP = applied.presOn > 0 ? amount * std::pow(10.0f, applied.presMixDb / 20.0f) : 0.0f;
        const float g = 1.0f - std::exp(-(float) n / (0.03f * (float) fs));
        const float b0 = mixB, p0 = mixP;
        mixB += (targB - mixB) * g; mixP += (targP - mixP) * g;
        if (b0 < 1e-7f && mixB < 1e-7f && p0 < 1e-7f && mixP < 1e-7f && !(applied.bassOn > 0 && applied.subDb < 0)) return;
        const float dB = 1.0f + 4.0f * applied.bassDrive, dP = 1.0f + 4.0f * applied.presDrive;
        const bool sub = applied.bassOn > 0 && applied.subDb < -0.01f && amount > 0;
        for (int c = 0; c < numCh && c < 2; ++c) {
            auto& s = ch[c];
            float* x = data[c];
            for (int i = 0; i < n; ++i) {
                const float t = (float) (i + 1) / (float) n;
                const float mb = b0 + (mixB - b0) * t, mp = p0 + (mixP - p0) * t;
                const float in = x[i];
                float add = 0;
                if (mb > 1e-7f) {
                    float b = s.f[2].process(s.f[1].process(s.f[0].process(in)));
                    const float a = std::fabs(b);
                    s.envB += (a - s.envB) * (a > s.envB ? aEnv : rEnv);
                    const float u = b / (s.envB * 1.4142f + 1e-6f);
                    float h = std::tanh(dB * u + 0.6f) - std::tanh(0.6f); // asymmetric: strong 2nd (octave), plus 3rd
                    h = s.f[5].process(s.f[11].process(s.f[4].process(s.f[3].process(h))));
                    add += h * s.envB * 1.4142f * mb * kCalB;
                }
                if (mp > 1e-7f) {
                    float b = s.f[7].process(in);
                    const float a = std::fabs(b);
                    s.envP += (a - s.envP) * (a > s.envP ? aEnv : rEnv);
                    const float u = b / (s.envP * 1.4142f + 1e-6f);
                    float h = std::tanh(dP * u + 0.15f) - std::tanh(0.15f);
                    h = s.f[10].process(s.f[12].process(s.f[9].process(s.f[8].process(h))));
                    add += h * s.envP * 1.4142f * mp * kCalP;
                }
                float y = in;
                if (sub) y = s.f[6].process(y);
                x[i] = y + add;
            }
        }
    }

private:
    // Calibration so the mix setting is roughly the harmonics' level relative to the band.
    static constexpr float kCalB = 6.0f, kCalP = 4.0f;
    struct Ch { Biquad f[13]; float envB = 0, envP = 0; };
    double fs = 48000;
    Ch ch[2];
    HarmPlan applied;
    float mixB = 0, mixP = 0, aEnv = 0.01f, rEnv = 0.001f;
};

struct HarmSuggestion {
    int track = 0;            // TrackMeas::id
    bool bass = true;         // false = presence fill
    HarmPlan plan;            // only the relevant half is switched on
    float confidence = 0;
    std::string reason;
};

// Who would benefit: low parts whose weight sits under ~110 Hz with weak harmonics (they vanish on
// small speakers), and leads that hold too little of their presence band.
// leads: track ids of the focus and section leads (the parts that should read clearly).
std::vector<HarmSuggestion> planHarmonics(const std::vector<TrackMeas>& tracks, const std::vector<int>& leads, int style);

} // namespace clr
