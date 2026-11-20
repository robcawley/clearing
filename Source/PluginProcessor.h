#pragma once
#include "Core/Analyser.h"
#include "Core/Biquad.h"
#include "Core/Roles.h"
#include "Hub.h"
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

class ClearingProcessor : public juce::AudioProcessor, private juce::Thread {
public:
    ClearingProcessor();
    ~ClearingProcessor() override;

    const juce::String getName() const override { return "CLEARING"; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == l.getMainInputChannelSet()
               && (l.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
                   || l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
    }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // Shared mix window support (message thread).
    clr::Hub& getHub() { return hub; }
    int getSlot() const { return slot; }
    uint64_t getUid() const { return uid.load(); }
    juce::String getTrackName() const;
    void setTrackName(const juce::String&);
    void updateTrackProperties(const TrackProperties&) override;
    std::array<clr::EqBandData, clr::kEqBands> currentPlan() const;
    clr::TrimPlan currentTrim() const;
    clr::HarmPlan currentHarm() const { const juce::SpinLock::ScopedLockType l(harmLock); return harmShared; }
    float matchedCompareDb() const { return matchDb.load(); }
    bool isMaster() const { return *apvts.getRawParameterValue("mode") > 0.5f; }
    int groupId() const { return (int) *apvts.getRawParameterValue("group"); }
    int styleId() const { return (int) *apvts.getRawParameterValue("style"); }
    int currentSection() const { return curSection.load(); }
    // Diagnostics for tests: a self band's detector (fast, usual) and the part's (level, usual).
    std::array<float, 4> selfDebug(int i) const { return { detFast[i], detSlow[i], envDb, envSlow }; }

    // One analysis step on the calling thread; the background thread does this too.
    void pump();
    void publish();
    // Apply any pending controls/plan from the hub now (tests and tools call this too).
    void pollHub(bool applyUiControls = true);
    // Background housekeeping: leader lookup and brief/section changes.
    void syncShared();

    juce::AudioProcessorValueTreeState apvts;

private:
    void run() override;
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void applyPlan(const clr::EqBandData* b, const clr::TrimPlan& t, const clr::HarmPlan& hp);
    void mirrorPlan();
    void updateSection(int n);
    void updateFilters(int numCh, int n);
    void setParam(const char* id, float plainValue);

    clr::Hub hub;
    int slot = -1;
    std::atomic<uint64_t> uid { 0 };

    // Audio thread -> analysis thread
    std::vector<float> fifoBuf;
    juce::AbstractFifo fifo { 1 << 16 };
    juce::CriticalSection analyseLock;
    clr::Analyser analyser;
    std::vector<float> drain;
    std::vector<int16_t> fifoBar, drainBar;
    // Master: the stereo out, for mix health (both channels).
    std::vector<float> stL, stR, drainL, drainR;
    std::vector<int16_t> stBar, drainStBar;
    juce::AbstractFifo stFifo { 1 << 17 };
    clr::HealthAnalyser health;
    bool healthOn = false;
    uint32_t seenEpoch = 0;
    std::atomic<uint32_t> seenCtrl { 0 }, seenPlan { 0 };
    uint32_t seenBrief = 0xffffffffu;
    juce::CriticalSection syncLock; // syncShared runs from the background thread and from tests

    // Controls received but not yet set on the parameters (message thread does that).
    struct Pending { int role = 0, locked = 0, layer = 0; float prio = 0.4f, amount = 1, movement = 1; };
    juce::SpinLock pendLock;
    Pending pend;
    std::atomic<uint32_t> pendGen { 0 }, pendDone { 0 };

    // Accepted EQ plan (written off the audio thread, read on it)
    struct Band {
        std::atomic<float> freq { 1000.f }, gain { 0.f }, q { 1.f }, thresh { -120.f };
        std::atomic<int> type { 0 }, on { 0 }, dynamic { 0 }, leaderSlot { -1 }, self { 0 }, ruleId { 0 };
        std::atomic<uint32_t> sectionMask { 0 };
        std::atomic<uint64_t> leaderUid { 0 };
    } bands[clr::kEqBands];
    // Self-triggered bands: a band-pass detector on this track's own input.
    clr::Biquad det[clr::kEqBands];
    float detFreq[clr::kEqBands] {}, detFast[clr::kEqBands] {}, detSlow[clr::kEqBands] {}, detWarm[clr::kEqBands] {};
    float envSlow = -120.0f, envWarm = 0;
    int curSec = -1;
    // Bar and tempo from the playhead (audio thread -> analysis thread).
    std::atomic<int> curBar { 0 };
    std::atomic<float> curBpm { 0 };
    std::atomic<int> curNum { 4 }, curDen { 4 };
    // Planned trims (written off the audio thread).
    std::atomic<float> trimBase { 0 };
    std::atomic<float> trimSec[clr::kSections] {};
    float gainDbSmooth = 0, gainLin = 1.0f;
    // Harmonics (surgical saturation): plan written off the audio thread, copied in under a try-lock.
    mutable juce::SpinLock harmLock;
    clr::HarmPlan harmShared, harmLocal;
    std::atomic<uint32_t> harmGen { 0 };
    uint32_t harmSeen = 0xffffffffu;
    clr::HarmonicStage harmStage;
    // Level-matched compare: long-term loudness before and after our processing.
    double eInS = 0, eOutS = 0;
    float onSamples = 0;
    std::atomic<float> matchDb { 0 };
    std::atomic<float> inPeak { 0 }; // highest |sample| at the insert while listening (gain staging)
    clr::Biquad filt[clr::kEqBands][2];
    float smoothGain[clr::kEqBands] {}, smoothAct[clr::kEqBands] {}, appliedGain[clr::kEqBands] {},
        appliedFreq[clr::kEqBands] {}, appliedQ[clr::kEqBands] {};
    int appliedType[clr::kEqBands] {};
    double fs = 48000;
    float envDb = -120.0f;

    // Sections, double-buffered for the audio thread.
    struct SecSet { int n = 0; clr::SectionData s[clr::kSections]; };
    SecSet secBuf[2];
    std::atomic<int> secActive { 0 };
    float secAmt = 1, secMov = 1;
    std::atomic<int> curSection { -1 };

    // Brief/sections as last seen (saved with the project).
    juce::CriticalSection sharedLock;
    juce::String briefCache;
    std::vector<clr::SectionData> sectionsCache;

    mutable juce::SpinLock nameLock;
    juce::String userName, hostName;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    std::atomic<float>* pRole; std::atomic<float>* pPriority; std::atomic<float>* pLocked;
    std::atomic<float>* pBypass; std::atomic<float>* pStyle; std::atomic<float>* pGroup;
    std::atomic<float>* pAmount; std::atomic<float>* pMovement; std::atomic<float>* pLayer;
    std::atomic<float>* pTrim; std::atomic<float>* pMode;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClearingProcessor)
};
