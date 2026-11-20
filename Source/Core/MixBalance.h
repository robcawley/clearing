#pragma once
// Mix balance: fader-level moves so the parts sit in a balance that suits the style.
//
// Each part's loudness at the stereo out is measured per section (its per-bar loudness at the
// insert plus its estimated fader offset). Each role group has a target loudness relative to the
// others for the style ("the beat 1 dB under the vocal, guitars 3 dB under the beat..."), the lead's
// place is set separately (on top / blended / inside the wall), and the moves that bring the parts
// closest to that ladder are found with the overall level kept where it is. Moves are whole-song
// where a part is consistently off, with section rides on top where it's off only in places.
#include "Balance.h"
#include <string>
#include <vector>

namespace clr {

enum class LeadPlace { OnTop = 0, Blended = 1, Inside = 2 };

enum MixGroup : int { GroupLead = 0, GroupDrums, GroupBass, GroupGuitars, GroupKeys, GroupSynths,
                      GroupBacking, GroupStrings, GroupBrass, GroupWinds, GroupPerc, GroupOther, GroupCount };
const char* mixGroupName(int g);
int mixGroupOf(int role);

struct BalanceStyle {
    std::string name;            // "industrial", "rock", "pop", "electronic", "hip hop", "orchestral", "ambient"
    float target[GroupCount] {}; // dB relative to each other (only differences matter)
};
// Known styles; unknown names fall back to "rock".
BalanceStyle balanceStyle(const std::string& name);
std::vector<std::string> balanceStyleNames();

struct PartLoudness {           // one part in one section
    int track = 0;              // TrackTimeline::id (the layer's first member for a layer)
    std::string name;
    int group = GroupOther;
    float lufs = kFloorDb;      // loudness at the stereo out while it plays (approx.)
    float target = 0;           // where the style puts it, on the same scale as lufs
    float move = 0;             // proposed level change for this section (dB)
    bool locked = false, lead = false;
};
struct SectionBalance {
    std::string name;
    int index = -1;             // saved section index, -1 for the whole song
    int start = 1, end = 1;
    std::vector<PartLoudness> parts;
    float spread = 0;           // RMS distance from the targets before the moves (dB)
};
struct LevelMove {
    int track = 0;              // TrackTimeline::id of the first member
    std::vector<int> members;   // every track the move applies to (a layer moves together)
    std::string name;
    int section = -1;           // -1 = whole song (Trim base), else saved section index (a ride on top)
    float gainDb = 0;
    float confidence = 0;
    std::string reason;
};
struct MixBalance {
    std::string style;
    LeadPlace lead = LeadPlace::Blended;
    std::vector<SectionBalance> sections;
    std::vector<LevelMove> moves;
    std::vector<std::string> notes;
    float spreadBefore = 0, spreadAfter = 0; // RMS distance from the targets across sections (dB)
};

// tracks: timelines with offsetDb (post-fader estimate) and per-bar loudness (lufs); sections: the
// saved sections (empty = treat the song as one section). Locked tracks are measured but never moved.
MixBalance planMixBalance(const std::vector<TrackTimeline>& tracks, const std::vector<SectionSpan>& sections,
                          const std::string& style, LeadPlace lead, float maxMoveDb = 6.0f);

} // namespace clr
