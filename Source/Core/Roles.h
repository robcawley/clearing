#pragma once

namespace clr {

// Indices are permanent (they are saved in projects): append only.
enum Role : int {
    RoleOther = 0, RoleKick, RoleBass, RoleSnare, RoleLeadVocal, RoleBackVocal,
    RoleAcousticGtr, RoleElectricGtr, RolePiano, RoleSynth, RoleOverheads,
    // Orchestral
    RoleSolo, RoleViolins, RoleViolas, RoleCellos, RoleDoubleBasses, RoleWoodwinds,
    RoleTrumpets, RoleHorns, RoleLowBrass, RoleHarp, RoleTimpani, RoleOrchPerc, RoleChoir,
    RoleCount
};

enum MixStyle : int { StyleBand = 0, StyleOrchestral = 1 };

inline const char* roleName(int r)
{
    static const char* n[] = { "Other", "Kick", "Bass", "Snare", "Lead Vocal", "Backing Vocal",
                               "Acoustic Gtr", "Electric Gtr", "Piano / Keys", "Synth", "Overheads",
                               "Solo Instrument", "Violins", "Violas", "Cellos", "Double Basses", "Woodwinds",
                               "Trumpets", "Horns", "Low Brass", "Harp", "Timpani", "Orch. Percussion", "Choir" };
    return (r >= 0 && r < RoleCount) ? n[r] : "Other";
}

inline bool roleIsOrchestral(int r) { return r >= RoleSolo && r < RoleCount; }

// Starting priority 0..1: how much this part needs to be heard.
inline float roleDefaultPriority(int r)
{
    static const float p[] = { 0.40f, 0.85f, 0.65f, 0.65f, 0.95f, 0.50f, 0.45f, 0.45f, 0.50f, 0.45f, 0.40f,
                               0.95f, 0.70f, 0.50f, 0.62f, 0.55f, 0.62f,
                               0.60f, 0.62f, 0.55f, 0.45f, 0.60f, 0.45f, 0.70f };
    return (r >= 0 && r < RoleCount) ? p[r] : 0.4f;
}

} // namespace clr
