#include "Planner.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace clr {

namespace {

float clamp01(float x) { return std::fmax(0.0f, std::fmin(1.0f, x)); }

float peakOf(const std::array<float, kBands>& d)
{
    float p = kFloorDb;
    for (float v : d) p = std::fmax(p, v);
    return p;
}

float qFromOctaves(float bw)
{
    const float p = std::pow(2.0f, bw);
    return std::sqrt(p) / (p - 1.0f);
}

std::string hz(float f)
{
    char b[32];
    if (f >= 1000.0f) std::snprintf(b, sizeof b, "%.1f kHz", f / 1000.0f);
    else std::snprintf(b, sizeof b, "%.0f Hz", f);
    return b;
}

std::string fmt1(float v) { char b[16]; std::snprintf(b, sizeof b, "%.1f", v); return b; }

double toPow(float db) { return db <= kFloorDb ? 0.0 : std::pow(10.0, db / 10.0); }
float toDb(double p) { return p <= 1e-14 ? kFloorDb : (float) (10.0 * std::log10(p)); }

// One instrument as the planner sees it: a single track, or a layer of tracks.
struct Unit {
    std::vector<int> members;              // indices into the track list
    std::string name;
    int role = RoleOther;
    float priority = 0, levelDb = kFloorDb, activeSec = 0, aud = 1;
    bool locked = false;                   // every member locked
    std::array<float, kBands> db {};
    int loudest = -1;                      // index of the loudest member
};

void addMerged(std::vector<Change>& list, const Change& c)
{
    for (auto& e : list) {
        if (e.track != c.track || e.type != c.type || e.dynamic != c.dynamic || e.sectionMask != c.sectionMask || e.self != c.self) continue;
        if (std::fabs(std::log2(e.freq / c.freq)) < 0.5f) {
            if (c.gainDb < e.gainDb) e = c; // keep the deeper cut
            return;
        }
    }
    list.push_back(c);
}

} // namespace

PlanResult planFull(const std::vector<TrackMeas>& tracks, const PlannerSettings& s)
{
    auto s_names = [&]() -> const std::vector<std::string>& { return s.sectionNames; };
    PlanResult res;
    const bool orch = s.style == StyleOrchestral;
    // Orchestral writing lives on dynamics and sustained blend: smaller moves, broader curves.
    const float maxCut = orch ? std::fmin(s.maxCutDb, 2.0f) : s.maxCutDb;
    const float maxDip = orch ? std::fmin(s.maxDipDb, 2.0f) : s.maxDipDb;
    const float minBw = orch ? 1.1f : 0.6f;
    const float minDiff = orch ? 0.12f : 0.08f;
    const float octPerBand = std::log2(18000.0f / 25.0f) / (float) kBands;

    // ---- Build units (layers are one instrument) --------------------------------------
    std::vector<Unit> units;
    {
        std::vector<int> layerUnit(64, -1);
        for (int i = 0; i < (int) tracks.size(); ++i) {
            const auto& t = tracks[(size_t) i];
            if (t.activeSec < s.minActiveSec) continue;
            int u = -1;
            if (t.layer > 0 && t.layer < 64) {
                if (layerUnit[(size_t) t.layer] < 0) { layerUnit[(size_t) t.layer] = (int) units.size(); units.push_back({}); }
                u = layerUnit[(size_t) t.layer];
            } else { u = (int) units.size(); units.push_back({}); }
            units[(size_t) u].members.push_back(i);
        }
        for (auto& u : units) {
            std::array<double, kBands> p {};
            double lp = 0;
            u.locked = true;
            float loudLvl = kFloorDb - 1;
            for (int m : u.members) {
                const auto& t = tracks[(size_t) m];
                for (int b = 0; b < kBands; ++b) p[(size_t) b] += toPow(t.db[(size_t) b]);
                lp += toPow(t.levelDb);
                u.priority = std::fmax(u.priority, t.priority);
                u.activeSec = std::fmax(u.activeSec, t.activeSec);
                u.locked = u.locked && t.locked;
                if (t.levelDb > loudLvl) { loudLvl = t.levelDb; u.loudest = m; }
            }
            for (int b = 0; b < kBands; ++b) u.db[(size_t) b] = toDb(p[(size_t) b]);
            u.levelDb = toDb(lp);
            const auto& lead = tracks[(size_t) u.loudest];
            u.role = lead.role;
            u.name = lead.name.empty() ? roleName(lead.role) : lead.name;
            if (u.members.size() > 1) u.name += " layer";
        }
    }
    if (units.empty()) {
        for (const auto& t : tracks)
            if (t.activeSec > 0 && t.activeSec < s.minActiveSec)
                res.notes.push_back((t.name.empty() ? std::string(roleName(t.role)) : t.name) + " was heard for only " + fmt1(t.activeSec)
                                    + " s. It needs 8 s or more, so it was left alone. Listen over a section where it plays.");
        return res;
    }

    // ---- Audibility: parts far below the loudest carry little weight ------------------
    float ref = kFloorDb;
    for (auto& u : units) ref = std::fmax(ref, u.levelDb);
    const bool haveLevels = ref > kFloorDb + 1;
    for (auto& u : units) {
        u.aud = haveLevels ? clamp01((u.levelDb - (ref - 30.0f)) / 15.0f) : 1.0f;
        if (haveLevels && u.aud < 0.15f)
            res.notes.push_back(u.name + " is about " + fmt1(ref - u.levelDb) + " dB below the loudest part, so it barely registers. "
                                "CLEARING won't cut anything to protect it. Mute it, or raise it if it should be heard.");
    }

    std::vector<Change> out; // unit-level changes; Change::track is a unit index here

    // ---- Static masking cuts -----------------------------------------------------------
    for (int hiI = 0; hiI < (int) units.size(); ++hiI) {
        const auto& hi = units[(size_t) hiI];
        if (hi.aud < 0.15f) continue;
        const float hiPeak = peakOf(hi.db);
        for (int loI = 0; loI < (int) units.size(); ++loI) {
            const auto& lo = units[(size_t) loI];
            if (loI == hiI || lo.locked || lo.aud < 0.15f) continue;
            const float diff = hi.priority - lo.priority;
            if (diff < minDiff) continue;
            const float loPeak = peakOf(lo.db);
            const float prio = std::fmin(1.0f, diff / 0.2f);

            std::array<float, kBands> score {};
            int best = -1;
            for (int b = 0; b < kBands; ++b) {
                const float occ = clamp01((hi.db[(size_t) b] - (hiPeak - 20.0f)) / 20.0f);
                const float loOcc = lo.db[(size_t) b] > loPeak - 25.0f ? 1.0f : 0.0f;
                const float mask = clamp01((lo.db[(size_t) b] - (hi.db[(size_t) b] - 6.0f)) / 12.0f);
                score[(size_t) b] = occ * loOcc * mask * prio * hi.aud;
                if (best < 0 || score[(size_t) b] > score[(size_t) best]) best = b;
            }
            if (best < 0 || score[(size_t) best] < 0.25f) continue;

            int a = best, z = best;
            while (a > 0 && score[(size_t) (a - 1)] >= 0.5f * score[(size_t) best]) --a;
            while (z < kBands - 1 && score[(size_t) (z + 1)] >= 0.5f * score[(size_t) best]) ++z;
            double wsum = 0, lsum = 0;
            for (int b = a; b <= z; ++b) { wsum += score[(size_t) b]; lsum += score[(size_t) b] * std::log2(bandCentre(b)); }
            Change c;
            c.track = loI;
            c.type = BandType::Bell;
            c.freq = (float) std::pow(2.0, lsum / wsum);
            c.gainDb = -s.strength * maxCut * std::fmin(1.0f, score[(size_t) best] * 1.4f);
            const float bw = std::fmax(minBw, (float) (z - a + 1) * octPerBand * 1.3f);
            c.q = std::fmin(4.0f, std::fmax(0.7f, qFromOctaves(bw)));
            c.confidence = clamp01(score[(size_t) best]) * (0.5f + 0.5f * std::fmin(1.0f, std::fmin(hi.activeSec, lo.activeSec) / 30.0f));
            c.cause = hi.name;
            if (std::fabs(c.gainDb) < 0.5f) continue;

            const std::string g = fmt1(-c.gainDb);
            if (hi.role == RoleKick && lo.role == RoleBass)
                c.reason = "Bass sits on the kick's main low peak near " + hz(c.freq) + ". A shallow cut of " + g + " dB leaves room for the kick's punch.";
            else
                c.reason = lo.name + " overlaps " + hi.name + " around " + hz(c.freq) + ", and " + hi.name
                           + " is the priority. A " + g + " dB cut there leaves the rest of its tone alone.";
            addMerged(out, c);
        }
    }

    // ---- Orchestral low-mid build-up ---------------------------------------------------
    if (orch) {
        auto region = [](const Unit& u) {
            double p = 0; int n = 0;
            for (int b = 0; b < kBands; ++b) { const float f = bandCentre(b); if (f >= 200 && f <= 400) { p += toPow(u.db[(size_t) b]); ++n; } }
            return n ? toDb(p / n) : kFloorDb;
        };
        float top = kFloorDb;
        for (const auto& u : units) if (u.aud >= 0.15f) top = std::fmax(top, region(u));
        std::vector<int> stack;
        for (int i = 0; i < (int) units.size(); ++i)
            if (units[(size_t) i].aud >= 0.15f && region(units[(size_t) i]) > top - 8.0f) stack.push_back(i);
        if (stack.size() >= 4) {
            std::sort(stack.begin(), stack.end(), [&](int x, int y) { return units[(size_t) x].priority < units[(size_t) y].priority; });
            const size_t share = stack.size() / 2;
            for (size_t i = 0; i < share; ++i) {
                const Unit& u = units[(size_t) stack[i]];
                if (u.locked || u.role == RoleSolo) continue;
                Change c;
                c.track = stack[i]; c.type = BandType::Bell; c.freq = 280; c.q = 0.8f;
                c.gainDb = -s.strength * 1.5f;
                if (c.gainDb > -0.5f) continue;
                c.confidence = 0.55f;
                c.reason = std::to_string(stack.size()) + " parts are stacking around 200-400 Hz. A broad " + fmt1(-c.gainDb)
                           + " dB dip on " + u.name + " shares the clean-up, so no single part is hollowed out.";
                c.cause = "low-mid build-up";
                addMerged(out, c);
            }
        }
    }

    // ---- Vocal air ---------------------------------------------------------------------
    for (int vi = 0; vi < (int) units.size(); ++vi) {
        const auto& v = units[(size_t) vi];
        if (v.role != RoleLeadVocal || v.locked) continue;
        float mid = 0, air = 0; int nm = 0, na = 0;
        for (int b = 0; b < kBands; ++b) {
            const float f = bandCentre(b);
            if (f >= 1000 && f <= 4000) { mid += v.db[(size_t) b]; ++nm; }
            if (f >= 10000 && f <= 16000) { air += v.db[(size_t) b]; ++na; }
        }
        if (!nm || !na) continue;
        mid /= nm; air /= na;
        const float drop = mid - air;
        if (drop < 16.0f || drop > 34.0f) continue;
        bool owned = false;
        for (int oi = 0; oi < (int) units.size(); ++oi) {
            if (oi == vi) continue;
            float oa = 0; int n = 0;
            for (int b = 0; b < kBands; ++b) if (bandCentre(b) >= 10000 && bandCentre(b) <= 16000) { oa += units[(size_t) oi].db[(size_t) b]; ++n; }
            if (n && oa / n > air - 3.0f) owned = true;
        }
        if (owned) continue;
        Change c;
        c.track = vi; c.type = BandType::HighShelf; c.freq = 11000; c.q = 0.7f;
        c.gainDb = s.strength * s.maxBoostDb * clamp01((drop - 16.0f) / 12.0f);
        if (c.gainDb < 0.4f) continue;
        c.confidence = 0.45f;
        c.reason = "The vocal is dull in context above 10 kHz and nothing else occupies that range. A gentle +"
                   + fmt1(c.gainDb) + " dB shelf adds air. Judge it at matched loudness, and drop it if hiss gets louder.";
        addMerged(out, c);
    }

    // ---- Focus-following dips ("movement") and section leads ---------------------------
    std::vector<Change> dips;
    auto unitOf = [&](int trackId) {
        for (int i = 0; i < (int) units.size(); ++i)
            for (int m : units[(size_t) i].members) if (tracks[(size_t) m].id == trackId) return i;
        return -1;
    };
    auto sectionList = [&](uint32_t mask) {
        std::string s;
        for (int k = 0; k < 32; ++k)
            if (mask >> k & 1u) {
                if (!s.empty()) s += ", ";
                s += k < (int) s_names().size() ? s_names()[(size_t) k] : "section " + std::to_string(k + 1);
            }
        return s;
    };
    // The leader's character region, the followers that crowd it, and one dip each.
    auto makeDips = [&](int fi, const TrackMeas& leader, uint32_t mask, bool sectionLead) {
        const Unit& f = units[(size_t) fi];
        const float lo = sectionLead ? 30.0f : 150.0f;
        float fpk = kFloorDb;
        for (int b = 0; b < kBands; ++b) if (bandCentre(b) >= lo && bandCentre(b) <= 6000) fpk = std::fmax(fpk, f.db[(size_t) b]);
        double wsum = 0, lsum = 0;
        std::vector<int> region;
        for (int b = 0; b < kBands; ++b) {
            const float fc = bandCentre(b);
            if (fc < lo || fc > 6000 || f.db[(size_t) b] < fpk - 6.0f) continue;
            const double w = toPow(f.db[(size_t) b]);
            wsum += w; lsum += w * std::log2(fc); region.push_back(b);
        }
        if (wsum <= 0) return;
        const float centre = (float) std::pow(2.0, lsum / wsum);
        const bool shelf = centre < 200.0f; // a low-end lead owns everything below it
        if (shelf) { region.clear(); for (int b = 0; b < kBands; ++b) if (bandCentre(b) >= 30 && bandCentre(b) <= std::fmax(150.0f, centre * 2.5f)) region.push_back(b); }
        const std::string where = mask ? " in " + sectionList(mask) : "";
        for (int i = 0; i < (int) units.size(); ++i) {
            if (i == fi) continue;
            const auto& u = units[(size_t) i];
            if (u.locked || u.aud < 0.15f) continue;
            if (!sectionLead && (f.priority - u.priority < minDiff || u.role == RoleSolo)) continue;
            if (sectionLead && shelf && (u.role == RoleKick || u.role == RoleSnare)) continue; // the drums share the floor with the bass
            const float uPk = peakOf(u.db);
            float occ = 0, mask2 = 0;
            for (int b : region) {
                occ = std::fmax(occ, clamp01((u.db[(size_t) b] - (uPk - 18.0f)) / 12.0f));
                mask2 = std::fmax(mask2, clamp01((u.db[(size_t) b] - (f.db[(size_t) b] - 10.0f)) / 12.0f));
            }
            const float amt = occ * mask2;
            Change c;
            c.track = i;
            c.type = shelf ? BandType::LowShelf : BandType::Bell;
            c.freq = shelf ? std::fmin(220.0f, std::fmax(100.0f, centre * 2.5f)) : centre;
            c.q = shelf ? 0.7f : 0.9f;
            c.gainDb = -s.strength * maxDip * std::fmin(1.0f, amt * 1.3f);
            if (c.gainDb > -0.5f) continue;
            c.dynamic = true;
            c.leader = leader.id;
            c.threshDb = leader.levelDb - 12.0f;
            c.sectionMask = mask;
            c.confidence = clamp01(0.7f * amt);
            c.cause = f.name;
            if (shelf)
                c.reason = f.name + " leads the low end" + where + ". " + u.name + " eases " + fmt1(-c.gainDb) + " dB below "
                           + hz(c.freq) + " only while it plays, so the bass owns the floor.";
            else if (sectionLead)
                c.reason = f.name + " leads" + where + ". " + u.name + " dips " + fmt1(-c.gainDb) + " dB around " + hz(c.freq)
                           + " only while it plays.";
            else
                c.reason = "Dips " + fmt1(-c.gainDb) + " dB around " + hz(c.freq) + " only while " + f.name
                           + " plays" + where + ", then comes back, the way players back off for a soloist. Movement scales it.";
            dips.push_back(c);
        }
    };
    if (s.dynamics) {
        // Sections that name their own lead, grouped by lead.
        uint32_t ledMask = 0;
        std::vector<std::pair<int, uint32_t>> leads; // track id, sections
        for (int k = 0; k < (int) s.sectionLeads.size() && k < 32; ++k) {
            const int L = s.sectionLeads[(size_t) k];
            if (L < 0) continue;
            ledMask |= 1u << k;
            bool found = false;
            for (auto& pr : leads) if (pr.first == L) { pr.second |= 1u << k; found = true; }
            if (!found) leads.push_back({ L, 1u << k });
        }
        // The overall focus, everywhere a section doesn't name its own lead.
        uint32_t focusMask = 0;
        if (ledMask) for (int k = 0; k < (int) s.sectionLeads.size() && k < 32; ++k) if (!(ledMask >> k & 1u)) focusMask |= 1u << k;
        int fi = -1;
        for (int i = 0; i < (int) units.size(); ++i) {
            const auto& u = units[(size_t) i];
            if (u.aud < 0.15f) continue;
            if (fi < 0 || u.priority > units[(size_t) fi].priority + 1e-4f
                || (std::fabs(u.priority - units[(size_t) fi].priority) < 1e-4f && u.levelDb > units[(size_t) fi].levelDb)) fi = i;
        }
        if (fi >= 0 && (!ledMask || focusMask)) makeDips(fi, tracks[(size_t) units[(size_t) fi].loudest], focusMask, false);
        for (auto& [L, m] : leads) {
            const int ui = unitOf(L);
            if (ui < 0) { res.notes.push_back("The lead for " + sectionList(m) + " wasn't heard while listening, so nothing follows it yet. Listen over that section."); continue; }
            const TrackMeas* lt = nullptr;
            for (const auto& t : tracks) if (t.id == L) lt = &t;
            makeDips(ui, *lt, m, true);
        }
    }

    // ---- Self-triggered cuts: a part tames its own build-up --------------------------
    std::vector<Change> selfs; // Change::track is the TrackMeas index here
    if (s.selfDynamics) {
        for (int ti = 0; ti < (int) tracks.size(); ++ti) {
            const auto& t = tracks[(size_t) ti];
            if (t.locked || t.activeSec < s.minActiveSec) continue;
            const int ui = unitOf(t.id);
            if (ui < 0 || units[(size_t) ui].aud < 0.15f) continue;
            bool hasPeaks = false; // no burst data (an old caller): never guess
            for (int b = 0; b < kBands; ++b) if (t.peakDb[(size_t) b] != 0.0f && t.peakDb[(size_t) b] > kFloorDb + 1) hasPeaks = true;
            if (!hasPeaks) continue;
            struct Zone { float lo, hi; const char* what; };
            std::vector<Zone> zones;
            switch (t.role) {
            case RolePiano: case RoleAcousticGtr: case RoleBass: case RoleCellos: case RoleLowBrass: case RoleDoubleBasses:
                zones.push_back({ 150, 400, "boom" }); break;
            case RoleLeadVocal: case RoleBackVocal: case RoleChoir:
                zones.push_back({ 5000, 9000, "sibilance" }); break;
            case RoleTrumpets: case RoleHorns: case RoleViolins: case RoleViolas: case RoleWoodwinds: case RoleElectricGtr: case RoleSolo:
                zones.push_back({ 2000, 5000, "harshness" }); break;
            case RoleSynth: case RoleOther:
                zones.push_back({ 150, 400, "boom" }); zones.push_back({ 2000, 5000, "harshness" }); break;
            default: break;
            }
            if (zones.empty()) continue;
            const float pk = peakOf(t.db);
            std::vector<float> crest;
            for (int b = 0; b < kBands; ++b) if (t.db[(size_t) b] > pk - 30.0f) crest.push_back(t.peakDb[(size_t) b] - t.db[(size_t) b]);
            if (crest.size() < 6) continue;
            std::nth_element(crest.begin(), crest.begin() + (long) crest.size() / 2, crest.end());
            const float med = crest[crest.size() / 2];
            float bestEx = 0; int bestB = -1; const char* bestWhat = "";
            for (auto& z : zones)
                for (int b = 0; b < kBands; ++b) {
                    const float fc = bandCentre(b);
                    if (fc < z.lo || fc > z.hi || t.db[(size_t) b] < pk - 15.0f) continue;
                    const float ex = (t.peakDb[(size_t) b] - t.db[(size_t) b]) - med;
                    if (ex > bestEx) { bestEx = ex; bestB = b; bestWhat = z.what; }
                }
            if (bestB < 0 || bestEx < 4.0f) continue;
            Change c;
            c.track = ti; c.type = BandType::Bell; c.freq = bandCentre(bestB); c.q = 1.4f;
            c.gainDb = -s.strength * maxDip * std::fmin(1.0f, (bestEx - 2.0f) / 6.0f);
            if (c.gainDb > -0.5f) continue;
            c.self = true; c.dynamic = true; c.leader = t.id;
            c.confidence = clamp01(0.3f + (bestEx - 4.0f) / 10.0f);
            c.cause = "itself";
            c.reason = "Tames " + std::string(bestWhat) + " around " + hz(c.freq) + " only when it builds up: there it jumps " + fmt1(bestEx)
                       + " dB further above its usual level than the rest of the part does. Untouched the rest of the time.";
            selfs.push_back(c);
        }
    }

    // ---- Per unit: strongest static changes, plus dips; expand to members -------------
    std::vector<int> leadUnits;
    for (int L : s.sectionLeads) if (L >= 0) { const int u = unitOf(L); if (u >= 0) leadUnits.push_back(u); }
    for (int ui = 0; ui < (int) units.size(); ++ui) {
        std::vector<Change> mine;
        const bool isLead = std::find(leadUnits.begin(), leadUnits.end(), ui) != leadUnits.end();
        for (const auto& c : out) if (c.track == ui && !isLead) mine.push_back(c); // a section lead is never cut statically
        std::sort(mine.begin(), mine.end(), [](const Change& x, const Change& y) { return std::fabs(x.gainDb) > std::fabs(y.gainDb); });
        if ((int) mine.size() > s.maxPerTrack) mine.resize((size_t) s.maxPerTrack);
        int nd = 0;
        for (const auto& d : dips) if (d.track == ui && nd < 2) { mine.push_back(d); ++nd; }
        const auto& u = units[(size_t) ui];
        for (auto c : mine)
            for (int m : u.members) {
                if (tracks[(size_t) m].locked) continue;
                Change e = c;
                e.track = tracks[(size_t) m].id;
                if (u.members.size() > 1) e.reason = "(Whole layer, so it stays one sound.) " + e.reason;
                res.changes.push_back(e);
            }
    }
    for (auto c : selfs) { c.track = tracks[(size_t) c.track].id; res.changes.push_back(c); }

    // ---- Layer report ------------------------------------------------------------------
    for (const auto& u : units) {
        if (u.members.size() < 2) continue;
        for (int m : u.members) {
            const auto& t = tracks[(size_t) m];
            const float rel = t.levelDb - u.levelDb;
            if (rel < -20.0f)
                res.notes.push_back(t.name + " sits " + fmt1(-rel) + " dB below the rest of " + u.name
                                    + " and adds almost nothing. Mute it, or bring it up if it's meant to be heard.");
        }
        for (size_t a = 0; a < u.members.size(); ++a)
            for (size_t b = a + 1; b < u.members.size(); ++b) {
                const auto& x = tracks[(size_t) u.members[a]];
                const auto& y = tracks[(size_t) u.members[b]];
                if (std::fabs(x.levelDb - y.levelDb) > 6.0f) continue;
                const float px = peakOf(x.db), py = peakOf(y.db);
                double d = 0; int n = 0;
                for (int k = 0; k < kBands; ++k) {
                    if (x.db[(size_t) k] < px - 40.0f && y.db[(size_t) k] < py - 40.0f) continue;
                    d += std::fabs((x.db[(size_t) k] - px) - (y.db[(size_t) k] - py)); ++n;
                }
                if (n >= 8 && d / n < 1.2)
                    res.notes.push_back(x.name + " and " + y.name + " have near-identical spectra. If they play the same notes, "
                                        "the doubling can comb-filter and sound phasey. Try detuning, offsetting or thinning one.");
            }
    }
    for (const auto& t : tracks)
        if (t.activeSec > 0 && t.activeSec < s.minActiveSec)
            res.notes.push_back((t.name.empty() ? std::string(roleName(t.role)) : t.name) + " was heard for only " + fmt1(t.activeSec)
                                + " s. It needs 8 s or more, so it was left alone. Listen over a section where it plays.");
    return res;
}

} // namespace clr
