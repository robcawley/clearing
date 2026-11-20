// Stands in for Logic: hosts five real CLEARING instances in their own process, names them the
// way Logic does (track properties), and plays synthetic parts through them at ~2x real time.
// Prints each instance's role, priority and applied EQ at the end.
#include "PluginProcessor.h"
#include "Hub.h"
#include <cstdio>
#include <random>
#include <sys/mman.h>

using namespace clr;
static const double FS = 48000.0, PI = 3.14159265358979323846;

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

// Logic's transport: 120 bpm, 4/4, a 24-bar arrangement that loops; it restarts from bar 1
// whenever the MCP starts listening (as if the user pressed play from the top).
struct FakePlayHead : juce::AudioPlayHead {
    int64_t sample = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm(120.0); p.setTimeSignature(TimeSignature { 4, 4 }); p.setIsPlaying(true);
        p.setTimeInSamples(sample); p.setPpqPosition((double) sample / FS * 2.0);
        return p;
    }
};
// Which parts play in each 8-bar block: all / kick+bass+vox / kick+bass+gtr+rhodes.
static bool plays(int part, int bar)
{
    const int blk = ((bar - 1) % 24) / 8;
    if (blk == 0) return true;
    if (blk == 1) return part <= 2;
    return part != 2;
}

int main(int argc, char** argv)
{
    const double runSec = argc > 1 ? atof(argv[1]) : 40.0;
    if (const char* e = getenv("CLEARING_HUB")) shm_unlink(e);
    juce::ScopedJuceInitialiser_GUI init;
    struct P { const char* name; std::vector<float> audio; };
    std::vector<P> parts = { { "Kick", kick(10) }, { "Bass DI", harmonic(10, 65, 1.2, 0.35, 0, 1) },
                             { "Lead Vox", harmonic(10, 220, 1.0, 0.04, -12, 2) }, { "Gtr", harmonic(10, 196, 0.9, 0.04, 0, 3) },
                             { "Rhodes", harmonic(10, 262, 1.1, 0.05, -6, 4) } };
    std::vector<std::unique_ptr<ClearingProcessor>> procs;
    for (auto& p : parts) {
        auto pr = std::make_unique<ClearingProcessor>();
        pr->prepareToPlay(FS, 480);
        juce::AudioProcessor::TrackProperties tp; tp.name = p.name;
        pr->updateTrackProperties(tp);
        procs.push_back(std::move(pr));
    }
    // A wide pad that no CLEARING instance measures (bars 17-24 of the loop): the stereo out should notice it.
    // (band-limited noise around 1.7 kHz, independent in each channel)
    std::vector<float> padL((size_t) (10 * FS)), padR(padL.size());
    {
        std::mt19937 rng(7); std::normal_distribution<float> nd(0.0f, 1.0f);
        Biquad f[2][2];
        for (auto& ch : f) for (auto& q : ch) q.set(makeCoeffs(BandType::BandPass, FS, 1700.0, 0, 1.4));
        for (size_t i = 0; i < padL.size(); ++i) {
            padL[i] = 0.2f * f[0][1].process(f[0][0].process(nd(rng)));
            padR[i] = 0.2f * f[1][1].process(f[1][0].process(nd(rng)));
        }
    }
    // The stereo out: one more instance in Master mode, fed the sum through "faders".
    const float fader[] = { 0.0f, -6.0f, 3.0f, -3.0f, 0.0f };
    auto master = std::make_unique<ClearingProcessor>();
    master->prepareToPlay(FS, 480);
    { juce::AudioProcessor::TrackProperties tp; tp.name = "Stereo Out"; master->updateTrackProperties(tp); }
    master->apvts.getParameter("mode")->setValueNotifyingHost(1.0f);
    FakePlayHead ph;
    for (auto& pr : procs) pr->setPlayHead(&ph);
    master->setPlayHead(&ph);
    Hub hub; hub.open();
    uint32_t epoch = hub.get() ? hub.get()->listenEpoch.load() : 0;
    std::printf("session up: %d instances\n", (int) procs.size()); fflush(stdout);
    juce::AudioBuffer<float> b(2, 480), mix(2, 480); juce::MidiBuffer m;
    size_t pos = 0;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const juce::File stopFile(argc > 2 ? juce::String(argv[2]) : juce::String()); // optional: stop early when this file appears
    while (juce::Time::getMillisecondCounterHiRes() - t0 < runSec * 1000.0) {
        if (argc > 2 && stopFile.existsAsFile()) break;
        if (hub.get() && hub.get()->listenEpoch.load() != epoch) { epoch = hub.get()->listenEpoch.load(); ph.sample = 0; }
        for (int rep = 0; rep < 2; ++rep) {
            const int bar = (int) (ph.sample / (int64_t) (FS * 2.0)) + 1;
            mix.clear();
            for (size_t k = 0; k < procs.size(); ++k) {
                const bool on = plays((int) k, bar);
                for (int i = 0; i < 480; ++i) { const float v = on ? parts[k].audio[(pos + (size_t) i) % parts[k].audio.size()] : 0.0f; b.setSample(0, i, v); b.setSample(1, i, v); }
                procs[k]->processBlock(b, m);
                const float g = std::pow(10.0f, fader[k] / 20.0f);
                for (int ch = 0; ch < 2; ++ch) mix.addFrom(ch, 0, b, ch, 0, 480, g);
            }
            if (((bar - 1) % 24) >= 16)
                for (int i = 0; i < 480; ++i) {
                    mix.addSample(0, i, padL[(pos + (size_t) i) % padL.size()]);
                    mix.addSample(1, i, padR[(pos + (size_t) i) % padR.size()]);
                }
            master->processBlock(mix, m);
            pos += 480; ph.sample += 480;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    for (auto& p : procs) {
        std::printf("FINAL %s | role=%s prio=%.2f locked=%d layer=%d amount=%.2f movement=%.2f |", p->getTrackName().toRawUTF8(),
                    roleName((int) *p->apvts.getRawParameterValue("role")), (double) *p->apvts.getRawParameterValue("priority"),
                    *p->apvts.getRawParameterValue("locked") > 0.5f ? 1 : 0, (int) *p->apvts.getRawParameterValue("layer"),
                    (double) *p->apvts.getRawParameterValue("amount"), (double) *p->apvts.getRawParameterValue("movement"));
        for (auto& e : p->currentPlan()) if (e.enabled) std::printf(" %.0fHz %+.1fdB%s", e.freq, e.gainDb, e.dynamic ? "(dip)" : "");
        const auto tr = p->currentTrim();
        std::printf(" | trim base %+.1f", tr.baseDb);
        { const auto hp = p->currentHarm(); if (hp.bassOn > 0) std::printf(" | harm bass %.0f-%.0f", hp.bassLo, hp.bassHi); if (hp.presOn > 0) std::printf(" | fill %.0f", hp.presFreq); }
        for (int k = 0; k < kSections; ++k) if (tr.sectionDb[k] != 0.0f) std::printf(" s%d %+.1f", k, tr.sectionDb[k]);
        std::printf("\n");
    }
    std::printf("MASTER mode=%d\n", master->isMaster() ? 1 : 0);
    procs.clear(); master.reset();
    if (const char* e = getenv("CLEARING_HUB")) shm_unlink(e);
    return 0;
}
