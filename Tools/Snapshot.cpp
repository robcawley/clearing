// Headless end-to-end run: several real CLEARING instances, a shared hub, the real editor.
// Feeds synthetic parts through the instances, lets the planner propose changes, applies them,
// measures the processed audio, and writes PNGs of the window.
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/mman.h>

using namespace clr;
static const double FS = 48000.0;
static const double PI = 3.14159265358979323846;

static std::vector<float> harmonic(double sec, double f0, double tilt, double gain, double airDb, int seed)
{
    std::vector<float> x((size_t) (sec * FS), 0.0f);
    for (int h = 1; h * f0 < 18000; ++h) {
        double amp = gain / std::pow((double) h, tilt);
        if (h * f0 > 10000) amp *= std::pow(10.0, airDb / 20.0);
        for (size_t i = 0; i < x.size(); ++i) x[i] += (float) (amp * std::sin(2 * PI * h * f0 * (double) i / FS + h + seed));
    }
    return x;
}
static std::vector<float> kick(double sec)
{
    std::vector<float> x((size_t) (sec * FS), 0.0f);
    double ph = 0;
    for (size_t s = 0; s < x.size(); s += (size_t) (FS * 0.5))
        for (size_t i = 0; i < (size_t) (0.25 * FS) && s + i < x.size(); ++i) {
            const double t = (double) i / FS;
            ph += 2 * PI * (50.0 + 40.0 * std::exp(-t * 40.0)) / FS;
            x[s + i] += (float) (0.8 * std::sin(ph) * std::exp(-t * 12.0));
        }
    return x;
}
static std::vector<float> snare(double sec)
{
    std::vector<float> x((size_t) (sec * FS), 0.0f);
    unsigned r = 12345;
    for (size_t s = (size_t) (FS * 0.5); s < x.size(); s += (size_t) FS)
        for (size_t i = 0; i < (size_t) (0.18 * FS) && s + i < x.size(); ++i) {
            r = r * 1664525u + 1013904223u;
            const double n = ((r >> 8) & 0xffff) / 32768.0 - 1.0, t = (double) i / FS;
            x[s + i] += (float) (0.25 * n * std::exp(-t * 22.0) + 0.3 * std::sin(2 * PI * 190 * t) * std::exp(-t * 30.0));
        }
    return x;
}

struct Part { const char* name; int role; std::vector<float> audio; };

struct FakePlayHead : juce::AudioPlayHead {
    double ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setPpqPosition(ppq); p.setTimeSignature(TimeSignature { 4, 4 }); p.setIsPlaying(true); p.setBpm(120.0);
        return p;
    }
};

// Gain (dB) of a sine at freq through proc over `blocks` blocks after `settle` blocks.
// Optional per-block hook (block index) for automation and the leader feed.
template <class Hook>
static double sineGainDb(ClearingProcessor& p, float freq, int settle, int blocks, Hook&& hook, double* maxStepRatio = nullptr)
{
    juce::AudioBuffer<float> b(2, 512); juce::MidiBuffer m;
    double ein = 0, eout = 0, maxIn = 0, maxOut = 0; float ph = 0; float lastIn = 0, lastOut = 0;
    for (int blk = 0; blk < settle + blocks; ++blk) {
        hook(blk);
        for (int i = 0; i < 512; ++i) { const float v = 0.3f * std::sin(ph); ph += (float) (2 * PI * freq / FS); b.setSample(0, i, v); b.setSample(1, i, v); }
        juce::AudioBuffer<float> ref(b);
        p.processBlock(b, m);
        if (blk >= settle)
            for (int i = 0; i < 512; ++i) {
                const float x = ref.getSample(0, i), y = b.getSample(0, i);
                ein += x * x; eout += y * y;
                maxIn = std::max(maxIn, (double) std::fabs(x - lastIn)); maxOut = std::max(maxOut, (double) std::fabs(y - lastOut));
                lastIn = x; lastOut = y;
            }
        else { lastIn = ref.getSample(0, 511); lastOut = b.getSample(0, 511); }
    }
    if (maxStepRatio) *maxStepRatio = maxOut / std::max(1e-9, maxIn);
    return 10 * std::log10(eout / ein);
}

int main(int argc, char** argv)
{
    const juce::String outDir = argc > 1 ? argv[1] : ".";
    const bool orch = argc > 2 && juce::String(argv[2]) == "orch";
    setenv("CLEARING_HUB", "/clearing_snapshot", 1);
    shm_unlink("/clearing_snapshot");
    juce::ScopedJuceInitialiser_GUI init;
    const double secs = 34;

    std::vector<Part> parts;
    if (!orch) {
        parts = { { "Kick", RoleKick, kick(secs) }, { "Bass", RoleBass, harmonic(secs, 65, 1.2, 0.35, 0, 1) },
                  { "Snare", RoleSnare, snare(secs) }, { "Lead Vox", RoleLeadVocal, harmonic(secs, 220, 1.0, 0.04, -12, 2) },
                  { "Rhythm Gtr", RoleElectricGtr, harmonic(secs, 196, 0.9, 0.04, 0, 3) },
                  { "Keys", RolePiano, harmonic(secs, 262, 1.1, 0.05, -6, 4) } };
    } else {
        parts = { { "Solo Oboe", RoleSolo, harmonic(secs, 440, 1.0, 0.05, -10, 1) }, { "Violins", RoleViolins, harmonic(secs, 330, 0.8, 0.05, -6, 2) },
                  { "Violas", RoleViolas, harmonic(secs, 250, 0.8, 0.05, -6, 3) }, { "Horns", RoleHorns, harmonic(secs, 262, 0.8, 0.05, -10, 4) },
                  { "Cellos", RoleCellos, harmonic(secs, 196, 0.8, 0.05, -6, 5) }, { "Basses", RoleDoubleBasses, harmonic(secs, 98, 0.9, 0.06, -12, 6) } };
    }

    std::vector<std::unique_ptr<ClearingProcessor>> procs;
    for (auto& p : parts) {
        auto pr = std::make_unique<ClearingProcessor>();
        pr->prepareToPlay(FS, 512);
        pr->setTrackName(p.name);
        auto set = [&](const char* id, float v) { auto* q = pr->apvts.getParameter(id); q->setValueNotifyingHost(q->convertTo0to1(v)); };
        set("role", (float) p.role); set("priority", roleDefaultPriority(p.role)); set("style", orch ? 1.f : 0.f);
        procs.push_back(std::move(pr));
    }
    int fails = 0;
    auto check = [&](bool ok, const char* msg) { if (!ok) { ++fails; std::printf("FAIL: %s\n", msg); } else std::printf("ok:   %s\n", msg); };
    for (auto& p : procs) p->publish();

    std::unique_ptr<ClearingEditor> ed(dynamic_cast<ClearingEditor*>(procs[0]->createEditor()));
    ed->setBounds(0, 0, ed->getWidth(), ed->getHeight());
    ed->refreshNow();
    check(ed->tracks.size() == parts.size(), "editor sees every instance through the hub");
    ed->startListening();

    auto feed = [&](size_t from, size_t to) {
        juce::AudioBuffer<float> b(2, 512); juce::MidiBuffer m;
        for (size_t s = from; s + 512 <= to; s += 512) {
            for (size_t k = 0; k < procs.size(); ++k) {
                for (int i = 0; i < 512; ++i) { b.setSample(0, i, parts[k].audio[s + (size_t) i]); b.setSample(1, i, parts[k].audio[s + (size_t) i]); }
                procs[k]->processBlock(b, m);
                procs[k]->pump();
            }
        }
        for (auto& p : procs) p->publish();
    };
    auto save = [&](const char* nm) {
        ed->refreshNow();
        auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
        juce::File f = juce::File(outDir).getChildFile(nm);
        f.deleteFile();
        juce::FileOutputStream os(f);
        juce::PNGImageFormat().writeImageToStream(img, os);
        std::printf("wrote %s\n", f.getFullPathName().toRawUTF8());
    };

    feed(0, (size_t) (14 * FS));
    save(orch ? "orch_1_listening.png" : "band_1_listening.png");
    feed((size_t) (14 * FS), (size_t) (secs * FS) - 512);
    ed->stopListening();
    check(!ed->suggestions.empty(), "planner proposes changes after listening");
    for (auto& s : ed->suggestions) std::printf("  %-10s %+.1f dB @ %.0f Hz  conf %.2f%s\n", ed->find(s.slot)->name.toRawUTF8(), s.change.gainDb, s.change.freq, s.change.confidence, s.change.dynamic ? "  (dip)" : "");
    for (auto& n : ed->notes) std::printf("  note: %s\n", n.toRawUTF8());
    ed->select(1);
    save(orch ? "orch_2_suggestions.png" : "band_2_suggestions.png");

    // Apply, then measure real audio through the instance that was cut.
    int target = -1; float gain = 0, freq = 0;
    for (auto& s : ed->suggestions) if (s.change.gainDb < 0 && !s.change.dynamic) { target = s.slot; gain = s.change.gainDb; freq = s.change.freq; break; }
    ed->applySuggestions();
    for (auto& p : procs) p->pollHub(false);
    if (target >= 0) {
        ClearingProcessor* tp = nullptr; for (auto& p : procs) if (p->getSlot() == target) tp = p.get();
        juce::AudioBuffer<float> b(2, 512); juce::MidiBuffer m;
        double ein = 0, eout = 0; float ph = 0;
        for (int blk = 0; blk < 200; ++blk) {
            for (int i = 0; i < 512; ++i) { const float v = 0.3f * std::sin(ph); ph += (float) (2 * PI * freq / FS); b.setSample(0, i, v); b.setSample(1, i, v); }
            juce::AudioBuffer<float> ref(b);
            tp->processBlock(b, m);
            if (blk > 100) for (int i = 0; i < 512; ++i) { ein += ref.getSample(0, i) * ref.getSample(0, i); eout += b.getSample(0, i) * b.getSample(0, i); }
        }
        const double got = 10 * std::log10(eout / ein);
        std::printf("applied cut %.1f dB at %.0f Hz; measured through the plug-in: %.2f dB\n", gain, freq, got);
        check(std::fabs(got - gain) < 0.4, "processed audio carries the planned cut");
        ed->undoApply();
        for (auto& p : procs) p->pollHub(false);
        const double afterUndo = sineGainDb(*tp, freq, 60, 100, [](int) {});
        std::printf("after undo: %.3f dB\n", afterUndo);
        check(std::fabs(afterUndo) < 0.1, "undo restores flat response");
        ed->applySuggestions();
        for (auto& p : procs) p->pollHub(false);
    }
    // Save/restore keeps the accepted plan.
    if (target >= 0) {
        juce::MemoryBlock mb; ClearingProcessor* tp = nullptr; for (auto& p : procs) if (p->getSlot() == target) tp = p.get();
        tp->getStateInformation(mb);
        ClearingProcessor fresh; fresh.prepareToPlay(FS, 512); fresh.setStateInformation(mb.getData(), (int) mb.getSize());
        const auto a = tp->currentPlan(), b = fresh.currentPlan();
        bool same = true;
        for (int i = 0; i < kEqBands; ++i)
            same = same && a[i].enabled == b[i].enabled && a[i].gainDb == b[i].gainDb && a[i].freq == b[i].freq && a[i].q == b[i].q
                   && a[i].dynamic == b[i].dynamic && a[i].leaderUid == b[i].leaderUid && a[i].threshDb == b[i].threshDb;
        check(same, "project state round-trips (every band, including dips and their leader)");
    }
    // ---- Movement: a dip on a supporting track follows the focus --------------------------
    {
        const Suggestion* dip = nullptr;
        for (auto& sg : ed->suggestions) if (sg.change.dynamic) { dip = &sg; break; }
        check(dip != nullptr, "planner proposes a focus-following dip");
        if (dip) {
            ClearingProcessor *fp = nullptr, *lp = nullptr; size_t li = 0;
            for (size_t k = 0; k < procs.size(); ++k) {
                if (procs[k]->getSlot() == dip->slot) fp = procs[k].get();
                if (procs[k]->getSlot() == dip->change.leader) { lp = procs[k].get(); li = k; }
            }
            for (auto& p : procs) p->syncShared();
            juce::AudioBuffer<float> lb(2, 512); juce::MidiBuffer m; size_t lpos = 0; bool leaderPlays = true;
            auto feedLeader = [&](int) {
                for (int i = 0; i < 512; ++i) { const float v = leaderPlays ? parts[li].audio[(lpos + (size_t) i) % parts[li].audio.size()] : 0.0f; lb.setSample(0, i, v); lb.setSample(1, i, v); }
                lpos += 512; lp->processBlock(lb, m);
            };
            // static moves on the follower would mix into the measurement: measure the dip alone
            auto plan = fp->currentPlan(); float staticAt = 0;
            for (auto& e : plan) if (e.enabled && !e.dynamic) staticAt += (float) magnitudeDb(makeCoeffs((BandType) e.type, FS, e.freq, e.gainDb, e.q), FS, dip->change.freq);
            const double on = sineGainDb(*fp, dip->change.freq, 120, 60, feedLeader) - staticAt;
            leaderPlays = false;
            const double off = sineGainDb(*fp, dip->change.freq, 120, 60, feedLeader) - staticAt;
            std::printf("dip %.1f dB at %.0f Hz on %s: leader playing %.2f dB, leader silent %.2f dB\n", dip->change.gainDb, dip->change.freq,
                        ed->find(dip->slot)->name.toRawUTF8(), on, off);
            check(std::fabs(on - dip->change.gainDb) < 0.5, "dip reaches its depth while the focus plays");
            check(std::fabs(off) < 0.2, "dip returns to flat when the focus stops");
            // Movement 0: no dip even while the focus plays.
            auto* mv = fp->apvts.getParameter("movement"); mv->setValueNotifyingHost(0.0f); leaderPlays = true;
            const double still = sineGainDb(*fp, dip->change.freq, 120, 60, feedLeader) - staticAt;
            check(std::fabs(still) < 0.2, "Movement 0% removes the dip");
            mv->setValueNotifyingHost(1.0f);
        }
    }
    // ---- Automation: Amount scales the static EQ, glides without clicks -----------------------
    if (target >= 0) {
        ClearingProcessor* tp = nullptr; for (auto& p : procs) if (p->getSlot() == target) tp = p.get();
        // disable dips on this track for a clean static measurement
        auto* mv = tp->apvts.getParameter("movement"); mv->setValueNotifyingHost(0.0f);
        auto* am = tp->apvts.getParameter("amount");
        am->setValueNotifyingHost(0.5f);
        const double half = sineGainDb(*tp, freq, 60, 100, [](int) {});
        am->setValueNotifyingHost(1.0f);
        const double full = sineGainDb(*tp, freq, 60, 100, [](int) {});
        std::printf("Amount 50%%: %.2f dB, 100%%: %.2f dB (planned %.1f)\n", half, full, gain);
        check(std::fabs(full - gain) < 0.4 && std::fabs(half - full / 2) < 0.3, "Amount automation scales the EQ");
        double ratio = 0;
        sineGainDb(*tp, freq, 4, 400, [&](int blk) { am->setValueNotifyingHost((blk / 4) % 2 ? 1.0f : 0.0f); }, &ratio);
        std::printf("Amount toggled 0/100%% every 4 blocks: largest output step %.3fx the input's\n", ratio);
        check(ratio < 1.15, "Amount automation is click-free");
        am->setValueNotifyingHost(1.0f);
        mv->setValueNotifyingHost(1.0f);

        // ---- Sections by bar ------------------------------------------------------------------
        auto* h = procs[0]->getHub().get();
        seqWrite(h->briefSeq, [&] {
            std::strncpy(h->brief, "Test brief: intimate verse, huge chorus.", kBriefLen - 1);
            h->numSections = 2;
            SectionData a; std::strncpy(a.name, "Verse", 23); a.startBar = 1; a.endBar = 4; a.amount = 0.0f; a.movement = 1; std::strncpy(a.feel, "intimate", 95);
            SectionData c; std::strncpy(c.name, "Chorus", 23); c.startBar = 5; c.endBar = 8; c.amount = 1.0f; c.movement = 0.4f; std::strncpy(c.feel, "huge", 95);
            h->sections[0] = a; h->sections[1] = c;
        });
        for (auto& p : procs) p->syncShared();
        FakePlayHead ph; tp->setPlayHead(&ph);
        mv->setValueNotifyingHost(0.0f);
        ph.ppq = 4.0;  const double verse = sineGainDb(*tp, freq, 120, 60, [](int) {});
        const int secV = tp->currentSection();
        ph.ppq = 20.0; const double chorus = sineGainDb(*tp, freq, 120, 60, [](int) {});
        const int secC = tp->currentSection();
        std::printf("  [dbg] sections seen: verse %d chorus %d\n", secV, secC);
        std::printf("sections: bar 2 (Verse, amount 0) %.2f dB, bar 6 (Chorus, amount 1) %.2f dB\n", verse, chorus);
        check(secV == 0 && secC == 1, "playhead finds the right section");
        check(std::fabs(verse) < 0.2 && std::fabs(chorus - gain) < 0.4, "sections scale Amount by bar");
        tp->setPlayHead(nullptr);
        mv->setValueNotifyingHost(1.0f);
        ed->refreshNow();

        // ---- Identity and arrangement survive a save and reopen ------------------------------
        juce::MemoryBlock st;
        uint64_t oldUid = 0;
        {
            ClearingProcessor x; x.prepareToPlay(FS, 512); x.syncShared();
            oldUid = x.getUid(); x.getStateInformation(st);
        }
        ClearingProcessor y; y.prepareToPlay(FS, 512);
        y.setStateInformation(st.getData(), (int) st.getSize());
        check(y.getUid() == oldUid, "a reopened instance keeps its identity (followers still find it)");
        ClearingProcessor dup; dup.prepareToPlay(FS, 512);
        juce::MemoryBlock tst; tp->getStateInformation(tst);
        dup.setStateInformation(tst.getData(), (int) tst.getSize());
        check(dup.getUid() != tp->getUid(), "a duplicated track gets its own identity");
        const std::string raw((const char*) tst.getData(), tst.getSize());
        check(raw.find("Test brief") != std::string::npos && raw.find("Chorus") != std::string::npos, "brief and sections are saved with the project");
    }

    // ---- 32 tracks: more than the old 16 all join --------------------------------------
    {
        std::vector<std::unique_ptr<ClearingProcessor>> extra;
        for (int i = 0; i < 26 - (int) procs.size(); ++i) extra.push_back(std::make_unique<ClearingProcessor>());
        bool allIn = true; for (auto& e : extra) allIn = allIn && e->getSlot() >= 0;
        check(allIn, "26 instances in one session all join the hub (limit is now 32)");
    }
    // ---- A dip limited to one section ------------------------------------------------------
    {
        const Suggestion* dip = nullptr;
        for (auto& sg : ed->suggestions) if (sg.change.dynamic && !sg.change.self) { dip = &sg; break; }
        if (dip) {
            ClearingProcessor *fp = nullptr, *lp = nullptr; size_t li = 0;
            for (size_t k = 0; k < procs.size(); ++k) {
                if (procs[k]->getSlot() == dip->slot) fp = procs[k].get();
                if (procs[k]->getSlot() == dip->change.leader) { lp = procs[k].get(); li = k; }
            }
            auto* h = fp->getHub().get();
            auto& sl = h->slots[fp->getSlot()];
            std::array<EqBandData, kEqBands> only {};
            only[0].freq = dip->change.freq; only[0].gainDb = dip->change.gainDb; only[0].q = dip->change.q; only[0].type = (int) dip->change.type;
            only[0].enabled = 1; only[0].dynamic = 1; only[0].threshDb = dip->change.threshDb; only[0].leaderUid = lp->getUid();
            only[0].sectionMask = 2u; // Chorus only (section 2)
            seqWrite(sl.planSeq, [&] { std::copy(only.begin(), only.end(), sl.plan); });
            fp->pollHub(false); fp->syncShared();
            FakePlayHead ph; fp->setPlayHead(&ph);
            juce::AudioBuffer<float> lb(2, 512); juce::MidiBuffer m; size_t lpos = 0;
            auto feedLeader = [&](int) {
                for (int i = 0; i < 512; ++i) { const float v = parts[li].audio[(lpos + (size_t) i) % parts[li].audio.size()]; lb.setSample(0, i, v); lb.setSample(1, i, v); }
                lpos += 512; lp->processBlock(lb, m);
            };
            ph.ppq = 4.0;  const double inVerse = sineGainDb(*fp, dip->change.freq, 120, 60, feedLeader);
            ph.ppq = 20.0; const double inChorus = sineGainDb(*fp, dip->change.freq, 120, 60, feedLeader);
            std::printf("section-limited dip %.1f dB: Verse %.2f dB, Chorus %.2f dB (leader playing in both)\n", dip->change.gainDb, inVerse, inChorus);
            // The Chorus section above sets Movement to 40%, which scales dips.
            check(std::fabs(inVerse) < 0.2 && std::fabs(inChorus - 0.4 * dip->change.gainDb) < 0.15, "a section lead's dip acts only in its section (at the section's Movement)");
            fp->setPlayHead(nullptr);
        }
    }
    // ---- Self-triggered band: cuts only while the track builds up there ----------------
    if (target >= 0) {
        ClearingProcessor* tp = nullptr; for (auto& p : procs) if (p->getSlot() == target) tp = p.get();
        auto& sl = tp->getHub().get()->slots[tp->getSlot()];
        std::array<EqBandData, kEqBands> keep; seqRead(sl.planSeq, [&] { std::copy(sl.plan, sl.plan + kEqBands, keep.begin()); });
        std::array<EqBandData, kEqBands> self {};
        self[0].freq = 3100; self[0].gainDb = -2.0f; self[0].q = 1.4f; self[0].type = (int) BandType::Bell;
        self[0].enabled = 1; self[0].dynamic = 1; self[0].self = 1; self[0].leaderUid = tp->getUid();
        seqWrite(sl.planSeq, [&] { std::copy(self.begin(), self.end(), sl.plan); });
        tp->pollHub(false);
        juce::AudioBuffer<float> b(2, 512); juce::MidiBuffer m; float ph = 0;
        // A steady 220 Hz part with a 3.1 kHz component; only the 3.1 kHz component bursts.
        float ph2 = 0;
        auto run = [&](float amp, int blocks, int measureFrom) {
            double ein = 0, eout = 0;
            for (int blk = 0; blk < blocks; ++blk) {
                for (int i = 0; i < 512; ++i) {
                    const float v = amp * std::sin(ph) + 0.3f * std::sin(ph2);
                    ph += (float) (2 * PI * 3100.0 / FS); ph2 += (float) (2 * PI * 220.0 / FS);
                    b.setSample(0, i, v); b.setSample(1, i, v);
                }
                juce::AudioBuffer<float> ref(b); tp->processBlock(b, m);
                // measure the 3.1 kHz part only: subtract the untouched 220 Hz (the band barely touches it)
                if (blk >= measureFrom) for (int i = 0; i < 512; ++i) { ein += ref.getSample(0, i) * ref.getSample(0, i); eout += b.getSample(0, i) * b.getSample(0, i); }
            }
            return 10 * std::log10(eout / ein);
        };
        const auto gain3k = [&](double totalDb, float amp) { // convert a whole-signal change into the 3.1 kHz change
            const double pIn = 0.5 * (amp * amp + 0.09), p220 = 0.5 * 0.09;
            const double pOut = pIn * std::pow(10.0, totalDb / 10.0) - p220;
            return 10 * std::log10(std::max(1e-12, pOut) / (0.5 * amp * amp));
        };
        const double usualTotal = run(0.05f, 600, 450);
        { auto d = tp->selfDebug(0); std::printf("  [dbg] det fast %.1f usual %.1f | part %.1f usual %.1f | total %.3f dB\n", d[0], d[1], d[2], d[3], usualTotal); }
        const double usual = gain3k(usualTotal, 0.05f);   // ~6 s at its usual level: no cut
        const double burst = gain3k(run(0.4f, 40, 20), 0.4f);       // the 3.1 kHz part jumps 18 dB: cut
        const double after = gain3k(run(0.05f, 150, 110), 0.05f);   // back to normal: releases
        std::printf("self band -2.0 dB at 3.1 kHz: usual %.2f dB, burst %.2f dB, after %.2f dB\n", usual, burst, after);
        check(std::fabs(usual) < 0.3 && burst < -1.5 && std::fabs(after) < 0.4, "a self-triggered band cuts only while the part builds up there");
        // A crescendo of the whole part (both components up 12 dB) is not a build-up.
        {
            double ein = 0, eout = 0;
            for (int blk = 0; blk < 300; ++blk) {
                const float g = blk < 150 ? 1.0f : 4.0f;
                for (int i = 0; i < 512; ++i) {
                    const float v = g * (0.05f * std::sin(ph) + 0.3f * std::sin(ph2));
                    ph += (float) (2 * PI * 3100.0 / FS); ph2 += (float) (2 * PI * 220.0 / FS);
                    b.setSample(0, i, v); b.setSample(1, i, v);
                }
                juce::AudioBuffer<float> ref(b); tp->processBlock(b, m);
                if (blk >= 170 && blk < 200) for (int i = 0; i < 512; ++i) { ein += ref.getSample(0, i) * ref.getSample(0, i); eout += b.getSample(0, i) * b.getSample(0, i); }
            }
            const double cres = 10 * std::log10(eout / ein);
            std::printf("whole-part crescendo +12 dB: overall change %.2f dB\n", cres);
            check(std::fabs(cres) < 0.1, "a crescendo of the whole part doesn't trigger it");
        }
        seqWrite(sl.planSeq, [&] { std::copy(keep.begin(), keep.end(), sl.plan); });
        tp->pollHub(false);
    }

    // ---- 0.5: level trims, planned per section, and the master's passthrough ----------
    {
        ClearingProcessor* tp = procs[3].get();
        auto& sl = tp->getHub().get()->slots[tp->getSlot()];
        std::array<EqBandData, kEqBands> keep; TrimPlan keepT;
        seqRead(sl.planSeq, [&] { std::copy(sl.plan, sl.plan + kEqBands, keep.begin()); keepT = sl.trim; });
        auto set = [&](const char* id, float v) { auto* q = tp->apvts.getParameter(id); q->setValueNotifyingHost(q->convertTo0to1(v)); };
        FakePlayHead ph; tp->setPlayHead(&ph);
        TrimPlan t; t.baseDb = 1.5f; t.sectionDb[1] = 2.0f;
        seqWrite(sl.planSeq, [&] { std::fill(sl.plan, sl.plan + kEqBands, EqBandData {}); sl.trim = t; });
        tp->pollHub(false);
        ph.ppq = 4.0;  const double verse = sineGainDb(*tp, 1000.f, 80, 60, [](int) {});
        ph.ppq = 20.0; const double chorus = sineGainDb(*tp, 1000.f, 80, 60, [](int) {});
        set("trim", 2.0f);
        ph.ppq = 4.0;  const double withParam = sineGainDb(*tp, 1000.f, 80, 60, [](int) {});
        std::printf("level: planned verse %.2f dB, chorus %.2f dB, +2 trim param %.2f dB\n", verse, chorus, withParam);
        check(std::fabs(verse - 1.5) < 0.1 && std::fabs(chorus - 3.5) < 0.1, "planned level moves: whole song +1.5, +2 more in the Chorus");
        check(std::fabs(withParam - 3.5) < 0.1, "the automatable Trim adds to the planned level");
        set("trim", 0.0f);
        TrimPlan saved = tp->currentTrim();
        juce::MemoryBlock mb; tp->getStateInformation(mb);
        seqWrite(sl.planSeq, [&] { sl.trim = TrimPlan {}; }); tp->pollHub(false);
        tp->setStateInformation(mb.getData(), (int) mb.getSize());
        const auto back = tp->currentTrim();
        check(back.baseDb == saved.baseDb && back.sectionDb[1] == saved.sectionDb[1], "planned levels are saved and restored with the project");
        // Harmonics travel through the hub, are saved with the project, and are restored.
        ph.ppq = 20.0; // in the Chorus (Amount 1), so section scaling doesn't hide anything
        {
            HarmPlan hp; hp.bassOn = 1; hp.bassLo = 35; hp.bassHi = 85; hp.bassMixDb = -10; hp.presOn = 1; hp.presFreq = 2600;
            seqWrite(sl.planSeq, [&] { sl.harm = hp; }); tp->pollHub(false);
            juce::MemoryBlock hb; tp->getStateInformation(hb);
            seqWrite(sl.planSeq, [&] { sl.harm = HarmPlan {}; }); tp->pollHub(false);
            const bool cleared = tp->currentHarm().bassOn == 0;
            tp->setStateInformation(hb.getData(), (int) hb.getSize());
            const auto h2 = tp->currentHarm();
            check(cleared && h2.bassOn == 1 && h2.presOn == 1 && std::fabs(h2.bassMixDb + 10) < 0.01f && std::fabs(h2.presFreq - 2600) < 0.5f, "harmonics reach the instance and are saved and restored with the project");
            // Running: a 50 Hz sine gains energy (its added harmonics) only while harmonics are on.
            set("trim", 0.0f);
            seqWrite(sl.planSeq, [&] { std::fill(sl.plan, sl.plan + kEqBands, EqBandData {}); sl.trim = TrimPlan {}; sl.harm = hp; }); tp->pollHub(false);
            const double withH = sineGainDb(*tp, 50.f, 120, 60, [](int) {});
            seqWrite(sl.planSeq, [&] { sl.harm = HarmPlan {}; }); tp->pollHub(false);
            const double without = sineGainDb(*tp, 50.f, 120, 60, [](int) {});
            std::printf("harmonic bass on a 50 Hz sine: %+.2f dB total with, %+.2f dB without\n", withH, without);
            check(withH > without + 0.05 && withH < without + 3.0 && std::fabs(without) < 0.05, "harmonic bass adds a little (harmonics), nothing when off");
        }
        // Level-matched compare: with a -3 dB bell, COMPARE plays the unprocessed track 3 dB down too.
        {
            std::array<EqBandData, kEqBands> bell3 {};
            bell3[0].freq = 1000; bell3[0].gainDb = -3; bell3[0].q = 1.0f; bell3[0].type = (int) BandType::Bell; bell3[0].enabled = 1;
            seqWrite(sl.planSeq, [&] { std::copy(bell3.begin(), bell3.end(), sl.plan); sl.trim = TrimPlan {}; sl.harm = HarmPlan {}; });
            tp->pollHub(false);
            auto* hh = tp->getHub().get();
            const double proc = sineGainDb(*tp, 1000.f, 1200, 60, [](int) {});
            hh->compareMatched.store(1); hh->bypassAll.store(1);
            const double matched = sineGainDb(*tp, 1000.f, 120, 60, [](int) {});
            hh->compareMatched.store(0);
            const double raw = sineGainDb(*tp, 1000.f, 120, 60, [](int) {});
            hh->bypassAll.store(0); hh->compareMatched.store(1);
            std::printf("compare: processed %.2f dB, matched compare %.2f dB, unmatched compare %.2f dB (learned %.2f)\n", proc, matched, raw, tp->matchedCompareDb());
            check(std::fabs(proc + 3.0) < 0.2 && std::fabs(matched - proc) < 0.4 && std::fabs(raw) < 0.1, "COMPARE is level-matched: the unprocessed track plays at the processed loudness");
        }
        // Master mode: an EQ plan and trims are ignored; it only measures.
        std::array<EqBandData, kEqBands> bell {};
        bell[0].freq = 1000; bell[0].gainDb = -3; bell[0].q = 1.4f; bell[0].type = (int) BandType::Bell; bell[0].enabled = 1;
        seqWrite(sl.planSeq, [&] { std::copy(bell.begin(), bell.end(), sl.plan); });
        tp->pollHub(false);
        set("mode", 1.0f);
        const double mst = sineGainDb(*tp, 1000.f, 120, 60, [](int) {});
        std::printf("master mode with a -3 dB bell and +1.5 dB planned: %.2f dB\n", mst);
        check(std::fabs(mst) < 0.05, "Master mode passes audio untouched");
        set("mode", 0.0f);
        tp->setPlayHead(nullptr);
        // The timeline: bars were recorded while capturing
        seqWrite(sl.planSeq, [&] { std::copy(keep.begin(), keep.end(), sl.plan); sl.trim = keepT; });
        tp->pollHub(false);
    }

    ed->select(1);
    save(orch ? "orch_3_applied.png" : "band_3_applied.png");
    ed->showHelp(true);
    save(orch ? "orch_4_help.png" : "band_4_help.png");
    {
        // An assistant connected, with some activity: the WORK WITH CLAUDE page and the header chip.
        auto* hh = procs[0]->getHub().get();
        const char* acts[] = { "started listening", "stopped listening", "Lead Vox: role Lead Vocal, priority 0.95", "proposed 6 changes (nothing applied yet)", "applied 2 changes on Bass DI" };
        seqWrite(hh->llmSeq, [&] {
            std::strncpy(hh->llmClient, "Claude", sizeof hh->llmClient - 1); std::strncpy(hh->llmVersion, "0.7.0", sizeof hh->llmVersion - 1);
            for (auto* a : acts) { auto& e = hh->log[hh->logCount % HubData::kLog]; e.unixTime = (int64_t) std::time(nullptr); std::memset(e.text, 0, sizeof e.text); std::strncpy(e.text, a, sizeof e.text - 1); ++hh->logCount; }
        });
        hh->llmBeatNs.store(Hub::nowNs()); hh->llmLastCallNs.store(Hub::nowNs());
        ed->showHelpPage("claude");
        save(orch ? "orch_5_help_claude.png" : "band_5_help_claude.png");
        ed->showHelpPage("other");
        save(orch ? "orch_6_help_other.png" : "band_6_help_other.png");
        ed->showHelp(false);
        save(orch ? "orch_7_claude_chip.png" : "band_7_claude_chip.png");
    }
    ed.reset();
    procs.clear();
    shm_unlink("/clearing_snapshot");
    std::printf("%s\n", fails ? "SNAPSHOT RUN FAILED" : "SNAPSHOT RUN OK");
    return fails;
}
