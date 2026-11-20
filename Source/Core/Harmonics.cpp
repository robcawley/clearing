#include "Harmonics.h"
#include <algorithm>
#include <cstdio>
#include <map>

namespace clr {
namespace {
double lin(float d) { return d <= kFloorDb + 1 ? 0.0 : std::pow(10.0, d / 10.0); }
float dbOf(double p) { return p > 1e-14 ? (float) (10.0 * std::log10(p)) : kFloorDb; }
double rangeEnergy(const TrackMeas& t, float lo, float hi)
{
    double s = 0;
    for (int b = 0; b < kBands; ++b) { const float f = bandCentre(b); if (f >= lo && f < hi) s += lin(t.db[(size_t) b]); }
    return s;
}
std::string hzs(float f)
{
    char b[32];
    if (f >= 1000) std::snprintf(b, sizeof b, "%.1f kHz", f / 1000.0f); else std::snprintf(b, sizeof b, "%d Hz", (int) std::lround(f));
    return b;
}
std::string pct(double x) { char b[16]; std::snprintf(b, sizeof b, "%d%%", (int) std::lround(x * 100)); return b; }
bool lowRole(int r) { return r == RoleBass || r == RoleKick || r == RoleDoubleBasses || r == RoleLowBrass || r == RoleSynth || r == RoleOther || r == RoleTimpani; }
} // namespace

std::vector<HarmSuggestion> planHarmonics(const std::vector<TrackMeas>& tracks, const std::vector<int>& leads, int style)
{
    std::vector<HarmSuggestion> out;
    const bool orch = style == StyleOrchestral;
    // Crowding of 150-400 Hz: how many parts are within 10 dB of the loudest there.
    float lmMax = kFloorDb; std::vector<float> lm;
    for (auto& t : tracks) { const float e = dbOf(rangeEnergy(t, 150, 400)); lm.push_back(e); lmMax = std::max(lmMax, e); }
    int crowd = 0; for (float e : lm) if (e > lmMax - 10.0f && e > kFloorDb + 1) ++crowd;

    // ---- Harmonic bass: one per layer (its member with the most weight down low) ----
    std::map<int, size_t> bestOfLayer;
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& t = tracks[i];
        if (t.layer <= 0) continue;
        auto it = bestOfLayer.find(t.layer);
        if (it == bestOfLayer.end() || rangeEnergy(t, 25, 130) > rangeEnergy(tracks[it->second], 25, 130)) bestOfLayer[t.layer] = i;
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& t = tracks[i];
        if (t.locked || t.activeSec < 8.0f || !lowRole(t.role)) continue;
        if (t.layer > 0 && bestOfLayer[t.layer] != i) continue;
        const double total = rangeEnergy(t, 20, 20000);
        if (total <= 0) continue;
        const double low = rangeEnergy(t, 20, 130);
        if (low / total < 0.5) continue; // its weight isn't down there
        int pk = -1;
        for (int b = 0; b < kBands; ++b) if (bandCentre(b) < 130 && (pk < 0 || t.db[(size_t) b] > t.db[(size_t) pk])) pk = b;
        if (pk < 0) continue;
        const float f0 = bandCentre(pk);
        if (f0 > 110) continue;
        const float fund = dbOf(rangeEnergy(t, f0 / 1.4f, f0 * 1.4f));
        const float harm = dbOf(rangeEnergy(t, f0 * 1.8f, f0 * 4.5f));
        const float ratio = harm - fund;
        if (ratio > -10.0f) continue; // already has the harmonics that carry it
        HarmSuggestion s; s.track = t.id; s.bass = true;
        auto& p = s.plan;
        p.bassOn = 1;
        p.bassLo = std::max(25.0f, f0 / 1.6f); p.bassHi = std::min(140.0f, f0 * 1.6f);
        p.bassHp = std::max(1.7f * f0, p.bassHi * 1.05f);
        const bool crowded = crowd >= 3;
        p.bassLp = crowded ? 300.0f : 450.0f;
        const bool kick = t.role == RoleKick;
        p.bassMixDb = kick ? -16.0f : crowded ? -14.0f : -12.0f;
        if (orch) p.bassMixDb -= 2.0f;
        p.bassDrive = kick ? 0.35f : 0.5f;
        s.confidence = std::min(0.85f, 0.4f + (-10.0f - ratio) / 20.0f);
        char buf[64]; std::snprintf(buf, sizeof buf, "%d", (int) std::lround(-ratio));
        s.reason = t.name + "'s weight sits near " + hzs(f0) + ", with its harmonics " + buf
                   + " dB under the fundamental. Earbuds and laptops barely play " + hzs(f0)
                   + ", so it thins out there. Harmonic bass adds harmonics of " + hzs(p.bassLo) + "-" + hzs(p.bassHi)
                   + " only, kept between " + hzs(p.bassHp) + " and " + hzs(p.bassLp) + (crowded ? " (low-mids are busy, so it stays low and gentle)" : "")
                   + ", so the note reads on small speakers. With it in, the sub itself can come down 1-2 dB for headroom.";
        out.push_back(s);
    }

    // ---- Presence fill: leads that hold too little of their presence band ----
    for (int id : leads) {
        auto it = std::find_if(tracks.begin(), tracks.end(), [&](const TrackMeas& t) { return t.id == id; });
        if (it == tracks.end()) continue;
        auto& t = *it;
        if (t.locked || t.activeSec < 8.0f || t.role == RoleKick || t.role == RoleBass) continue;
        if (std::any_of(out.begin(), out.end(), [&](const HarmSuggestion& s) { return s.track == t.id && !s.bass; })) continue;
        int pk = -1; float mx = kFloorDb;
        for (int b = 0; b < kBands; ++b) mx = std::max(mx, t.db[(size_t) b]);
        for (int b = 0; b < kBands; ++b) { const float f = bandCentre(b); if (f >= 1200 && f <= 5000 && (pk < 0 || t.db[(size_t) b] > t.db[(size_t) pk])) pk = b; }
        if (pk < 0 || t.db[(size_t) pk] < mx - 30.0f) continue; // no presence of its own to build on
        const float f = bandCentre(pk);
        double mine = rangeEnergy(t, f / 1.3f, f * 1.3f), all = 0;
        for (auto& o : tracks) all += rangeEnergy(o, f / 1.3f, f * 1.3f);
        if (all <= 0) continue;
        const double share = mine / all;
        const double target = orch ? 0.30 : 0.35;
        if (share >= target) continue;
        HarmSuggestion s; s.track = t.id; s.bass = false;
        auto& p = s.plan;
        p.presOn = 1; p.presFreq = f; p.presQ = 1.2f;
        p.presDrive = orch ? 0.3f : 0.4f;
        p.presMixDb = share < 0.2 ? -13.0f : -15.0f;
        if (orch) p.presMixDb -= 2.0f;
        s.confidence = (float) std::min(0.8, 0.35 + (target - share) * 1.5);
        s.reason = t.name + " leads but holds only " + pct(share) + " of the band around " + hzs(f)
                   + ", where its attack and definition live. Light saturation of that band adds harmonics above it (density, not level), so it reads clearer without getting louder.";
        out.push_back(s);
    }
    return out;
}

} // namespace clr
