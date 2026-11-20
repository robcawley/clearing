#pragma once
// Cross-instance hub: a POSIX shared-memory segment that every CLEARING
// instance maps, so instances can see each other whether or not Logic hosts
// them in one process. The audio thread touches only lock-free atomics in it
// (its own envelope, and a leader's envelope); everything else is done on
// background and message threads.
#include "Core/Analyser.h"
#include "Core/Harmonics.h"
#include "Core/Health.h"
#include <atomic>
#include <cstdint>
#include <cstring>

namespace clr {

constexpr int kSlots = 32;
constexpr int kEqBands = 6;
constexpr int kSections = 16;
constexpr int kBriefLen = 3000;
constexpr uint32_t kHubMagic = 0x434c5238; // 'CLR8'

struct EqBandData {
    float freq = 1000, gainDb = 0, q = 1.0f;
    int32_t type = 0;       // BandType
    int32_t enabled = 0;
    int32_t dynamic = 0;    // 1 = moves with the leader (focus-following dip)
    float threshDb = -120;  // leader level where the dip starts
    uint32_t sectionMask = 0; // bit k = active in section k; 0 = everywhere
    uint64_t leaderUid = 0; // persistent id of the leader instance
    int32_t self = 0;       // 1 = triggered by this track's own build-up in the band
    int32_t ruleId = 0;     // > 0: placed by hand (add_dip), removable as a rule
};

struct SectionData {
    char name[24] {};
    float startBar = 1, endBar = 1;   // inclusive bar numbers
    float amount = 1, movement = 1;   // multipliers for this section
    char feel[96] {};
    uint64_t leadUid = 0;             // the track that leads this section (0 = the overall focus)
};

// Planned level moves (balance): a whole-song trim plus one per section, in dB.
struct TrimPlan {
    float baseDb = 0;
    float sectionDb[kSections] {};
};

struct Slot {
    std::atomic<uint64_t> owner { 0 };       // instance uid, 0 = free
    std::atomic<uint64_t> heartbeatNs { 0 };

    // Written by the owner's audio thread every block (relaxed).
    std::atomic<float> envDb { -120.0f };
    std::atomic<uint64_t> envNs { 0 };

    // Measurement, written only by the owner (seqlock).
    std::atomic<uint32_t> measSeq { 0 };
    char name[24] {};
    int32_t role = 0, group = 1, locked = 0, style = 0, hostNamed = 0, layer = 0, section = -1;
    float priority = 0.4f, amount = 1.0f, movement = 1.0f;
    float live[kBands] {};
    float cap[kBands] {};
    float capPeak[kBands] {};
    float activeSec = 0, rmsDb = -120.0f, capRmsDb = -120.0f;
    float capPeakDb = -120.0f;   // highest sample peak at the insert during the last listen (dBFS), for gain staging
    int32_t mode = 0;         // 0 = track, 1 = master (stereo out: measures only)
    float trimParamDb = 0;    // the Trim parameter (user/automation)
    float bpm = 0;            // from the playhead
    int32_t meterNum = 4, meterDen = 4;
    float matchDb = 0;        // processed minus unprocessed loudness of this track's EQ + harmonics (for matched compare)

    // Timeline while listening, by bar (index = bar - 1); written by the owner (seqlock).
    std::atomic<uint32_t> tlSeq { 0 };
    int32_t tlBars = 0;
    float tl[kBars][kCoarse] {};
    float tlLufs[kBars] {};

    // Mix health, master instances only (seqlock): per bar and health band.
    std::atomic<uint32_t> hSeq { 0 };
    int32_t hBars = 0;
    float hCorr[kBars][kHBands] {};   // correlation -1..1
    float hMono[kBars][kHBands] {};   // dB kept in a mono fold-down (0 = all)
    float hEn[kBars][kHBands] {};     // band energy, dB
    float hTp[kBars] {};              // true peak per bar, dBTP
    float hLufsI = -120, hLufsSmax = -120, hTpMax = -120, hSpMax = -120;

    // Controls, written by any window or the MCP server, applied by the owner (seqlock).
    // Layer / amount / movement: -1 = leave unchanged.
    std::atomic<uint32_t> ctrlSeq { 0 };
    char ctrlName[24] {};
    int32_t ctrlRole = 0, ctrlLocked = 0, ctrlLayer = -1;
    float ctrlPriority = 0.4f, ctrlAmount = -1, ctrlMovement = -1;

    // Accepted plan, written by whoever pressed Apply, applied by the owner (seqlock).
    std::atomic<uint32_t> planSeq { 0 };
    EqBandData plan[kEqBands];
    TrimPlan trim;
    HarmPlan harm;
};

struct HubData {
    std::atomic<uint32_t> magic { 0 };
    std::atomic<int32_t> listening { 0 };    // 1 while capturing
    std::atomic<uint32_t> listenEpoch { 0 }; // bumped when a new capture starts
    std::atomic<int32_t> bypassAll { 0 };    // compare: hear everything unprocessed
    std::atomic<int32_t> compareMatched { 1 }; // compare at the processed loudness (level-matched A/B)
    // Arrangement brief and sections (seqlock). Saved in every instance's state.
    std::atomic<uint32_t> briefSeq { 0 };
    char brief[kBriefLen] {};
    int32_t numSections = 0;
    SectionData sections[kSections];
    Slot slots[kSlots];

    // The LLM side (clearing-mcp): who's connected, and a short log of what it did, for the window.
    std::atomic<uint64_t> llmBeatNs { 0 };     // steady-clock ns; refreshed every second while connected
    std::atomic<uint64_t> llmLastCallNs { 0 }; // last tool call
    std::atomic<uint32_t> llmSeq { 0 };        // seqlock for the fields below
    char llmClient[48] {};                     // e.g. "Claude" (from the MCP client's name)
    char llmVersion[16] {};
    static constexpr int kLog = 12;
    uint32_t logCount = 0;                     // total entries ever written; newest at (logCount-1) % kLog
    struct LogEntry { int64_t unixTime; char text[112]; } log[kLog] {};
};

// Generic seqlock helpers (plain-data payload copied between enter/leave).
template <class F> inline void seqWrite(std::atomic<uint32_t>& seq, F&& f)
{
    seq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
    f();
    seq.fetch_add(1, std::memory_order_release); // even: stable
}
template <class F> inline bool seqRead(const std::atomic<uint32_t>& seq, F&& f)
{
    for (int i = 0; i < 8; ++i) {
        const uint32_t a = seq.load(std::memory_order_acquire);
        if (a & 1) continue;
        f();
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == a) return true;
    }
    return false;
}

class Hub {
public:
    // Maps (creating if needed) the shared segment. Returns false if the host
    // sandbox refuses it; callers then run stand-alone.
    bool open();
    void close();
    bool ok() const { return data != nullptr; }
    HubData* get() const { return data; }
    static uint64_t nowNs();

    // Claim a free (or abandoned) slot; -1 if none.
    int claim(uint64_t id);
    void release(int slot, uint64_t id);
    // The live slot owned by uid; -1 if none.
    int find(uint64_t uid) const;

    static constexpr uint64_t kStaleNs = 4'000'000'000ull;
    bool live(int slot) const;
    const char* name() const { return segName; }
    void useName(const char* n) { std::strncpy(segName, n, sizeof segName - 1); }

private:
    HubData* data = nullptr;
    int fd = -1;
    char segName[64] = "/clearing_hub_v8";
};

} // namespace clr
