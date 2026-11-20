#pragma once
#include "Analyser.h"
#include "Biquad.h"
#include "Roles.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace clr {

struct TrackMeas {
    int id = 0;                       // caller's handle (slot index)
    std::string name;
    int role = RoleOther;
    float priority = 0.4f;            // 0..1
    bool locked = false;              // never change this track
    int layer = 0;                    // 0 = none; tracks sharing a layer id are one instrument
    std::array<float, kBands> db {};  // captured long-term spectrum, absolute dB
    float levelDb = kFloorDb;         // average level of the captured material, dB RMS
    std::array<float, kBands> peakDb {}; // loudest short-term level per band (all kFloorDb/0 if unknown)
    float activeSec = 0;              // seconds of audible material captured
};

struct Change {
    int track = 0;                    // TrackMeas::id
    BandType type = BandType::Bell;
    float freq = 1000, gainDb = 0, q = 1.4f;
    float confidence = 0;             // 0..1
    std::string reason;
    std::string cause;                // name of the part it makes room for, if any
    // Focus-following ("movement"): the band moves to gainDb only while the
    // leader plays, scaled by how far the leader is above threshDb.
    bool dynamic = false;
    int leader = -1;                  // TrackMeas::id of the instance whose level drives it
    float threshDb = kFloorDb;
    uint32_t sectionMask = 0;         // dips: bit k = active in section k; 0 = everywhere
    // Self-triggered: the band moves to gainDb only when this track itself builds up there.
    bool self = false;
};

struct PlannerSettings {
    float strength = 0.6f;            // 0..1, scales every change
    float maxCutDb = 3.0f;
    float maxBoostDb = 2.0f;
    float maxDipDb = 3.0f;            // focus-following dips (2 dB in orchestral style)
    int maxPerTrack = 3;              // static changes; plus at most one dip
    float minActiveSec = 8.0f;        // tracks with less captured audio are left alone
    int style = StyleBand;            // StyleOrchestral: gentler, wider, fewer changes
    bool dynamics = true;             // propose focus-following dips
    bool selfDynamics = true;         // propose self-triggered cuts (boom, harshness, sibilance)
    // Per section (index = section number): the track id that leads it, or -1 for the overall focus.
    std::vector<int> sectionLeads;
    std::vector<std::string> sectionNames;
};

struct PlanResult {
    std::vector<Change> changes;
    std::vector<std::string> notes;   // layer report and other observations, no action taken
};

// Conservative planner. Finds persistent overlap between a priority part and a
// lower-priority part and cuts the lower one a little. Layers (tracks sharing a
// layer id) are judged and corrected together. Parts far below the loudest one
// carry little weight. Overlap is evidence of possible masking, not proof; every
// change carries a confidence and a plain-language reason.
PlanResult planFull(const std::vector<TrackMeas>& tracks, const PlannerSettings& s);
inline std::vector<Change> plan(const std::vector<TrackMeas>& t, const PlannerSettings& s) { return planFull(t, s).changes; }

} // namespace clr
