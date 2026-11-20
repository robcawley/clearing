#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <random>

using namespace clr;

juce::AudioProcessorValueTreeState::ParameterLayout ClearingProcessor::layout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    StringArray roles;
    for (int i = 0; i < RoleCount; ++i) roles.add(roleName(i));
    // Setup controls: saved with the project, kept out of Logic's automation menu.
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "role", 1 }, "Role", roles, 0,
                                                 AudioParameterChoiceAttributes().withAutomatable(false)));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "priority", 1 }, "Priority", NormalisableRange<float>(0.f, 1.f, 0.01f), 0.4f,
                                                AudioParameterFloatAttributes().withAutomatable(false)));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "locked", 1 }, "Lock", false,
                                               AudioParameterBoolAttributes().withAutomatable(false)));
    // Bypass stays automatable.
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "bypass", 1 }, "Bypass", false));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "style", 1 }, "Style", StringArray { "Band / Pop", "Orchestral" }, 0,
                                                 AudioParameterChoiceAttributes().withAutomatable(false)));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "group", 1 }, "Group", StringArray { "1", "2", "3", "4", "5", "6", "7", "8" }, 0,
                                                 AudioParameterChoiceAttributes().withAutomatable(false)));
    // Automation: how much of this track's EQ is applied, and how deep its focus-following dips go.
    auto pct = AudioParameterFloatAttributes()
                   .withStringFromValueFunction([](float v, int) { return String(juce::roundToInt(v * 100.0f)) + "%"; })
                   .withValueFromStringFunction([](const String& t) { return t.getFloatValue() / 100.0f; })
                   .withLabel("%");
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "amount", 2 }, "Amount", NormalisableRange<float>(0.f, 1.f, 0.001f), 1.0f, pct));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "movement", 2 }, "Movement", NormalisableRange<float>(0.f, 1.f, 0.001f), 1.0f, pct));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "layer", 2 }, "Layer",
                                                 StringArray { "None", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6", "Layer 7", "Layer 8" }, 0,
                                                 AudioParameterChoiceAttributes().withAutomatable(false)));
    // Balance: a level trim after the EQ (automatable), and what this instance is.
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "trim", 3 }, "Trim", NormalisableRange<float>(-12.f, 12.f, 0.01f), 0.0f,
                                                AudioParameterFloatAttributes()
                                                    .withStringFromValueFunction([](float v, int) { return (v > 0 ? "+" : "") + String(v, 1) + " dB"; })
                                                    .withValueFromStringFunction([](const String& t) { return t.getFloatValue(); })
                                                    .withLabel("dB")));
    l.add(std::make_unique<AudioParameterChoice>(ParameterID { "mode", 3 }, "Mode", StringArray { "Track", "Master (stereo out)" }, 0,
                                                 AudioParameterChoiceAttributes().withAutomatable(false)));
    return l;
}

ClearingProcessor::ClearingProcessor()
    : juce::AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                            .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("CLEARING analysis"),
      apvts(*this, nullptr, "CLEARING", layout())
{
    pRole = apvts.getRawParameterValue("role");     pPriority = apvts.getRawParameterValue("priority");
    pLocked = apvts.getRawParameterValue("locked"); pBypass = apvts.getRawParameterValue("bypass");
    pStyle = apvts.getRawParameterValue("style");   pGroup = apvts.getRawParameterValue("group");
    pAmount = apvts.getRawParameterValue("amount"); pMovement = apvts.getRawParameterValue("movement");
    pLayer = apvts.getRawParameterValue("layer");
    pTrim = apvts.getRawParameterValue("trim"); pMode = apvts.getRawParameterValue("mode");
    fifoBuf.assign((size_t) fifo.getTotalSize(), 0.0f);
    fifoBar.assign((size_t) fifo.getTotalSize(), 0);
    stL.assign((size_t) stFifo.getTotalSize(), 0.0f); stR = stL; stBar.assign((size_t) stFifo.getTotalSize(), 0);
    drainL.assign(4096, 0.0f); drainR = drainL; drainStBar.assign(4096, 0);
    drain.assign(4096, 0.0f);
    drainBar.assign(4096, 0);
    analyser.prepare(fs);

    std::random_device rd;
    uint64_t id = ((uint64_t) rd() << 32) ^ (uint64_t) rd() ^ clr::Hub::nowNs();
    if (id == 0) id = 1;
    uid.store(id);
    if (hub.open()) slot = hub.claim(id);
    if (slot >= 0) {
        // A reused slot may hold the previous owner's plan and controls: start clean.
        seenCtrl.store(hub.get()->slots[slot].ctrlSeq.load());
        mirrorPlan();
    }
    startThread(juce::Thread::Priority::low);
}

ClearingProcessor::~ClearingProcessor()
{
    alive->store(false);
    stopThread(2000);
    hub.release(slot, uid.load());
    hub.close();
}

// The name shown everywhere: one the user typed, else the host's track name, else "Track N".
juce::String ClearingProcessor::getTrackName() const
{
    const juce::SpinLock::ScopedLockType l(nameLock);
    if (userName.isNotEmpty()) return userName;
    if (hostName.isNotEmpty()) return hostName;
    return "Track " + juce::String(slot >= 0 ? slot + 1 : 1);
}
void ClearingProcessor::setTrackName(const juce::String& n) { const juce::SpinLock::ScopedLockType l(nameLock); userName = n.trim().substring(0, 22); }
void ClearingProcessor::updateTrackProperties(const TrackProperties& p)
{
    const juce::SpinLock::ScopedLockType l(nameLock);
    hostName = p.name.trim().substring(0, 22);
}

std::array<EqBandData, kEqBands> ClearingProcessor::currentPlan() const
{
    std::array<EqBandData, kEqBands> r;
    for (int i = 0; i < kEqBands; ++i) {
        r[i].freq = bands[i].freq.load(); r[i].gainDb = bands[i].gain.load(); r[i].q = bands[i].q.load();
        r[i].type = bands[i].type.load(); r[i].enabled = bands[i].on.load(); r[i].dynamic = bands[i].dynamic.load();
        r[i].threshDb = bands[i].thresh.load(); r[i].leaderUid = bands[i].leaderUid.load();
        r[i].sectionMask = bands[i].sectionMask.load(); r[i].self = bands[i].self.load(); r[i].ruleId = bands[i].ruleId.load();
    }
    return r;
}

TrimPlan ClearingProcessor::currentTrim() const
{
    TrimPlan t; t.baseDb = trimBase.load();
    for (int k = 0; k < kSections; ++k) t.sectionDb[k] = trimSec[k].load();
    return t;
}

void ClearingProcessor::setParam(const char* id, float v)
{
    if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(v));
}

void ClearingProcessor::prepareToPlay(double sampleRate, int)
{
    fs = sampleRate;
    {
        const juce::CriticalSection::ScopedLockType l(analyseLock);
        analyser.prepare(sampleRate);
        fifo.reset();
    }
    for (auto& f : filt) for (auto& c : f) c.reset();
    for (int i = 0; i < kEqBands; ++i) {
        smoothGain[i] = 0; smoothAct[i] = 0; appliedGain[i] = 1e9f;
        det[i].reset(); detFreq[i] = 0; detFast[i] = -120.0f; detSlow[i] = -120.0f; detWarm[i] = 0;
    }
    envSlow = -120.0f; envWarm = 0;
    gainDbSmooth = 0; gainLin = 1.0f;
    envDb = -120.0f;
    harmStage.prepare(sampleRate); harmSeen = 0xffffffffu;
    eInS = eOutS = 0; onSamples = 0;
    {
        const juce::CriticalSection::ScopedLockType l(analyseLock);
        health.prepare(sampleRate);
        stFifo.reset();
    }
}

// ---------------------------------------------------------------------------------------------
// Audio thread

void ClearingProcessor::updateSection(int n)
{
    float tA = 1.0f, tM = 1.0f;
    int cur = -1;
    curBar.store(0, std::memory_order_relaxed); // until the playhead says otherwise
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto ppq = pos->getPpqPosition()) {
                double num = 4, den = 4;
                if (auto ts = pos->getTimeSignature()) { num = ts->numerator; den = ts->denominator; }
                const double barLen = num * 4.0 / den;
                const double bar = std::floor(*ppq / barLen + 1e-9) + 1.0;
                curBar.store(pos->getIsPlaying() && bar >= 1 ? (int) bar : 0, std::memory_order_relaxed);
                if (auto bpm = pos->getBpm()) curBpm.store((float) *bpm, std::memory_order_relaxed);
                curNum.store((int) num, std::memory_order_relaxed); curDen.store((int) den, std::memory_order_relaxed);
                const auto& set = secBuf[secActive.load(std::memory_order_acquire)];
                for (int k = 0; k < set.n; ++k)
                    if (bar >= set.s[k].startBar && bar <= set.s[k].endBar) { tA = set.s[k].amount; tM = set.s[k].movement; cur = k; break; }
            }
    curSection.store(cur, std::memory_order_relaxed);
    curSec = cur;
    const float a = 1.0f - std::exp(-(float) n / (0.2f * (float) fs)); // section moves glide over ~200 ms
    secAmt += (tA - secAmt) * a;
    secMov += (tM - secMov) * a;
}

void ClearingProcessor::updateFilters(int numCh, int n)
{
    auto* h = hub.get();
    const bool off = (h && h->bypassAll.load(std::memory_order_relaxed)) || *pBypass > 0.5f;
    const float amount = pAmount->load() * secAmt;
    const float movement = pMovement->load() * secMov;
    const float nf = (float) n, fsf = (float) fs;
    const float gA = 1.0f - std::exp(-nf / (0.03f * fsf));   // 30 ms: automation without zipper noise
    const float aAtt = 1.0f - std::exp(-nf / (0.04f * fsf)); // dips: 40 ms in
    const float aRel = 1.0f - std::exp(-nf / (0.35f * fsf)); // 350 ms back
    const uint64_t now = Hub::nowNs();

    for (int i = 0; i < kEqBands; ++i) {
        float target = 0.0f;
        const uint32_t mask = bands[i].sectionMask.load(std::memory_order_relaxed);
        const bool inSection = mask == 0 || (curSec >= 0 && curSec < 32 && ((mask >> curSec) & 1u));
        if (bands[i].on.load() && !off && bands[i].self.load()) {
            // Self-triggered: moves only when this track builds up in the band (beyond its recent norm).
            const float rise = (detFast[i] - detSlow[i]) - juce::jmax(0.0f, envDb - envSlow);
            const float act = inSection ? juce::jlimit(0.0f, 1.0f, (rise - 3.0f) / 6.0f) : 0.0f;
            smoothAct[i] += (act - smoothAct[i]) * (act > smoothAct[i] ? aAtt : aRel);
            target = bands[i].gain.load() * smoothAct[i] * amount;
        } else if (bands[i].on.load() && !off) {
            if (bands[i].dynamic.load()) {
                float env = -120.0f;
                const int ls = bands[i].leaderSlot.load(std::memory_order_relaxed);
                if (h && ls >= 0 && ls < kSlots) {
                    auto& L = h->slots[ls];
                    if (L.owner.load(std::memory_order_relaxed) == bands[i].leaderUid.load(std::memory_order_relaxed)
                        && now - L.envNs.load(std::memory_order_relaxed) < 300'000'000ull)
                        env = L.envDb.load(std::memory_order_relaxed);
                }
                const float act = inSection ? juce::jlimit(0.0f, 1.0f, (env - bands[i].thresh.load()) / 9.0f) : 0.0f;
                smoothAct[i] += (act - smoothAct[i]) * (act > smoothAct[i] ? aAtt : aRel);
                target = bands[i].gain.load() * smoothAct[i] * movement * amount;
            } else {
                target = bands[i].gain.load() * amount;
            }
        }
        smoothGain[i] += (target - smoothGain[i]) * gA;
        if (std::fabs(target - smoothGain[i]) < 0.002f) smoothGain[i] = target;
        const float f = bands[i].freq.load(), q = bands[i].q.load();
        const int t = bands[i].type.load();
        if (std::fabs(smoothGain[i] - appliedGain[i]) > 0.0005f || f != appliedFreq[i] || q != appliedQ[i] || t != appliedType[i]) {
            const auto c = makeCoeffs((BandType) t, fs, f, smoothGain[i], q);
            for (int ch = 0; ch < numCh && ch < 2; ++ch) filt[i][ch].set(c);
            appliedGain[i] = smoothGain[i]; appliedFreq[i] = f; appliedQ[i] = q; appliedType[i] = t;
        }
    }
}

void ClearingProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    const int n = buf.getNumSamples(), nch = juce::jmin(2, buf.getNumChannels());
    if (n <= 0) return;

    // Tap the input (before our own EQ, so measurements never chase our corrections).
    const float* l = buf.getReadPointer(0);
    const float* r = buf.getReadPointer(nch > 1 ? 1 : 0);
    updateSection(n); // also the bar this block belongs to, which travels with the audio
    if (auto* hh = hub.get(); hh && hh->listening.load(std::memory_order_relaxed)) {
        float pk = 0;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        if (pk > inPeak.load(std::memory_order_relaxed)) inPeak.store(pk, std::memory_order_relaxed);
    }
    {
        const auto barNow = (int16_t) curBar.load(std::memory_order_relaxed);
        int s1, z1, s2, z2;
        fifo.prepareToWrite(n, s1, z1, s2, z2);
        for (int i = 0; i < z1; ++i) { fifoBuf[(size_t) (s1 + i)] = 0.5f * (l[i] + r[i]); fifoBar[(size_t) (s1 + i)] = barNow; }
        for (int i = 0; i < z2; ++i) { fifoBuf[(size_t) (s2 + i)] = 0.5f * (l[z1 + i] + r[z1 + i]); fifoBar[(size_t) (s2 + i)] = barNow; }
        fifo.finishedWrite(z1 + z2);
    }

    // This track's level, published for any track that follows it.
    double e = 0;
    for (int i = 0; i < n; ++i) e += 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
    e /= n;
    const float blockDb = e > 1e-12 ? (float) (10.0 * std::log10(e)) : -120.0f;
    const float aA = 1.0f - std::exp(-(float) n / (0.010f * (float) fs));
    const float aR = 1.0f - std::exp(-(float) n / (0.150f * (float) fs));
    envDb += (blockDb - envDb) * (blockDb > envDb ? aA : aR);
    if (auto* h = hub.get(); h && slot >= 0) {
        h->slots[slot].envDb.store(envDb, std::memory_order_relaxed);
        h->slots[slot].envNs.store(Hub::nowNs(), std::memory_order_relaxed);
    }

    // Detectors for self-triggered bands (on the input, before our own EQ).
    for (int b = 0; b < kEqBands; ++b) {
        if (!bands[b].on.load(std::memory_order_relaxed) || !bands[b].self.load(std::memory_order_relaxed)) continue;
        const float f = bands[b].freq.load();
        if (f != detFreq[b]) { det[b].set(makeCoeffs(BandType::BandPass, fs, f, 0.0, juce::jmax(0.7f, bands[b].q.load()))); detFreq[b] = f; }
        double de = 0;
        for (int i = 0; i < n; ++i) { const float y = det[b].process(0.5f * (l[i] + r[i])); de += (double) y * y; }
        const float d = de > 1e-14 * n ? (float) (10.0 * std::log10(de / n)) : -120.0f;
        detFast[b] += (d - detFast[b]) * (d > detFast[b] ? aA : 1.0f - std::exp(-(float) n / (0.12f * (float) fs)));
        if (detFast[b] > -80.0f) { // learn the band's usual level: quickly over the first second, then slowly
            if (detSlow[b] < -100.0f) detSlow[b] = detFast[b];
            const float tau = detWarm[b] < (float) fs ? 0.25f : 4.0f;
            detSlow[b] += (detFast[b] - detSlow[b]) * (1.0f - std::exp(-(float) n / (tau * (float) fs)));
            detWarm[b] += (float) n;
        }
    }
    // The whole part's usual level, so a band only counts as building up if it rises more than the part does.
    if (envDb > -80.0f) {
        if (envSlow < -100.0f) envSlow = envDb;
        const float tau = envWarm < (float) fs ? 0.25f : 4.0f;
        envSlow += (envDb - envSlow) * (1.0f - std::exp(-(float) n / (tau * (float) fs)));
        envWarm += (float) n;
    }

    const bool master = pMode->load() > 0.5f;
    if (master) { // the stereo out, both channels, for mix health
        const auto barNow = (int16_t) curBar.load(std::memory_order_relaxed);
        int s1, z1, s2, z2;
        stFifo.prepareToWrite(n, s1, z1, s2, z2);
        for (int i = 0; i < z1; ++i) { stL[(size_t) (s1 + i)] = l[i]; stR[(size_t) (s1 + i)] = r[i]; stBar[(size_t) (s1 + i)] = barNow; }
        for (int i = 0; i < z2; ++i) { stL[(size_t) (s2 + i)] = l[z1 + i]; stR[(size_t) (s2 + i)] = r[z1 + i]; stBar[(size_t) (s2 + i)] = barNow; }
        stFifo.finishedWrite(z1 + z2);
    }
    auto* hb = hub.get();
    const bool comparing = hb && hb->bypassAll.load(std::memory_order_relaxed);
    const bool offNow = comparing || *pBypass > 0.5f;
    const double ePre = e;
    if (!master) {
        updateFilters(nch, n);
        for (int b = 0; b < kEqBands; ++b) {
            if (smoothGain[b] == 0.0f && appliedGain[b] == 0.0f) continue; // flat: skip
            for (int ch = 0; ch < nch; ++ch) {
                float* d = buf.getWritePointer(ch);
                for (int i = 0; i < n; ++i) d[i] = filt[b][ch].process(d[i]);
            }
        }
        // Harmonics: a new plan is copied in when the lock is free (never waits).
        if (harmGen.load(std::memory_order_acquire) != harmSeen) {
            const juce::SpinLock::ScopedTryLockType tl(harmLock);
            if (tl.isLocked()) { harmLocal = harmShared; harmSeen = harmGen.load(); }
        }
        harmStage.set(harmLocal);
        if (harmLocal.bassOn > 0 || harmLocal.presOn > 0 || harmStage.active()) {
            float* chans[2] = { buf.getWritePointer(0), buf.getWritePointer(nch > 1 ? 1 : 0) };
            harmStage.process(chans, nch, n, offNow ? 0.0f : juce::jlimit(0.0f, 1.0f, pAmount->load() * secAmt));
        }
        // Level-matched compare: learn how much louder or quieter our processing makes this track
        // (over ~3 s of real signal, only while it's active and settled).
        if (!offNow) onSamples += (float) n; else onSamples = 0;
        if (onSamples > 0.5f * (float) fs && ePre > 1e-8) {
            double ePost = 0;
            for (int ch = 0; ch < nch; ++ch) { const float* d = buf.getReadPointer(ch); for (int i = 0; i < n; ++i) ePost += (double) d[i] * d[i]; }
            ePost /= (double) (n * nch);
            const double a = 1.0 - std::exp(-(double) n / (3.0 * fs));
            if (eInS <= 0) { eInS = ePre; eOutS = ePost; }
            eInS += (ePre - eInS) * a; eOutS += (ePost - eOutS) * a;
            matchDb.store((float) juce::jlimit(-6.0, 6.0, 10.0 * std::log10((eOutS + 1e-20) / (eInS + 1e-20))), std::memory_order_relaxed);
        }
    }

    // Level: the Trim parameter plus planned trims (whole song + this section), gliding ~50 ms.
    {
        auto* h = hub.get();
        const bool off = (h && h->bypassAll.load(std::memory_order_relaxed)) || *pBypass > 0.5f;
        float target = pTrim->load();
        if (!master && !off) {
            target += trimBase.load(std::memory_order_relaxed);
            if (curSec >= 0 && curSec < kSections) target += trimSec[curSec].load(std::memory_order_relaxed);
        }
        if (off) target = 0.0f;
        // Matched compare: the unprocessed track plays at the processed loudness, so the A/B is about tone.
        if (!master && h && h->bypassAll.load(std::memory_order_relaxed) && h->compareMatched.load(std::memory_order_relaxed) && *pBypass <= 0.5f)
            target = matchDb.load(std::memory_order_relaxed) + pTrim->load();
        gainDbSmooth += (target - gainDbSmooth) * (1.0f - std::exp(-(float) n / (0.05f * (float) fs)));
        if (std::fabs(target - gainDbSmooth) < 0.001f) gainDbSmooth = target;
        const float g1 = std::pow(10.0f, gainDbSmooth / 20.0f);
        if (g1 != 1.0f || gainLin != 1.0f) {
            for (int ch = 0; ch < nch; ++ch) {
                float* d = buf.getWritePointer(ch);
                for (int i = 0; i < n; ++i) d[i] *= gainLin + (g1 - gainLin) * (float) (i + 1) / (float) n; // ramp: no zipper
            }
        }
        gainLin = g1;
    }
}

// ---------------------------------------------------------------------------------------------
// Background thread

void ClearingProcessor::pump()
{
    const juce::CriticalSection::ScopedLockType l(analyseLock);
    auto* h = hub.get();
    const bool listening = h && h->listening.load() != 0;
    if (h) {
        const uint32_t e = h->listenEpoch.load();
        if (e != seenEpoch) { seenEpoch = e; analyser.resetCapture(); health.reset(); inPeak.store(0); }
    }
    // Master: mix health from the stereo out, while listening.
    for (;;) {
        const int ready = stFifo.getNumReady();
        if (ready <= 0) break;
        const int take = juce::jmin(ready, (int) drainL.size());
        int s1, z1, s2, z2;
        stFifo.prepareToRead(take, s1, z1, s2, z2);
        std::copy(stL.begin() + s1, stL.begin() + s1 + z1, drainL.begin()); std::copy(stL.begin() + s2, stL.begin() + s2 + z2, drainL.begin() + z1);
        std::copy(stR.begin() + s1, stR.begin() + s1 + z1, drainR.begin()); std::copy(stR.begin() + s2, stR.begin() + s2 + z2, drainR.begin() + z1);
        std::copy(stBar.begin() + s1, stBar.begin() + s1 + z1, drainStBar.begin()); std::copy(stBar.begin() + s2, stBar.begin() + s2 + z2, drainStBar.begin() + z1);
        stFifo.finishedRead(z1 + z2);
        if (!listening) continue;
        healthOn = true;
        for (int a = 0, tot = z1 + z2; a < tot;) {
            int e2 = a + 1;
            while (e2 < tot && drainStBar[(size_t) e2] == drainStBar[(size_t) a]) ++e2;
            health.process(drainL.data() + a, drainR.data() + a, e2 - a, drainStBar[(size_t) a]);
            a = e2;
        }
    }
    for (;;) {
        const int ready = fifo.getNumReady();
        if (ready <= 0) break;
        const int take = juce::jmin(ready, (int) drain.size());
        int s1, z1, s2, z2;
        fifo.prepareToRead(take, s1, z1, s2, z2);
        std::copy(fifoBuf.begin() + s1, fifoBuf.begin() + s1 + z1, drain.begin());
        std::copy(fifoBuf.begin() + s2, fifoBuf.begin() + s2 + z2, drain.begin() + z1);
        std::copy(fifoBar.begin() + s1, fifoBar.begin() + s1 + z1, drainBar.begin());
        std::copy(fifoBar.begin() + s2, fifoBar.begin() + s2 + z2, drainBar.begin() + z1);
        fifo.finishedRead(z1 + z2);
        // push in runs of one bar, so every sample lands in the bar it was played in
        for (int a = 0, tot = z1 + z2; a < tot;) {
            int e2 = a + 1;
            while (e2 < tot && drainBar[(size_t) e2] == drainBar[(size_t) a]) ++e2;
            analyser.setBar(drainBar[(size_t) a]);
            analyser.push(drain.data() + a, e2 - a, listening);
            a = e2;
        }
    }
}

void ClearingProcessor::publish()
{
    auto* h = hub.get();
    if (!h || slot < 0) return;
    auto& s = h->slots[slot];
    std::array<float, kBands> live, cap, capPk;
    float act, rms, capRms;
    {
        const juce::CriticalSection::ScopedLockType l(analyseLock);
        live = analyser.liveDb(); cap = analyser.captureDb(); act = analyser.activeSec(); rms = analyser.rmsDb();
        capPk = analyser.capturePeakDb();
        capRms = analyser.captureRmsDb();
    }
    const juce::String nameNow = getTrackName();
    bool hostNamed;
    { const juce::SpinLock::ScopedLockType l(nameLock); hostNamed = userName.isEmpty() && hostName.isNotEmpty(); }
    Pending p;
    const bool pending = pendGen.load() != pendDone.load();
    if (pending) { const juce::SpinLock::ScopedLockType l(pendLock); p = pend; }
    seqWrite(s.measSeq, [&] {
        std::strncpy(s.name, nameNow.toRawUTF8(), sizeof s.name - 1); s.name[sizeof s.name - 1] = 0;
        s.hostNamed = hostNamed;
        s.role = pending ? p.role : (int) *pRole;
        s.locked = pending ? p.locked : (*pLocked > 0.5f);
        s.priority = pending ? p.prio : pPriority->load();
        s.layer = pending ? p.layer : (int) *pLayer;
        s.amount = pending ? p.amount : pAmount->load();
        s.movement = pending ? p.movement : pMovement->load();
        s.group = groupId() + 1; s.style = (int) *pStyle; s.section = curSection.load();
        std::copy(live.begin(), live.end(), s.live); std::copy(cap.begin(), cap.end(), s.cap); std::copy(capPk.begin(), capPk.end(), s.capPeak);
        s.activeSec = act; s.rmsDb = rms; s.capRmsDb = capRms;
        { const float p = inPeak.load(); s.capPeakDb = p > 1e-6f ? 20.0f * std::log10(p) : -120.0f; }
        s.mode = pMode->load() > 0.5f ? 1 : 0; s.trimParamDb = pTrim->load();
        s.bpm = curBpm.load(); s.meterNum = curNum.load(); s.meterDen = curDen.load();
        s.matchDb = matchDb.load();
    });
    if (healthOn) {
        const juce::CriticalSection::ScopedLockType l(analyseLock);
        const int last = health.lastBar();
        const float I = health.integratedLufs(), S = health.maxShortTermLufs(), tp = health.truePeakDb(), sp = health.samplePeakDb();
        seqWrite(s.hSeq, [&] {
            s.hBars = last;
            for (int b = 1; b <= last; ++b) {
                const auto& hb = health.bar(b);
                for (int k = 0; k < kHBands; ++k) {
                    s.hCorr[b - 1][k] = HealthAnalyser::correlation(hb, k);
                    s.hMono[b - 1][k] = HealthAnalyser::monoLossDb(hb, k);
                    s.hEn[b - 1][k] = HealthAnalyser::energyDb(hb, k);
                }
                s.hTp[b - 1] = hb.tpDb;
            }
            s.hLufsI = I; s.hLufsSmax = S; s.hTpMax = tp; s.hSpMax = sp;
        });
    }
    {
        const juce::CriticalSection::ScopedLockType l(analyseLock);
        const int last = analyser.lastBar();
        seqWrite(s.tlSeq, [&] {
            s.tlBars = last;
            for (int b = 1; b <= last; ++b) {
                for (int k = 0; k < kCoarse; ++k) s.tl[b - 1][k] = analyser.barDb(b, k);
                s.tlLufs[b - 1] = analyser.barLufs(b);
            }
        });
    }
    s.heartbeatNs.store(Hub::nowNs());
}

void ClearingProcessor::applyPlan(const EqBandData* b, const TrimPlan& t, const HarmPlan& hp)
{
    { const juce::SpinLock::ScopedLockType l(harmLock); harmShared = hp; }
    harmGen.fetch_add(1, std::memory_order_release);
    trimBase.store(t.baseDb);
    for (int k = 0; k < kSections; ++k) trimSec[k].store(t.sectionDb[k]);
    for (int i = 0; i < kEqBands; ++i) {
        bands[i].freq.store(b[i].freq); bands[i].gain.store(b[i].gainDb); bands[i].q.store(b[i].q);
        bands[i].type.store(b[i].type); bands[i].dynamic.store(b[i].dynamic); bands[i].thresh.store(b[i].threshDb);
        bands[i].leaderUid.store(b[i].leaderUid);
        bands[i].sectionMask.store(b[i].sectionMask); bands[i].self.store(b[i].self); bands[i].ruleId.store(b[i].ruleId);
        bands[i].leaderSlot.store(b[i].dynamic && !b[i].self ? hub.find(b[i].leaderUid) : -1);
        bands[i].on.store(b[i].enabled);
    }
}

// Publish this instance's own plan into its slot, so readers see what is really applied.
void ClearingProcessor::mirrorPlan()
{
    auto* h = hub.get();
    if (!h || slot < 0) return;
    auto& s = h->slots[slot];
    const auto cur = currentPlan();
    const auto tr = currentTrim();
    const auto hp = currentHarm();
    seqWrite(s.planSeq, [&] { std::copy(cur.begin(), cur.end(), s.plan); s.trim = tr; s.harm = hp; });
    seenPlan.store(s.planSeq.load());
}

void ClearingProcessor::pollHub(bool applyUi)
{
    auto* h = hub.get();
    if (!h || slot < 0) return;
    auto& s = h->slots[slot];

    const uint32_t ps = s.planSeq.load();
    if (ps != seenPlan && !(ps & 1)) {
        EqBandData tmp[kEqBands];
        TrimPlan tr; HarmPlan hp;
        if (seqRead(s.planSeq, [&] { std::copy(s.plan, s.plan + kEqBands, tmp); tr = s.trim; hp = s.harm; })) {
            applyPlan(tmp, tr, hp);
            seenPlan = ps; // the version we know we've seen; a newer one is re-read next poll
            // The plan is project state, not a parameter: tell the host the project changed.
            auto alv = alive;
            juce::MessageManager::callAsync([this, alv] {
                if (alv->load()) updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
            });
        }
    }
    const uint32_t cs = s.ctrlSeq.load();
    if (applyUi && cs != seenCtrl && !(cs & 1)) {
        char nm[24]; int role = 0, locked = 0, layer = -1; float prio = 0.4f, amount = -1, movement = -1;
        if (seqRead(s.ctrlSeq, [&] {
                std::memcpy(nm, s.ctrlName, sizeof nm); role = s.ctrlRole; locked = s.ctrlLocked; prio = s.ctrlPriority;
                layer = s.ctrlLayer; amount = s.ctrlAmount; movement = s.ctrlMovement;
            })) {
            seenCtrl = s.ctrlSeq.load();
            nm[sizeof nm - 1] = 0;
            // Re-sending the name already shown (host name or the fallback) is not a rename.
            const juce::String req = juce::String::fromUTF8(nm).trim();
            juce::String host, user;
            { const juce::SpinLock::ScopedLockType l(nameLock); host = hostName; user = userName; }
            const bool sameAsShown = user.isEmpty() && req == getTrackName();
            setTrackName(req == host || sameAsShown ? juce::String() : req);

            Pending p;
            p.role = role; p.locked = locked; p.prio = prio;
            p.layer = layer >= 0 ? juce::jlimit(0, 8, layer) : (int) *pLayer;
            p.amount = amount >= 0 ? juce::jlimit(0.0f, 1.0f, amount) : pAmount->load();
            p.movement = movement >= 0 ? juce::jlimit(0.0f, 1.0f, movement) : pMovement->load();
            { const juce::SpinLock::ScopedLockType l(pendLock); pend = p; }
            const uint32_t gen = pendGen.fetch_add(1) + 1;
            publish();
            auto alv = alive;
            juce::MessageManager::callAsync([this, alv, p, gen, amount, movement, layer] {
                if (!alv->load()) return;
                setParam("role", (float) p.role); setParam("locked", p.locked ? 1.f : 0.f); setParam("priority", p.prio);
                if (layer >= 0) setParam("layer", (float) p.layer);
                // Amount / movement are automatable: a gesture so Logic records them in Touch/Latch.
                for (auto [id, v, given] : { std::tuple<const char*, float, bool> { "amount", p.amount, amount >= 0 },
                                             std::tuple<const char*, float, bool> { "movement", p.movement, movement >= 0 } })
                    if (given)
                        if (auto* prm = apvts.getParameter(id)) {
                            prm->beginChangeGesture(); prm->setValueNotifyingHost(prm->convertTo0to1(v)); prm->endChangeGesture();
                        }
                if (pendGen.load() == gen) pendDone.store(gen);
            });
        }
    }
}

void ClearingProcessor::syncShared()
{
    auto* h = hub.get();
    if (!h) return;
    for (auto& b : bands)
        b.leaderSlot.store(b.dynamic.load() && b.on.load() && !b.self.load() ? hub.find(b.leaderUid.load()) : -1);

    const juce::CriticalSection::ScopedLockType sl(syncLock);
    const uint32_t bs = h->briefSeq.load();
    if (bs == seenBrief || (bs & 1)) return;
    char brief[kBriefLen];
    SecSet set;
    if (!seqRead(h->briefSeq, [&] {
            std::memcpy(brief, h->brief, sizeof brief);
            set.n = juce::jlimit(0, kSections, (int) h->numSections);
            std::copy(h->sections, h->sections + kSections, set.s);
        })) return;
    // Only mark as seen the version actually copied; a newer write is picked up next time.
    if (h->briefSeq.load() != bs) return;
    seenBrief = bs;
    brief[kBriefLen - 1] = 0;
    {
        const juce::CriticalSection::ScopedLockType l(sharedLock);
        briefCache = juce::String::fromUTF8(brief);
        sectionsCache.assign(set.s, set.s + set.n);
    }
    const int next = 1 - secActive.load();
    secBuf[next] = set;
    secActive.store(next, std::memory_order_release);
}

void ClearingProcessor::run()
{
    int tick = 0;
    while (!threadShouldExit()) {
        pump();
        if ((tick++ % 5) == 0) { publish(); pollHub(); syncShared(); }
        wait(20);
    }
}

juce::AudioProcessorEditor* ClearingProcessor::createEditor() { return new ClearingEditor(*this); }

// ---------------------------------------------------------------------------------------------
// State

void ClearingProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    { const juce::SpinLock::ScopedLockType l(nameLock); state.setProperty("trackName", userName, nullptr); }
    state.setProperty("uid", juce::String::toHexString((juce::int64) uid.load()), nullptr);
    juce::ValueTree plan("plan");
    for (auto& b : currentPlan()) {
        juce::ValueTree v("band");
        v.setProperty("f", b.freq, nullptr).setProperty("g", b.gainDb, nullptr).setProperty("q", b.q, nullptr)
         .setProperty("t", b.type, nullptr).setProperty("on", b.enabled, nullptr).setProperty("dyn", b.dynamic, nullptr)
         .setProperty("th", b.threshDb, nullptr).setProperty("lead", juce::String::toHexString((juce::int64) b.leaderUid), nullptr)
         .setProperty("sec", (juce::int64) b.sectionMask, nullptr).setProperty("self", b.self, nullptr).setProperty("rule", b.ruleId, nullptr);
        plan.appendChild(v, nullptr);
    }
    {
        const auto tr = currentTrim();
        juce::ValueTree t("trim");
        t.setProperty("base", tr.baseDb, nullptr);
        for (int k = 0; k < kSections; ++k) if (tr.sectionDb[k] != 0.0f) t.setProperty("s" + juce::String(k), tr.sectionDb[k], nullptr);
        plan.appendChild(t, nullptr);
    }
    {
        const auto hp = currentHarm();
        juce::ValueTree t("harm");
        t.setProperty("bOn", hp.bassOn, nullptr).setProperty("bLo", hp.bassLo, nullptr).setProperty("bHi", hp.bassHi, nullptr)
         .setProperty("bDr", hp.bassDrive, nullptr).setProperty("bMix", hp.bassMixDb, nullptr).setProperty("bHp", hp.bassHp, nullptr)
         .setProperty("bLp", hp.bassLp, nullptr).setProperty("sub", hp.subDb, nullptr)
         .setProperty("pOn", hp.presOn, nullptr).setProperty("pF", hp.presFreq, nullptr).setProperty("pQ", hp.presQ, nullptr)
         .setProperty("pDr", hp.presDrive, nullptr).setProperty("pMix", hp.presMixDb, nullptr);
        plan.appendChild(t, nullptr);
    }
    state.removeChild(state.getChildWithName("plan"), nullptr);
    state.appendChild(plan, nullptr);

    juce::ValueTree arr("arrangement");
    {
        const juce::CriticalSection::ScopedLockType l(sharedLock);
        arr.setProperty("brief", briefCache, nullptr);
        for (auto& s : sectionsCache) {
            juce::ValueTree v("section");
            v.setProperty("name", juce::String::fromUTF8(s.name), nullptr).setProperty("start", s.startBar, nullptr)
             .setProperty("end", s.endBar, nullptr).setProperty("amount", s.amount, nullptr)
             .setProperty("movement", s.movement, nullptr).setProperty("feel", juce::String::fromUTF8(s.feel), nullptr)
             .setProperty("lead", juce::String::toHexString((juce::int64) s.leadUid), nullptr);
            arr.appendChild(v, nullptr);
        }
    }
    state.removeChild(state.getChildWithName("arrangement"), nullptr);
    state.appendChild(arr, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void ClearingProcessor::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (!xml) return;
    auto state = juce::ValueTree::fromXml(*xml);
    if (!state.isValid()) return;
    setTrackName(state.getProperty("trackName").toString());

    // Keep the same identity across sessions so tracks that follow this one still find it.
    // A duplicate (copied track) whose original is already live keeps its fresh id.
    const uint64_t saved = (uint64_t) state.getProperty("uid").toString().getHexValue64();
    if (saved != 0 && saved != uid.load() && slot >= 0 && hub.find(saved) < 0) {
        uint64_t cur = uid.load();
        if (hub.get()->slots[slot].owner.compare_exchange_strong(cur, saved)) uid.store(saved);
    }

    auto plan = state.getChildWithName("plan");
    EqBandData tmp[kEqBands];
    for (int i = 0; i < kEqBands && i < plan.getNumChildren(); ++i) {
        auto b = plan.getChild(i);
        tmp[i].freq = (float) (double) b["f"]; tmp[i].gainDb = (float) (double) b["g"]; tmp[i].q = (float) (double) b["q"];
        tmp[i].type = (int) b["t"]; tmp[i].enabled = (int) b["on"]; tmp[i].dynamic = (int) b.getProperty("dyn", 0);
        tmp[i].threshDb = (float) (double) b.getProperty("th", -120.0);
        tmp[i].leaderUid = (uint64_t) b.getProperty("lead", "0").toString().getHexValue64();
        tmp[i].sectionMask = (uint32_t) (juce::int64) b.getProperty("sec", 0); tmp[i].self = (int) b.getProperty("self", 0);
        tmp[i].ruleId = (int) b.getProperty("rule", 0);
    }
    TrimPlan tr;
    {
        auto t = plan.getChildWithName("trim");
        if (t.isValid()) {
            tr.baseDb = (float) (double) t.getProperty("base", 0.0);
            for (int k = 0; k < kSections; ++k) tr.sectionDb[k] = (float) (double) t.getProperty("s" + juce::String(k), 0.0);
        }
    }
    HarmPlan hp;
    {
        auto t = plan.getChildWithName("harm");
        if (t.isValid()) {
            auto f = [&](const char* k, float d) { return (float) (double) t.getProperty(k, d); };
            hp.bassOn = (int) t.getProperty("bOn", 0); hp.bassLo = f("bLo", 30); hp.bassHi = f("bHi", 90); hp.bassDrive = f("bDr", 0.5f);
            hp.bassMixDb = f("bMix", -12); hp.bassHp = f("bHp", 100); hp.bassLp = f("bLp", 400); hp.subDb = f("sub", 0);
            hp.presOn = (int) t.getProperty("pOn", 0); hp.presFreq = f("pF", 2500); hp.presQ = f("pQ", 1.2f);
            hp.presDrive = f("pDr", 0.4f); hp.presMixDb = f("pMix", -15);
        }
    }
    applyPlan(tmp, tr, hp);
    mirrorPlan();

    auto arr = state.getChildWithName("arrangement");
    if (arr.isValid()) {
        juce::String brief = arr.getProperty("brief").toString();
        std::vector<SectionData> secs;
        for (int i = 0; i < arr.getNumChildren() && (int) secs.size() < kSections; ++i) {
            auto v = arr.getChild(i);
            SectionData s;
            std::strncpy(s.name, v["name"].toString().toRawUTF8(), sizeof s.name - 1);
            std::strncpy(s.feel, v["feel"].toString().toRawUTF8(), sizeof s.feel - 1);
            s.startBar = (float) (double) v["start"]; s.endBar = (float) (double) v["end"];
            s.amount = (float) (double) v.getProperty("amount", 1.0); s.movement = (float) (double) v.getProperty("movement", 1.0);
            s.leadUid = (uint64_t) v.getProperty("lead", "0").toString().getHexValue64();
            secs.push_back(s);
        }
        // The first instance to load fills the hub; the rest find it there.
        if (auto* h = hub.get(); h && h->briefSeq.load() == 0 && (brief.isNotEmpty() || !secs.empty())) {
            seqWrite(h->briefSeq, [&] {
                std::strncpy(h->brief, brief.toRawUTF8(), kBriefLen - 1); h->brief[kBriefLen - 1] = 0;
                h->numSections = (int32_t) secs.size();
                for (size_t i = 0; i < secs.size(); ++i) h->sections[i] = secs[i];
            });
        }
        {
            const juce::CriticalSection::ScopedLockType l(sharedLock);
            briefCache = brief; sectionsCache = secs;
        }
        SecSet set; set.n = (int) secs.size();
        for (size_t i = 0; i < secs.size(); ++i) set.s[i] = secs[i];
        const int next = 1 - secActive.load();
        secBuf[next] = set;
        secActive.store(next, std::memory_order_release);
    }
    state.removeChild(plan, nullptr);
    state.removeChild(arr, nullptr);
    apvts.replaceState(state);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ClearingProcessor(); }
