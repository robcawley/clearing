#include "MixBalance.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace clr {

const char* mixGroupName(int g)
{
    static const char* n[GroupCount] = { "lead", "drums", "bass", "guitars", "keys", "synths", "backing vocals",
                                         "strings", "brass", "woodwinds", "percussion", "other parts" };
    return g >= 0 && g < GroupCount ? n[g] : "other parts";
}

int mixGroupOf(int role)
{
    switch (role) {
    case RoleLeadVocal: case RoleSolo: return GroupLead;
    case RoleKick: case RoleSnare: case RoleOverheads: return GroupDrums;
    case RoleBass: case RoleDoubleBasses: return GroupBass;
    case RoleAcousticGtr: case RoleElectricGtr: return GroupGuitars;
    case RolePiano: case RoleHarp: return GroupKeys;
    case RoleSynth: return GroupSynths;
    case RoleBackVocal: case RoleChoir: return GroupBacking;
    case RoleViolins: case RoleViolas: case RoleCellos: return GroupStrings;
    case RoleTrumpets: case RoleHorns: case RoleLowBrass: return GroupBrass;
    case RoleWoodwinds: return GroupWinds;
    case RoleTimpani: case RoleOrchPerc: return GroupPerc;
    default: return GroupOther;
    }
}

// Group loudness relative to each other (dB). The lead's value is for "blended"; on top / inside shift it.
// Order: lead, drums, bass, guitars, keys, synths, backing, strings, brass, winds, perc, other.
BalanceStyle balanceStyle(const std::string& raw)
{
    std::string s; for (char ch : raw) s += (char) std::tolower((unsigned char) ch);
    struct T { const char* name; float v[GroupCount]; };
    static const T table[] = {
        { "industrial", { 0.0f, -0.5f, -3.0f, -2.5f, -5.5f, -4.0f, -5.0f, -6.0f, -6.0f, -7.0f, -5.5f, -6.0f } },
        { "rock",       { 0.0f, -1.0f, -3.5f, -2.0f, -6.0f, -6.0f, -4.5f, -6.0f, -6.0f, -7.0f, -6.0f, -6.5f } },
        { "pop",        { 0.0f, -2.5f, -4.0f, -6.0f, -5.0f, -5.0f, -4.0f, -6.0f, -6.0f, -7.0f, -7.0f, -7.0f } },
        { "electronic", { 0.0f, 0.5f, -1.5f, -4.5f, -4.5f, -2.0f, -4.5f, -5.0f, -6.0f, -7.0f, -5.0f, -5.5f } },
        { "hip hop",    { 0.0f, -0.5f, -1.5f, -6.0f, -5.0f, -4.5f, -4.0f, -6.0f, -6.0f, -7.0f, -6.0f, -6.5f } },
        { "orchestral", { 0.0f, -6.0f, -5.0f, -7.0f, -4.5f, -6.0f, -3.5f, -1.0f, -2.5f, -3.0f, -5.0f, -6.0f } },
        { "ambient",    { 0.0f, -4.0f, -3.5f, -4.0f, -2.5f, -1.5f, -3.0f, -2.5f, -4.0f, -4.0f, -6.0f, -3.0f } },
    };
    if (s == "hiphop" || s == "hip-hop" || s == "rap") s = "hip hop";
    if (s == "metal" || s == "alternative" || s == "punk" || s == "band") s = "rock";
    if (s == "edm" || s == "techno" || s == "house" || s == "synthwave" || s == "trip hop") s = "electronic";
    if (s == "film" || s == "cinematic" || s == "score") s = "orchestral";
    if (s == "country" || s == "singer-songwriter" || s == "folk" || s == "r&b" || s == "soul") s = "pop";
    for (auto& t : table)
        if (s == t.name) { BalanceStyle b; b.name = t.name; std::copy(t.v, t.v + GroupCount, b.target); return b; }
    BalanceStyle b; b.name = "rock"; std::copy(table[1].v, table[1].v + GroupCount, b.target); return b;
}

std::vector<std::string> balanceStyleNames() { return { "industrial", "rock", "pop", "electronic", "hip hop", "orchestral", "ambient" }; }

namespace {
double lin(float d) { return d <= kFloorDb + 1 ? 0.0 : std::pow(10.0, d / 10.0); }
float dbOf(double p) { return p > 1e-14 ? (float) (10.0 * std::log10(p)) : kFloorDb; }
std::string f1(float x) { char b[16]; std::snprintf(b, sizeof b, "%.1f", x); return b; }
std::string article(const std::string& w) { return !w.empty() && std::string("aeiou").find(w[0]) != std::string::npos ? "an" : "a"; }
std::string signed1(float x) { char b[16]; std::snprintf(b, sizeof b, "%+.1f", x); return b; }

struct Unit {
    std::vector<int> ids;
    std::string name;
    int role = RoleOther;
    float priority = 0.4f;
    bool locked = false;
    std::vector<float> lufs; // per bar, at the stereo out
    float peak = kFloorDb;
};

// Loudness per bar: the measured value, or an estimate from the band energies when it's missing.
std::vector<float> barLoudness(const TrackTimeline& t)
{
    std::vector<float> l((size_t) kBars, kFloorDb);
    for (int b = 0; b < kBars; ++b) {
        float v = b < (int) t.lufs.size() ? t.lufs[(size_t) b] : kFloorDb;
        if (v <= -100.0f) {
            // no loudness reading: weight the bands roughly like K-weighting (low end counts less)
            static const float w[kCoarse] = { -8.0f, -2.0f, 0.0f, 0.0f, 1.5f, 3.0f, 2.0f };
            double s = 0;
            for (int k = 0; k < kCoarse; ++k) s += lin(t.db[(size_t) b][(size_t) k] + w[k]);
            v = s > 0 ? dbOf(s) : kFloorDb;
        }
        l[(size_t) b] = v > kFloorDb + 1 ? v + t.offsetDb : kFloorDb;
    }
    return l;
}
} // namespace

MixBalance planMixBalance(const std::vector<TrackTimeline>& tracks, const std::vector<SectionSpan>& sectionsIn,
                          const std::string& styleName, LeadPlace lead, float maxMove)
{
    MixBalance out;
    const BalanceStyle style = balanceStyle(styleName);
    out.style = style.name; out.lead = lead;
    const float leadShift = lead == LeadPlace::OnTop ? 2.0f : lead == LeadPlace::Inside ? -2.0f : 0.0f;

    // Units: a layer is one part.
    std::vector<Unit> units;
    std::map<int, size_t> byLayer;
    int bars = 0;
    for (auto& t : tracks) {
        auto l = barLoudness(t);
        for (int b = 0; b < kBars; ++b) if (l[(size_t) b] > kFloorDb + 1) bars = std::max(bars, b + 1);
        if (t.layer > 0 && byLayer.count(t.layer)) {
            auto& u = units[byLayer[t.layer]];
            u.ids.push_back(t.id);
            if (u.name.find(" layer") == std::string::npos) u.name += " layer";
            u.priority = std::max(u.priority, t.priority);
            u.locked = u.locked || t.locked;
            for (int b = 0; b < kBars; ++b) u.lufs[(size_t) b] = dbOf(lin(u.lufs[(size_t) b]) + lin(l[(size_t) b]));
            continue;
        }
        Unit u; u.ids = { t.id }; u.name = t.name; u.role = t.role; u.priority = t.priority; u.locked = t.locked; u.lufs = l;
        if (t.layer > 0) byLayer[t.layer] = units.size();
        units.push_back(u);
    }
    for (auto& u : units) for (float v : u.lufs) u.peak = std::max(u.peak, v);
    if (units.empty() || bars < 2) { out.notes.push_back("Not enough per-bar data: listen while the song plays from the top."); return out; }

    std::vector<SectionSpan> secs = sectionsIn;
    const bool whole = secs.empty();
    if (whole) { SectionSpan s; s.name = "the whole song"; s.start = 1; s.end = bars; secs.push_back(s); }

    // Per section: who plays, how loud, where the style puts them.
    const size_t U = units.size(), S = secs.size();
    std::vector<std::vector<float>> L(U, std::vector<float>(S, NAN)), T(U, std::vector<float>(S, 0.0f));
    std::vector<std::vector<int>> nbars(U, std::vector<int>(S, 0));
    std::vector<int> leadOf(S, -1);
    for (size_t si = 0; si < S; ++si) {
        auto& sp = secs[si];
        SectionBalance sb; sb.name = sp.name; sb.index = whole ? -1 : (int) si;
        sb.start = std::max(1, sp.start); sb.end = std::min(sp.end, bars);
        out.sections.push_back(sb);
        if (sb.end < sb.start) continue;
        const int len = sb.end - sb.start + 1;
        std::vector<int> present;
        for (size_t u = 0; u < U; ++u) {
            double e = 0; int n = 0;
            for (int b = sb.start - 1; b < sb.end; ++b) {
                const float v = units[u].lufs[(size_t) b];
                if (v > units[u].peak - 20.0f && v > -70.0f) { e += lin(v); ++n; }
            }
            if (n >= std::max(2, len / 4)) { L[u][si] = dbOf(e / n); nbars[u][si] = n; present.push_back((int) u); }
        }
        if (present.empty()) continue;
        // the lead: the section's named lead if it plays, else the highest-priority part (a lead vocal first)
        int leadU = -1;
        for (int u : present) for (int id : units[(size_t) u].ids) if (id == sp.leadId) leadU = u;
        if (leadU < 0)
            for (int u : present) {
                auto score = [&](int x) { return units[(size_t) x].priority + (mixGroupOf(units[(size_t) x].role) == GroupLead ? 1.0f : 0.0f); };
                if (leadU < 0 || score(u) > score(leadU)) leadU = u;
            }
        leadOf[si] = leadU;
        auto groupOf = [&](int u) { return u == leadU ? (int) GroupLead : mixGroupOf(units[(size_t) u].role) == GroupLead ? (int) GroupBacking : mixGroupOf(units[(size_t) u].role); };
        std::map<int, std::vector<int>> groups;
        for (int u : present) groups[groupOf(u)].push_back(u);
        for (auto& [g, mem] : groups) {
            float meanP = 0; for (int u : mem) meanP += units[(size_t) u].priority; meanP /= (float) mem.size();
            for (int u : mem)
                T[(size_t) u][si] = style.target[g] + (g == GroupLead ? leadShift : 0.0f) - 10.0f * std::log10((float) mem.size())
                                  + 4.0f * (units[(size_t) u].priority - meanP);
        }
    }

    // Solve for one level per part (g) and one overall level per section (c: sections are allowed to be
    // louder or quieter than each other; that's the arrangement), so that L + g sits on T + c.
    // Weighted medians throughout, so a part that's far off doesn't pull the others.
    auto wmedian = [](std::vector<std::pair<float, float>> v) {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        float tot = 0; for (auto& x : v) tot += x.second;
        float acc = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            acc += v[i].second;
            if (acc > tot * 0.5f + 1e-6f) return v[i].first;
            if (std::fabs(acc - tot * 0.5f) <= 1e-6f && i + 1 < v.size()) // exactly half: the smaller move
                return std::fabs(v[i].first) <= std::fabs(v[i + 1].first) ? v[i].first : v[i + 1].first;
        }
        return v.back().first;
    };
    std::vector<float> g(U, 0.0f), cS(S, 0.0f);
    for (int it = 0; it < 30; ++it) {
        for (size_t si = 0; si < S; ++si) {
            std::vector<std::pair<float, float>> r;
            for (size_t u = 0; u < U; ++u) if (!std::isnan(L[u][si])) r.push_back({ L[u][si] + g[u] - T[u][si], (int) u == leadOf[si] ? 1.0f : units[u].priority });
            cS[si] = wmedian(r);
        }
        for (size_t u = 0; u < U; ++u) {
            if (units[u].locked) { g[u] = 0; continue; }
            std::vector<std::pair<float, float>> r;
            for (size_t si = 0; si < S; ++si) if (!std::isnan(L[u][si])) r.push_back({ T[u][si] + cS[si] - L[u][si], (float) nbars[u][si] });
            g[u] = wmedian(r);
        }
        // keep the overall level: the parts' moves are centred on 0 (priority-weighted median)
        std::vector<std::pair<float, float>> gm;
        for (size_t u = 0; u < U; ++u) if (!units[u].locked) gm.push_back({ g[u], units[u].priority });
        const float k = wmedian(gm);
        for (size_t u = 0; u < U; ++u) if (!units[u].locked) g[u] -= k;
    }
    // Section by section, what each part needs: its song level plus any ride.
    double sumBefore = 0, sumAfter = 0; int nSpread = 0;
    std::vector<std::vector<float>> need(U, std::vector<float>(S, NAN));
    for (size_t si = 0; si < S; ++si) {
        auto& sb = out.sections[si];
        double sp2 = 0;
        for (size_t u = 0; u < U; ++u) {
            if (std::isnan(L[u][si])) continue;
            PartLoudness pl; pl.track = units[u].ids[0]; pl.name = units[u].name; pl.lufs = L[u][si];
            pl.lead = (int) u == leadOf[si];
            pl.group = pl.lead ? GroupLead : mixGroupOf(units[u].role) == GroupLead ? GroupBacking : mixGroupOf(units[u].role);
            pl.target = T[u][si] + cS[si]; pl.locked = units[u].locked;
            pl.move = pl.locked ? 0.0f : std::clamp(pl.target - pl.lufs, -maxMove, maxMove);
            need[u][si] = pl.move;
            sp2 += (pl.lufs - pl.target) * (pl.lufs - pl.target);
            sumBefore += (pl.lufs - pl.target) * (pl.lufs - pl.target); ++nSpread;
            sb.parts.push_back(pl);
        }
        sb.spread = sb.parts.empty() ? 0.0f : (float) std::sqrt(sp2 / sb.parts.size());
    }

    // Whole-song moves, then rides where a part is still off by 1.5 dB or more in a section.
    for (size_t u = 0; u < U; ++u) {
        if (units[u].locked) continue;
        auto& Un = units[u];
        const float gu = std::clamp(g[u], -maxMove, maxMove);
        const float base = std::fabs(gu) >= 0.75f ? std::round(gu * 2.0f) / 2.0f : 0.0f;
        auto describe = [&](size_t sIdx, float m) {
            const auto& sb = out.sections[sIdx];
            const PartLoudness* me = nullptr; const PartLoudness* ld = nullptr;
            for (auto& p : sb.parts) { if (p.track == Un.ids[0]) me = &p; if (p.lead) ld = &p; }
            std::string r = Un.name + " is " + f1(std::fabs(m)) + " dB too " + (m > 0 ? "quiet" : "loud") + " against the rest of the mix for " + article(out.style) + " " + out.style + " balance";
            if (me && ld && ld != me) {
                const float now = me->lufs - ld->lufs, want = me->target - ld->target;
                r += ": it's " + f1(std::fabs(now)) + " dB " + (now < 0 ? "under " : "over ") + ld->name + " now, and " + out.style + " puts " + mixGroupName(me->group)
                   + " about " + f1(std::fabs(want)) + " dB " + (want < 0 ? "under" : "over") + " the lead";
            } else if (me && me->lead) {
                r += std::string(" with the lead ") + (lead == LeadPlace::OnTop ? "on top" : lead == LeadPlace::Inside ? "inside the wall" : "blended in");
            }
            return r + ".";
        };
        size_t rep = 0; int repBars = -1;
        for (size_t s = 0; s < S; ++s) if (!std::isnan(L[u][s]) && nbars[u][s] > repBars) { repBars = nbars[u][s]; rep = s; }
        if (repBars < 0) continue;
        if (base != 0.0f) {
            LevelMove m; m.track = Un.ids[0]; m.members = Un.ids; m.name = Un.name; m.section = -1; m.gainDb = base;
            m.confidence = std::min(0.9f, 0.5f + std::fabs(base) / 12.0f);
            m.reason = describe(rep, g[u]) + " " + signed1(base) + " dB for the whole song.";
            if (std::fabs(g[u]) > maxMove + 0.5f) m.reason += " It wants about " + f1(std::fabs(g[u])) + " dB: worth fixing at the source (clip gain or the part's level) too.";
            out.moves.push_back(m);
        }
        if (whole) continue;
        for (size_t s = 0; s < S; ++s) {
            if (std::isnan(need[u][s])) continue;
            const float ride = std::round((need[u][s] - base) * 2.0f) / 2.0f;
            if (std::fabs(ride) < 1.5f) continue;
            LevelMove m; m.track = Un.ids[0]; m.members = Un.ids; m.name = Un.name; m.section = out.sections[s].index;
            m.gainDb = std::clamp(ride, -maxMove, maxMove);
            m.confidence = std::min(0.8f, 0.4f + std::fabs(ride) / 12.0f);
            m.reason = "In " + out.sections[s].name + ": " + describe(s, need[u][s]) + " A ride of " + signed1(m.gainDb) + " dB there"
                     + (base != 0.0f ? " (on top of " + signed1(base) + " dB for the song)." : ".");
            out.moves.push_back(m);
        }
    }
    for (auto& sb : out.sections)
        for (auto& p : sb.parts) {
            float applied = 0;
            for (auto& m : out.moves) if (m.track == p.track && (m.section < 0 || m.section == sb.index)) applied += m.gainDb;
            sumAfter += (p.lufs + applied - p.target) * (p.lufs + applied - p.target);
        }
    out.spreadBefore = nSpread ? (float) std::sqrt(sumBefore / nSpread) : 0;
    out.spreadAfter = nSpread ? (float) std::sqrt(sumAfter / nSpread) : 0;
    std::sort(out.moves.begin(), out.moves.end(), [](const LevelMove& a, const LevelMove& b) {
        if ((a.section < 0) != (b.section < 0)) return a.section < 0;
        return std::fabs(a.gainDb) > std::fabs(b.gainDb);
    });
    return out;
}

} // namespace clr
