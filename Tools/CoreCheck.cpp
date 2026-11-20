// JUCE-free checks for the analysis and planning core.
#include "Core/Balance.h"
#include "Core/Harmonics.h"
#include "Core/Health.h"
#include "Core/MixBalance.h"
#include "Core/Planner.h"
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace clr;

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++fails; std::printf("FAIL: %s\n", msg); } } while (0)

static const double FS = 48000.0;

// Synthetic parts. Deterministic noise so runs repeat exactly.
static std::vector<float> kickTrack(double sec)
{
    std::vector<float> x((size_t) (sec * FS), 0.0f);
    const int period = (int) (FS * 0.5);
    for (size_t start = 0; start < x.size(); start += period)
        for (int i = 0; i < (int) (0.25 * FS) && start + i < x.size(); ++i) {
            const double t = i / FS;
            const double f = 50.0 + 40.0 * std::exp(-t * 40.0);
            static double ph = 0; ph += 2 * 3.14159265358979 * f / FS;
            x[start + i] += (float) (0.8 * std::sin(ph) * std::exp(-t * 12.0) + 0.1 * std::exp(-t * 500.0) * ((i * 7919) % 200 / 100.0 - 1.0));
        }
    return x;
}
static std::vector<float> harmonic(double sec, double f0, double tilt, double gain, double airDb)
{
    std::vector<float> x((size_t) (sec * FS), 0.0f);
    for (int h = 1; h * f0 < 18000; ++h) {
        double amp = gain / std::pow((double) h, tilt);
        if (h * f0 > 10000) amp *= std::pow(10.0, airDb / 20.0);
        for (size_t i = 0; i < x.size(); ++i) x[i] += (float) (amp * std::sin(2 * 3.14159265358979 * h * f0 * i / FS + h));
    }
    return x;
}

static TrackMeas measure(int id, const char* name, int role, float prio, const std::vector<float>& x)
{
    Analyser a; a.prepare(FS); a.resetCapture();
    for (size_t i = 0; i + 512 <= x.size(); i += 512) a.push(&x[i], 512, true);
    TrackMeas t; t.id = id; t.name = name; t.role = role; t.priority = prio;
    t.db = a.captureDb(); t.activeSec = a.activeSec(); t.levelDb = a.captureRmsDb(); t.peakDb = a.capturePeakDb();
    return t;
}

int main()
{
    // 1. Analyser puts a sine in the right band, at a believable level.
    {
        std::vector<float> s((size_t) (2 * FS));
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float) (0.5 * std::sin(2 * 3.14159265358979 * 1000.0 * i / FS));
        auto t = measure(0, "sine", RoleOther, 0.4f, s);
        int pk = 0; for (int b = 1; b < kBands; ++b) if (t.db[b] > t.db[pk]) pk = b;
        CHECK(std::fabs(bandCentre(pk) - 1000.0f) < 120.0f, "sine peaks near 1 kHz");
        CHECK(t.activeSec > 1.5f, "active seconds counted");
    }
    // 2. Silence is gated out of the capture.
    {
        std::vector<float> s((size_t) (2 * FS), 0.0f);
        auto t = measure(0, "silence", RoleOther, 0.4f, s);
        CHECK(t.activeSec == 0.0f, "silence not counted as active");
    }
    // 3. Biquad: bell hits its gain at centre, shelf at the top.
    {
        auto c = makeCoeffs(BandType::Bell, FS, 1000, -3.0, 1.4);
        CHECK(std::fabs(magnitudeDb(c, FS, 1000) + 3.0) < 0.05, "bell gain at centre");
        CHECK(std::fabs(magnitudeDb(c, FS, 100)) < 0.3, "bell flat far away");
        auto h = makeCoeffs(BandType::HighShelf, FS, 11000, 2.0, 0.7);
        CHECK(std::fabs(magnitudeDb(h, FS, 18000) - 2.0) < 0.3, "shelf reaches gain at top");
        Biquad bq; bq.set(c);
        double e0 = 0, e1 = 0;
        for (int i = 0; i < 48000; ++i) { float in = (float) std::sin(2 * 3.14159265358979 * 1000 * i / FS); float o = bq.process(in); if (i > 4800) { e0 += in * in; e1 += o * o; } }
        CHECK(std::fabs(10 * std::log10(e1 / e0) + 3.0) < 0.1, "time-domain bell matches response");
    }
    // 4. A mix: kick, bass sitting on the kick, vocal, guitar.
    {
        auto kick = measure(0, "Kick", RoleKick, roleDefaultPriority(RoleKick), kickTrack(40));
        auto bass = measure(1, "Bass", RoleBass, roleDefaultPriority(RoleBass), harmonic(40, 65.0, 1.2, 0.35, 0));
        auto vox  = measure(2, "Vocal", RoleLeadVocal, 0.95f, harmonic(40, 220.0, 1.0, 0.04, -12));
        auto gtr  = measure(3, "Guitar", RoleElectricGtr, 0.45f, harmonic(40, 196.0, 0.9, 0.04, 0));
        PlannerSettings s;
        auto changes = plan({ kick, bass, vox, gtr }, s);
        for (auto& c : changes) std::printf("  track %d %-6s %.0f Hz %+.1f dB Q%.1f conf %.2f  %s\n", c.track, c.type == BandType::Bell ? "bell" : "shelf", c.freq, c.gainDb, c.q, c.confidence, c.reason.c_str());

        bool bassCut = false, kickTouched = false, voxCut = false;
        for (auto& c : changes) {
            if (c.track == 1 && c.gainDb < 0 && c.freq > 40 && c.freq < 130) bassCut = true;
            if (c.track == 0 && !c.dynamic) kickTouched = true;
            if (c.track == 2 && c.gainDb < 0) voxCut = true;
            CHECK(c.gainDb >= -s.maxCutDb - 1e-3f && c.gainDb <= s.maxBoostDb + 1e-3f, "change within limits");
        }
        CHECK(bassCut, "bass is cut in the kick's low region");
        CHECK(!kickTouched, "priority kick gets no static cut");
        CHECK(!voxCut, "lead vocal is never cut to make room for lesser parts");
        int perTrack[4] {}, dyn[4] {}; for (auto& c : changes) (c.dynamic ? dyn : perTrack)[c.track]++;
        for (int i = 0; i < 4; ++i) CHECK(perTrack[i] <= s.maxPerTrack && dyn[i] <= 1, "per-track cap respected (3 static + 1 dip)");
        bool dip = false; for (auto& c : changes) if (c.dynamic) { dip = true; CHECK(c.leader == 2 && c.threshDb > -100, "dip follows the lead vocal with a real threshold"); }
        CHECK(dip, "focus-following dip proposed");

        // Locked track: never changed.
        bass.locked = true;
        auto locked = plan({ kick, bass, vox, gtr }, s);
        bool any = false; for (auto& c : locked) if (c.track == 1) any = true;
        CHECK(!any, "locked track untouched");

        // Strength 0: nothing worth doing.
        PlannerSettings z; z.strength = 0.0f;
        CHECK(plan({ kick, bass, vox, gtr }, z).empty(), "strength 0 makes no changes");

        // Too little audio captured: leave alone.
        auto brief = bass; brief.activeSec = 2; brief.locked = false;
        auto few = plan({ kick, brief, vox, gtr }, s);
        any = false; for (auto& c : few) if (c.track == 1) any = true;
        CHECK(!any, "short capture leaves the track alone");

        // Determinism.
        auto again = plan({ kick, bass, vox, gtr }, s);
        auto again2 = plan({ kick, bass, vox, gtr }, s);
        CHECK(again.size() == again2.size(), "planning is deterministic");
    }
    // 4b. Orchestral: four sustained sections stacking low-mid; solo untouched; cuts stay gentle.
    {
        auto solo = measure(0, "Solo Oboe", RoleSolo, roleDefaultPriority(RoleSolo), harmonic(40, 440.0, 1.0, 0.05, -10));
        auto vln  = measure(1, "Violins", RoleViolins, roleDefaultPriority(RoleViolins), harmonic(40, 330.0, 0.8, 0.05, -6));
        auto vla  = measure(2, "Violas", RoleViolas, roleDefaultPriority(RoleViolas), harmonic(40, 250.0, 0.8, 0.05, -6));
        auto hrn  = measure(3, "Horns", RoleHorns, roleDefaultPriority(RoleHorns), harmonic(40, 262.0, 0.8, 0.05, -10));
        auto cel  = measure(4, "Cellos", RoleCellos, roleDefaultPriority(RoleCellos), harmonic(40, 196.0, 0.8, 0.05, -6));
        PlannerSettings o; o.style = StyleOrchestral;
        auto ch = plan({ solo, vln, vla, hrn, cel }, o);
        for (auto& c : ch) std::printf("  [orch] track %d %.0f Hz %+.1f dB Q%.1f  %s\n", c.track, c.freq, c.gainDb, c.q, c.reason.c_str());
        bool soloTouched = false; float deepest = 0;
        for (auto& c : ch) { if (c.track == 0 && c.gainDb < 0) soloTouched = true; deepest = std::fmin(deepest, c.gainDb); }
        CHECK(!soloTouched, "orchestral: solo line never cut");
        CHECK(deepest >= -2.0f - 1e-3f, "orchestral: cuts capped at 2 dB");
        CHECK(!ch.empty(), "orchestral: stacked sections get a shared clean-up");
        for (auto& c : ch) CHECK(c.q <= 1.6f || c.type != BandType::Bell, "orchestral: curves stay broad");
    }
    // 6. Layers, level weighting and the layer report.
    {
        auto grandA = measure(0, "Grand A", RolePiano, 0.85f, harmonic(30, 196.0, 0.9, 0.06, -12));
        auto grandB = measure(1, "Grand B", RolePiano, 0.85f, harmonic(30, 262.0, 0.9, 0.002, -12)); // ~30 dB down
        auto grandC = measure(2, "Grand C", RolePiano, 0.85f, harmonic(30, 196.0, 0.9, 0.05, -12));  // same spectrum as A
        auto str1 = measure(3, "Strings 1", RoleViolins, 0.6f, harmonic(30, 247.0, 0.8, 0.06, -6));
        auto str2 = measure(4, "Strings 2", RoleViolins, 0.6f, harmonic(30, 233.0, 0.8, 0.05, -6));
        grandA.layer = grandB.layer = grandC.layer = 1;
        str1.layer = str2.layer = 2;
        PlannerSettings o; o.style = StyleOrchestral;
        auto r = planFull({ grandA, grandB, grandC, str1, str2 }, o);
        for (auto& c : r.changes) std::printf("  [layers] track %d %.0f Hz %+.1f dB%s  %s\n", c.track, c.freq, c.gainDb, c.dynamic ? " (dip)" : "", c.reason.c_str());
        for (auto& n : r.notes) std::printf("  [note] %s\n", n.c_str());
        bool pianoCut = false; float g3 = 99, g4 = 99; int n3 = 0, n4 = 0;
        for (auto& c : r.changes) {
            if (c.track <= 2) pianoCut = true;
            if (c.track == 3) { ++n3; g3 = c.gainDb; }
            if (c.track == 4) { ++n4; g4 = c.gainDb; }
        }
        CHECK(!pianoCut, "layers: no piano layer is cut against another");
        CHECK(n3 > 0 && n3 == n4 && std::fabs(g3 - g4) < 1e-4f, "layers: the string layer is corrected as one (same moves on both)");
        bool quietNote = false, twinNote = false;
        for (auto& n : r.notes) { if (n.find("Grand B") != std::string::npos && n.find("adds almost nothing") != std::string::npos) quietNote = true;
                                  if (n.find("near-identical") != std::string::npos) twinNote = true; }
        CHECK(quietNote, "layer report flags the near-silent layer");
        CHECK(twinNote, "layer report flags near-identical doubled layers");

        // Ungrouped quiet protector: drives nothing.
        auto quiet = grandB; quiet.layer = 0; quiet.priority = 0.95f; quiet.name = "Quiet Piano";
        auto s1 = str1; s1.layer = 0;
        auto r2 = planFull({ quiet, s1, grandA }, o);
        bool causedByQuiet = false;
        for (auto& c : r2.changes) if (c.cause.find("Quiet Piano") != std::string::npos) causedByQuiet = true;
        CHECK(!causedByQuiet, "a part far below the mix protects nothing");

        // Unheard part is reported, not touched.
        auto brief = s1; brief.activeSec = 2.5f; brief.id = 9; brief.name = "Deep Bass";
        auto r3 = planFull({ grandA, brief }, o);
        bool unheard = false; for (auto& n : r3.notes) if (n.find("Deep Bass") != std::string::npos) unheard = true;
        CHECK(unheard, "a part heard too briefly is reported");
    }
    // 7. Section leads: a bass synth owns the low end in section 2; the focus rules section 1.
    {
        auto up   = measure(0, "Upright", RoleSolo, 0.95f, harmonic(30, 330.0, 1.0, 0.05, -10));
        auto g1   = measure(1, "Grand 1", RolePiano, 0.85f, harmonic(30, 98.0, 0.7, 0.06, -12));
        auto g2   = measure(2, "Grand 2", RolePiano, 0.85f, harmonic(30, 131.0, 0.7, 0.05, -12));
        auto bass = measure(3, "Downshift", RoleBass, 0.65f, harmonic(30, 41.0, 1.6, 0.4, -40));
        auto str  = measure(4, "Strings", RoleViolins, 0.6f, harmonic(30, 330.0, 0.8, 0.07, -6));
        g1.layer = g2.layer = 1;
        PlannerSettings o; o.style = StyleOrchestral; o.selfDynamics = false;
        o.sectionLeads = { -1, 3 }; o.sectionNames = { "Orchestral", "Downshift Static" };
        auto r = planFull({ up, g1, g2, bass, str }, o);
        for (auto& c : r.changes) std::printf("  [leads] track %d %s %.0f Hz %+.1f dB mask %u leader %d  %s\n", c.track,
            c.type == BandType::LowShelf ? "lowshelf" : "bell", c.freq, c.gainDb, c.sectionMask, c.leader, c.reason.c_str());
        bool pianoShelf = false, focusMasked = true, anyFocus = false, bassTouchedInOwn = false;
        float g1g = 0, g2g = 1;
        for (auto& c : r.changes) {
            if (!c.dynamic) continue;
            if (c.leader == 3 && c.sectionMask == 2u && c.type == BandType::LowShelf && (c.track == 1 || c.track == 2)) pianoShelf = true;
            if (c.leader == 3 && c.track == 1) g1g = c.gainDb;
            if (c.leader == 3 && c.track == 2) g2g = c.gainDb;
            if (c.leader == 0) { anyFocus = true; if (c.sectionMask != 1u) focusMasked = false; }
            if (c.track == 3 && c.sectionMask == 2u) bassTouchedInOwn = true;
        }
        CHECK(pianoShelf, "section lead: the piano layer gives the low end to the bass, only in its section");
        CHECK(g1g == g2g, "section lead: the layer eases back as one");
        CHECK(anyFocus && focusMasked, "the overall focus still leads, only in the sections without their own lead");
        CHECK(!bassTouchedInOwn, "the section's lead is never dipped in its own section");
    }
    // 8. Self-triggered cuts: a part that builds up in bursts at 3 kHz gets one; a steady part doesn't.
    {
        std::vector<float> steady = harmonic(30, 220.0, 0.9, 0.05, -6);
        std::vector<float> bursty = steady;
        for (size_t s0 = 0; s0 < bursty.size(); s0 += (size_t) (2.0 * FS))
            for (size_t i = 0; i < (size_t) (0.35 * FS) && s0 + i < bursty.size(); ++i)
                bursty[s0 + i] += (float) (0.12 * std::sin(2 * 3.14159265358979 * 3100.0 * (double) i / FS));
        auto calm = measure(0, "Steady Brass", RoleHorns, 0.6f, steady);
        auto hot = measure(1, "Hot Brass", RoleHorns, 0.6f, bursty);
        PlannerSettings o; o.dynamics = false;
        auto r1 = planFull({ calm }, o), r2 = planFull({ hot }, o);
        bool calmSelf = false; const Change* hs = nullptr;
        for (auto& c : r1.changes) if (c.self) calmSelf = true;
        for (auto& c : r2.changes) if (c.self) hs = &c;
        if (hs) std::printf("  [self] %.0f Hz %+.1f dB  %s\n", hs->freq, hs->gainDb, hs->reason.c_str());
        CHECK(!calmSelf, "self-triggered: a steady part is left alone");
        CHECK(hs && hs->freq > 2400 && hs->freq < 4000 && hs->leader == 1 && hs->dynamic, "self-triggered: bursts at 3 kHz get a cut that follows the part itself");
        TrackMeas noPeaks = hot; noPeaks.peakDb = {};
        bool np = false; for (auto& c : planFull({ noPeaks }, o).changes) if (c.self) np = true;
        CHECK(!np, "self-triggered: never proposed without burst data");
    }
    // 9. Timelines: level fit from the master, section detection, balance moves.
    {
        auto mk = [](int id, const char* name, int role, float prio) { TrackTimeline t; t.id = id; t.name = name; t.role = role; t.priority = prio;
                                                                       for (auto& r : t.db) r.fill(kFloorDb); return t; };
        auto up = mk(0, "Upright", RoleSolo, 0.95f), str = mk(1, "Strings", RoleViolins, 0.6f), bass = mk(2, "Bass", RoleBass, 0.65f);
        for (int b = 0; b < 32; ++b) {
            up.db[(size_t) b][3] = -30.0f; up.db[(size_t) b][2] = -40.0f;
            str.db[(size_t) b][2] = -26.0f + (float) (b % 3); str.db[(size_t) b][3] = -24.0f + (float) (b % 2);
            if (b >= 16) { bass.db[(size_t) b][1] = -20.0f; bass.db[(size_t) b][0] = -24.0f + (float) (b % 4); }
        }
        // the stereo out: Upright at its insert level, Strings 3 dB down, Bass 3 dB up
        Timeline master; for (auto& r : master) r.fill(kFloorDb);
        auto lin = [](float d) { return d <= kFloorDb + 1 ? 0.0 : std::pow(10.0, d / 10.0); };
        for (int b = 0; b < 32; ++b) for (int k = 0; k < kCoarse; ++k) {
            const double p = lin(up.db[(size_t) b][(size_t) k]) + 0.5 * lin(str.db[(size_t) b][(size_t) k]) + 2.0 * lin(bass.db[(size_t) b][(size_t) k]);
            master[(size_t) b][(size_t) k] = p > 0 ? (float) (10 * std::log10(p)) : kFloorDb;
        }
        auto fit = estimateLevels({ up, str, bass }, master);
        std::printf("  [fit] Upright %+.2f  Strings %+.2f  Bass %+.2f dB  r2 %.3f over %d bars\n", fit.offsetDb[0], fit.offsetDb[1], fit.offsetDb[2], fit.r2, fit.bars);
        CHECK(std::fabs(fit.offsetDb[0]) < 0.3f && std::fabs(fit.offsetDb[1] + 3.0f) < 0.3f && std::fabs(fit.offsetDb[2] - 3.0f) < 0.3f && fit.r2 > 0.95f,
              "level fit recovers each track's level at the stereo out");
        // Ill-conditioned: two parts with almost the same spectrum, told apart only by when they play.
        {
            auto v = mk(3, "Vox", RoleLeadVocal, 0.9f), g = mk(4, "Gtr", RoleElectricGtr, 0.45f);
            Timeline m2; for (auto& r : m2) r.fill(kFloorDb);
            for (int b = 0; b < 24; ++b) {
                const bool vOn = b < 16, gOn = b < 8 || b >= 16;
                for (int k = 2; k <= 4; ++k) {
                    if (vOn) v.db[(size_t) b][(size_t) k] = -24.0f - 2.0f * (float) (k - 2);
                    if (gOn) g.db[(size_t) b][(size_t) k] = -24.5f - 2.0f * (float) (k - 2) + (k == 4 ? 0.3f : 0.0f);
                    const double p = 2.0 * lin(v.db[(size_t) b][(size_t) k]) + 0.5 * lin(g.db[(size_t) b][(size_t) k]);
                    m2[(size_t) b][(size_t) k] = p > 0 ? (float) (10 * std::log10(p)) : kFloorDb;
                }
            }
            auto f2 = estimateLevels({ v, g }, m2);
            std::printf("  [fit, near-identical spectra] Vox %+.2f  Gtr %+.2f dB\n", f2.offsetDb[0], f2.offsetDb[1]);
            CHECK(std::fabs(f2.offsetDb[0] - 3.01f) < 0.1f && std::fabs(f2.offsetDb[1] + 3.01f) < 0.1f, "level fit is exact even when two parts sound alike");
        }
        auto secs = detectSections({ up, str, bass }, 32);
        for (auto& s : secs) std::printf("  [sections] %s bars %d-%d (%zu playing)\n", s.name.c_str(), s.start, s.end, s.active.size());
        CHECK(secs.size() == 2 && secs[0].start == 1 && secs[1].start == 17 && secs[1].end == 32, "sections detected where the bass comes in");
        std::vector<std::string> notes;
        auto mv = planBalance({ up, str, bass }, secs, StyleBand, &notes);
        for (auto& m : mv) std::printf("  [balance] track %d section %d %+.1f dB  %s\n", m.track, m.section, m.gainDb, m.reason.c_str());
        bool upRaise = false, strCut = false;
        for (auto& m : mv) { if (m.track == 0 && m.gainDb > 1.5f && m.gainDb <= 2.0f) upRaise = true; if (m.track == 1 && m.gainDb < 0) strCut = true; }
        CHECK(upRaise, "balance: the buried lead is raised (at most +2 dB)");
        CHECK(strCut, "balance: the biggest masker gives way when +2 dB isn't enough");
        bool merged = true; for (auto& m : mv) if (m.section != -1) merged = false;
        CHECK(merged, "balance: the same move in every section becomes one whole-song move");
        auto lockedUp = up; lockedUp.locked = true;
        bool any = false; for (auto& m : planBalance({ lockedUp, str, bass }, secs, StyleBand)) if (m.track == 0) any = true;
        CHECK(!any, "balance: a locked track is never moved");
        // A lead that's already clear gets nothing.
        auto loud = up; for (int b = 0; b < 32; ++b) loud.db[(size_t) b][3] = -18.0f;
        bool none = true; for (auto& m : planBalance({ loud, str, bass }, secs, StyleBand)) if (m.track == 0) none = false;
        CHECK(none, "balance: a lead that already holds its band is left alone");
    }

    // 10. Mix health on the stereo out.
    {
        const double PI = 3.14159265358979;
        HealthAnalyser h; h.prepare(FS);
        // 4 s of a 1 kHz sine at amplitude 0.1 in both channels: -23 LUFS, mono-safe.
        std::vector<float> L((size_t) (4 * FS)), R;
        for (size_t i = 0; i < L.size(); ++i) L[i] = (float) (0.1 * std::sin(2 * PI * 997.0 * i / FS));
        R = L;
        for (size_t i = 0; i + 512 <= L.size(); i += 512) h.process(&L[i], &R[i], 512, 1 + (int) (i / (size_t) (2 * FS)));
        const float I = h.integratedLufs();
        std::printf("  [health] 997 Hz at -20 dBFS (amplitude 0.1) both channels: %.2f LUFS, max short-term %.2f, corr mid %.2f, mono loss %.2f dB\n",
                    I, h.maxShortTermLufs(), HealthAnalyser::correlation(h.bar(1), 3), HealthAnalyser::monoLossDb(h.bar(1), 3));
        CHECK(std::fabs(I + 20.0f) < 0.5f, "integrated loudness of a known sine (-20 LUFS: 0 dBFS in both channels is 0 LUFS)");
        CHECK(HealthAnalyser::correlation(h.bar(1), 3) > 0.99f && std::fabs(HealthAnalyser::monoLossDb(h.bar(1), 3)) < 0.1f, "identical channels: correlation 1, nothing lost in mono");
        // Sub out of phase between the channels, mid in phase.
        HealthAnalyser p; p.prepare(FS);
        std::vector<float> l2((size_t) (2 * FS)), r2(l2.size());
        for (size_t i = 0; i < l2.size(); ++i) {
            const float sub = (float) (0.3 * std::sin(2 * PI * 45.0 * i / FS)), mid = (float) (0.1 * std::sin(2 * PI * 800.0 * i / FS));
            l2[i] = sub + mid; r2[i] = -sub + mid;
        }
        for (size_t i = 0; i + 512 <= l2.size(); i += 512) p.process(&l2[i], &r2[i], 512, 1);
        const float cs = HealthAnalyser::correlation(p.bar(1), 0), cm = HealthAnalyser::correlation(p.bar(1), 3);
        std::printf("  [health] sub out of phase: sub corr %.2f (mono %.1f dB), mid corr %.2f\n", cs, HealthAnalyser::monoLossDb(p.bar(1), 0), cm);
        CHECK(cs < -0.9f && HealthAnalyser::monoLossDb(p.bar(1), 0) < -15.0f && cm > 0.9f, "an out-of-phase sub is caught per band");
        // True peak: a sine at fs/4 sampled at 45 degrees has sample peaks at -3 dB, true peak 0 dB.
        HealthAnalyser t; t.prepare(FS);
        std::vector<float> s((size_t) FS);
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float) std::sin(PI / 2 * i + PI / 4);
        for (size_t i = 0; i + 512 <= s.size(); i += 512) t.process(&s[i], &s[i], 512, 1);
        std::printf("  [health] fs/4 sine: sample peak %.2f dB, true peak %.2f dBTP\n", t.samplePeakDb(), t.truePeakDb());
        CHECK(std::fabs(t.samplePeakDb() + 3.01f) < 0.1f && std::fabs(t.truePeakDb()) < 0.6f, "true peak sees the inter-sample peak");
    }
    // 11. Harmonics stage: adds harmonics of the band only, at about the set level.
    {
        const double PI = 3.14159265358979;
        auto goertzel = [](const std::vector<float>& x, double f, size_t from) {
            const double w = 2 * 3.14159265358979 * f / FS, cw = 2 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (size_t i = from; i < x.size(); ++i) { const double s0 = x[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            const double p = s1 * s1 + s2 * s2 - cw * s1 * s2;
            return 10.0 * std::log10(p / (double) (x.size() - from) + 1e-30);
        };
        auto run = [&](const HarmPlan& hp, double f0, double amp) {
            HarmonicStage st; st.prepare(FS); st.set(hp);
            std::vector<float> L((size_t) (2 * FS)), R;
            for (size_t i = 0; i < L.size(); ++i) L[i] = (float) (amp * std::sin(2 * PI * f0 * i / FS));
            R = L;
            for (size_t i = 0; i + 512 <= L.size(); i += 512) { float* d[2] = { &L[i], &R[i] }; st.process(d, 2, 512, 1.0f); }
            return L;
        };
        HarmPlan hb; hb.bassOn = 1; hb.bassLo = 32; hb.bassHi = 80; hb.bassHp = 85; hb.bassLp = 400; hb.bassMixDb = -12;
        auto y = run(hb, 50.0, 0.5);
        const double f = goertzel(y, 50, 24000), h2 = goertzel(y, 100, 24000), h3 = goertzel(y, 150, 24000), h8 = goertzel(y, 1000, 24000);
        std::printf("  [harmonics] 50 Hz bass: fundamental %.1f, 2nd %.1f, 3rd %.1f, 1 kHz %.1f dB\n", f, h2, h3, h8);
        CHECK(h2 - f > -30 && h3 - f > -35 && std::max(h2, h3) - f < -6 && h8 - f < -50, "harmonic bass adds 2nd/3rd harmonics under the fundamental, nothing high up");
        auto quiet = run(hb, 50.0, 0.05);
        const double fq = goertzel(quiet, 50, 24000), h2q = goertzel(quiet, 100, 24000);
        CHECK(std::fabs((h2q - fq) - (h2 - f)) < 3.0, "harmonic level follows the fundamental (same ratio 20 dB quieter)");
        HarmPlan off; auto dry = run(off, 50.0, 0.5);
        CHECK(goertzel(dry, 100, 24000) - goertzel(dry, 50, 24000) < -80, "switched off: adds nothing");
        HarmPlan pf; pf.presOn = 1; pf.presFreq = 2500; pf.presMixDb = -15;
        auto z = run(pf, 2500.0, 0.2);
        const double pf0 = goertzel(z, 2500, 24000), pf2 = goertzel(z, 5000, 24000), pf3 = goertzel(z, 7500, 24000), plow = goertzel(z, 1250, 24000);
        std::printf("  [harmonics] 2.5 kHz presence: fundamental %.1f, 5 kHz %.1f, 7.5 kHz %.1f, 1.25 kHz %.1f dB\n", pf0, pf2, pf3, plow);
        CHECK(std::max(pf2, pf3) - pf0 > -40 && std::max(pf2, pf3) - pf0 < -8 && plow - pf0 < -60, "presence fill adds harmonics above the band only");
    }
    // 12. Harmonics planner.
    {
        auto bass = measure(0, "Sub Bass", RoleBass, 0.65f, harmonic(12, 50.0, 3.0, 0.4, -20));   // weight at 50 Hz, weak harmonics
        auto rich = measure(1, "Rich Bass", RoleBass, 0.65f, harmonic(12, 55.0, 0.7, 0.3, 0));   // plenty of harmonics
        auto lead = measure(2, "Upright", RoleSolo, 0.95f, harmonic(12, 330.0, 1.6, 0.02, -20)); // quiet presence
        auto pad = measure(3, "Pad", RoleSynth, 0.4f, harmonic(12, 262.0, 0.6, 0.06, 0));          // owns the presence
        auto hs = planHarmonics({ bass, rich, lead, pad }, { 2 }, StyleBand);
        bool subB = false, richB = false, fill = false;
        for (auto& s : hs) {
            std::printf("  [harmonics plan] track %d %s conf %.2f: %s\n", s.track, s.bass ? "bass" : "presence", s.confidence, s.reason.c_str());
            if (s.track == 0 && s.bass) subB = true;
            if (s.track == 1 && s.bass) richB = true;
            if (s.track == 2 && !s.bass) fill = true;
        }
        CHECK(subB, "harmonic bass proposed for a bass with its weight at 50 Hz and weak harmonics");
        CHECK(!richB, "a bass that already has its harmonics is left alone");
        CHECK(fill, "presence fill proposed for a lead that holds too little of its presence band");
        auto lk = lead; lk.locked = true;
        bool none = true; for (auto& s : planHarmonics({ bass, rich, lk, pad }, { 2 }, StyleBand)) if (s.track == 2) none = false;
        CHECK(none, "a locked lead gets no presence fill");
    }
    // 13. What the stereo out shows that the tracks don't.
    {
        auto mk = [](int id, const char* name) { TrackTimeline t; t.id = id; t.name = name; for (auto& r : t.db) r.fill(kFloorDb); return t; };
        auto kick = mk(0, "Kick"), bass = mk(1, "Bass"), keys = mk(2, "Keys");
        for (int b = 0; b < 24; ++b) {
            kick.db[(size_t) b][0] = -20; kick.db[(size_t) b][1] = -22; kick.db[(size_t) b][5] = -40;
            if (b >= 8) { bass.db[(size_t) b][1] = -21; bass.db[(size_t) b][2] = -30; }
            keys.db[(size_t) b][3] = -26; keys.db[(size_t) b][4] = -30;
        }
        Timeline m; for (auto& r : m) r.fill(kFloorDb);
        auto lin = [](float d) { return d <= kFloorDb + 1 ? 0.0 : std::pow(10.0, d / 10.0); };
        for (int b = 0; b < 24; ++b) for (int k = 0; k < kCoarse; ++k) {
            double p = lin(kick.db[(size_t) b][(size_t) k]) + lin(bass.db[(size_t) b][(size_t) k]) + lin(keys.db[(size_t) b][(size_t) k]);
            if (k == 1 && b >= 8) p *= 0.4;              // kick + bass partly cancel where they overlap
            if (k == 3 && b >= 12 && b < 18) p += lin(-18); // a part nobody measures
            m[(size_t) b][(size_t) k] = p > 0 ? (float) (10 * std::log10(p)) : kFloorDb;
        }
        std::vector<TrackTimeline> tt { kick, bass, keys };
        auto fit = estimateLevels(tt, m);
        std::printf("  [residuals] fit Kick %+.2f Bass %+.2f Keys %+.2f dB, r2 %.2f, %d cells left out\n", fit.offsetDb[0], fit.offsetDb[1], fit.offsetDb[2], fit.r2, fit.excluded);
        CHECK(std::fabs(fit.offsetDb[2]) < 0.5f, "the fit isn't dragged by a part nobody measures (Keys stays at 0 dB)");
        for (size_t i = 0; i < tt.size(); ++i) tt[i].offsetDb = fit.offsetDb[i];
        auto fs = findResiduals(tt, m);
        bool unexplained = false, cancel = false;
        for (auto& f : fs) {
            std::printf("  [residuals] %s: %s\n", f.kind.c_str(), f.text.c_str());
            if (f.kind == "unexplained" && f.band == 3 && f.start >= 12 && f.start <= 14 && f.end >= 17 && f.end <= 19) unexplained = true;
            if (f.kind == "low-end cancellation" && f.band == 1 && f.start == 9) cancel = true;
        }
        CHECK(unexplained, "energy no track accounts for is found, with its band and bars");
        CHECK(cancel, "low end lost where kick and bass overlap is found");
        // An unmeasured part playing only alongside a measured one with a similar spectrum.
        {
            auto gtr = mk(0, "Gtr"), bs = mk(1, "Bass");
            for (int b = 0; b < 24; ++b) {
                bs.db[(size_t) b][1] = -20; bs.db[(size_t) b][2] = -28;
                if (b < 8 || b >= 16) { gtr.db[(size_t) b][3] = -24; gtr.db[(size_t) b][4] = -27; gtr.db[(size_t) b][2] = -32; }
            }
            Timeline m2; for (auto& r : m2) r.fill(kFloorDb);
            for (int b = 0; b < 24; ++b) for (int k = 0; k < kCoarse; ++k) {
                double p = 0.5 * lin(gtr.db[(size_t) b][(size_t) k]) + lin(bs.db[(size_t) b][(size_t) k]);
                if (b >= 16 && (k == 3 || k == 4)) p += 2.0 * lin(gtr.db[(size_t) b][(size_t) k]); // a pad, a little louder than the guitar
                m2[(size_t) b][(size_t) k] = p > 0 ? (float) (10 * std::log10(p)) : kFloorDb;
            }
            std::vector<TrackTimeline> t2 { gtr, bs };
            auto f2 = estimateLevels(t2, m2);
            for (size_t i = 0; i < t2.size(); ++i) t2[i].offsetDb = f2.offsetDb[i];
            bool padFound = false;
            for (auto& f : findResiduals(t2, m2)) if (f.kind == "unexplained" && f.start == 17) padFound = true;
            std::printf("  [residuals] hidden pad with the guitar: Gtr %+.2f dB (true -3.01), pad found %d\n", f2.offsetDb[0], padFound ? 1 : 0);
            CHECK(std::fabs(f2.offsetDb[0] + 3.01f) < 0.5f && padFound, "a hidden part playing alongside a measured one doesn't inflate it, and is found");
        }
    }

    // 14. Mix balance: fader moves towards the style's balance.
    {
        auto mk = [](int id, const char* name, int role, float prio, float a, float b, bool locked = false) {
            TrackTimeline t; t.id = id; t.name = name; t.role = role; t.priority = prio; t.locked = locked;
            for (auto& r : t.db) r.fill(kFloorDb);
            t.lufs.assign((size_t) kBars, kFloorDb);
            for (int x = 0; x < 16; ++x) { t.lufs[(size_t) x] = x < 8 ? a : b; t.db[(size_t) x][3] = -30; }
            return t;
        };
        std::vector<TrackTimeline> tt {
            mk(0, "Vox", RoleLeadVocal, 0.95f, -20, -20), mk(1, "Drums", RoleKick, 0.85f, -30, -30),
            mk(2, "Sub", RoleBass, 0.65f, -23, -23), mk(3, "Gtr A", RoleElectricGtr, 0.45f, -22.5f, -16.5f),
            mk(4, "Gtr B", RoleElectricGtr, 0.45f, -22.5f, -22.5f), mk(5, "Synth", RoleSynth, 0.45f, -27, -27),
            mk(6, "Pad", RoleSynth, 0.45f, -27, -27, true) };
        SectionSpan s1; s1.name = "Verse"; s1.start = 1; s1.end = 8; s1.leadId = 0;
        SectionSpan s2; s2.name = "Chorus"; s2.start = 9; s2.end = 16; s2.leadId = 0;
        auto mb = planMixBalance(tt, { s1, s2 }, "industrial", LeadPlace::Blended);
        float drums = 0, gtrRide = 0, pad = 0, voxB = 0;
        for (auto& m : mb.moves) {
            std::printf("  [balance] %s %s %+.1f dB: %s\n", m.name.c_str(), m.section < 0 ? "song" : ("s" + std::to_string(m.section)).c_str(), m.gainDb, m.reason.c_str());
            if (m.name == "Drums" && m.section < 0) drums = m.gainDb;
            if (m.name == "Gtr A" && m.section == 1) gtrRide = m.gainDb;
            if (m.name == "Pad") pad = 1;
            if (m.name == "Vox" && m.section < 0) voxB = m.gainDb;
        }
        std::printf("  [balance] spread %.1f -> %.1f dB\n", mb.spreadBefore, mb.spreadAfter);
        CHECK(drums >= 4.0f, "drums far under the vocal for industrial get a big whole-song lift");
        CHECK(gtrRide <= -3.0f, "a guitar that's too loud only in the chorus gets a ride down there");
        CHECK(pad == 0, "a locked part is never moved");
        CHECK(mb.spreadAfter < mb.spreadBefore * 0.6f, "the moves bring the mix much closer to the target balance");
        auto top = planMixBalance(tt, { s1, s2 }, "industrial", LeadPlace::OnTop);
        float voxT = 0; for (auto& m : top.moves) if (m.name == "Vox" && m.section < 0) voxT = m.gainDb;
        CHECK(voxT > voxB + 1.0f, "lead 'on top' puts the vocal higher than 'blended'");
        auto pop = planMixBalance(tt, { s1, s2 }, "pop", LeadPlace::OnTop);
        float gp = 0; for (auto& m : pop.moves) if (m.name == "Gtr B" && m.section < 0) gp = m.gainDb;
        float gi = 0; for (auto& m : mb.moves) if (m.name == "Gtr B" && m.section < 0) gi = m.gainDb;
        CHECK(gp < gi, "pop sits the guitars lower than industrial");
    }
    // 5. Equal priorities: no one is favoured.
    {
        auto a = measure(0, "A", RoleOther, 0.5f, harmonic(20, 100.0, 1.0, 0.1, 0));
        auto b = measure(1, "B", RoleOther, 0.5f, harmonic(20, 100.0, 1.0, 0.1, 0));
        CHECK(plan({ a, b }, PlannerSettings()).empty(), "equal priority makes no changes");
    }
    std::printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
