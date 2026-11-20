#pragma once
// Arrangement and balance from per-bar timelines: section detection, post-fader level
// estimation from a master (stereo out) measurement, and per-section level moves.
#include "Analyser.h"
#include "Roles.h"
#include <array>
#include <string>
#include <vector>

namespace clr {

using Timeline = std::array<std::array<float, kCoarse>, kBars>; // dB per bar (0-based index = bar-1) per coarse band

struct TrackTimeline {
    int id = 0;
    std::string name;
    int role = RoleOther;
    float priority = 0.4f;
    int layer = 0;
    bool locked = false;
    Timeline db {};      // insert-level energy; kFloorDb where not heard
    float offsetDb = 0;  // estimated post-fader offset (from estimateLevels), added before balancing
    std::vector<float> lufs; // optional: approx. loudness per bar at the insert (kFloorDb where not heard)
};

struct SectionSpan {
    std::string name;
    int start = 1, end = 1;  // inclusive bars
    int leadId = -1;         // TrackTimeline::id leading this section, -1 = the overall focus
    std::vector<std::string> active; // parts playing (filled by detectSections / describe)
};

struct LevelFit {
    std::vector<float> offsetDb; // per track (same order as the input); kFloorDb = not found in the master
    float r2 = 0;                // fit quality, 0..1
    int bars = 0;                // bars used
    int excluded = 0;            // bar/band cells left out: far more at the stereo out than the tracks explain
};

// What the stereo out shows that the tracks don't: energy no CLEARING track accounts for, and low end
// lost where low parts overlap (phase). Uses the fitted offsets (TrackTimeline::offsetDb).
struct MixFinding {
    std::string kind;        // "unexplained" or "low-end cancellation"
    int band = 0;            // coarse band
    int start = 1, end = 1;  // bars, inclusive
    float db = 0;            // how far the stereo out is from what the tracks predict
    std::vector<std::string> parts; // cancellation: the overlapping low parts
    std::string text;
};
std::vector<MixFinding> findResiduals(const std::vector<TrackTimeline>& tracks, const Timeline& master);

struct BalanceMove {
    int track = 0;           // TrackTimeline::id
    int section = -1;        // index into the sections given; -1 = whole song
    float gainDb = 0;
    float confidence = 0;
    std::string reason;
};

// Total energy of a timeline row (all coarse bands), dB.
float barTotalDb(const Timeline& t, int barIndex);

// Fit master ~ sum(g_t * track_t) over bars and coarse bands (non-negative least squares on power).
// Each g_t (as dB) is that track's level change between its insert and the stereo out: fader, pan, buses.
LevelFit estimateLevels(const std::vector<TrackTimeline>& tracks, const Timeline& master);

// Split the song where the set of playing parts changes or the level jumps; spans of 4+ bars.
std::vector<SectionSpan> detectSections(const std::vector<TrackTimeline>& tracks, int bars);

// Per section: the leader's share of its main band, and the parts sharing it (for reports).
struct SectionShare { std::string leader; std::string band; float share = 0; std::vector<std::pair<std::string, float>> others; bool active = false; };
std::vector<SectionShare> describeSections(const std::vector<TrackTimeline>& tracks, const std::vector<SectionSpan>& sections);

// Small, explained level moves so the leader of each section holds its band.
std::vector<BalanceMove> planBalance(const std::vector<TrackTimeline>& tracks, const std::vector<SectionSpan>& sections,
                                     int style, std::vector<std::string>* notes = nullptr);

} // namespace clr
