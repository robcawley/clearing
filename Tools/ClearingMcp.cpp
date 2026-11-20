// clearing-mcp: a Model Context Protocol server (JSON-RPC over stdio) that lets an
// LLM inspect and drive the CLEARING instances in a running Logic session.
// It talks only to the shared hub; it never touches Logic or the audio thread.
#include "Core/Balance.h"
#include "Core/MixBalance.h"
#include "Core/Planner.h"
#include "Hub.h"
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <BinaryData.h>
#include <memory>
#include <cmath>
#include <cstring>
#include <atomic>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <mach-o/dyld.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace clr;
using juce::DynamicObject;
using juce::String;
using juce::var;

namespace {

var obj() { return var(new DynamicObject()); }
var& put(var& o, const juce::Identifier& k, const var& v) { o.getDynamicObject()->setProperty(k, v); return o; }
double rnd(double x, int dp) { const double m = std::pow(10.0, dp); return std::round(x * m) / m; }

String hz(float f)
{
    return f >= 1000.0f ? String(f / 1000.0f, 1) + " kHz" : String((int) std::round(f)) + " Hz";
}

int roleFromVar(const var& v)
{
    if (v.isInt() || v.isDouble()) return juce::jlimit(0, RoleCount - 1, (int) v);
    const String s = v.toString().trim();
    for (int r = 0; r < RoleCount; ++r) if (s.equalsIgnoreCase(roleName(r))) return r;
    // forgiving matches ("vocal", "guitar", "keys", "strings"...)
    const String l = s.toLowerCase();
    struct A { const char* k; int r; } al[] = {
        { "lead vox", RoleLeadVocal }, { "vocal", RoleLeadVocal }, { "vox", RoleLeadVocal }, { "backing", RoleBackVocal },
        { "bvs", RoleBackVocal }, { "acoustic", RoleAcousticGtr }, { "electric", RoleElectricGtr }, { "guitar", RoleElectricGtr },
        { "gtr", RoleElectricGtr }, { "piano", RolePiano }, { "keys", RolePiano }, { "organ", RolePiano }, { "synth", RoleSynth },
        { "pad", RoleSynth }, { "overhead", RoleOverheads }, { "cymbal", RoleOverheads }, { "kick", RoleKick }, { "bass", RoleBass },
        { "snare", RoleSnare }, { "solo", RoleSolo }, { "violin", RoleViolins }, { "viola", RoleViolas }, { "cello", RoleCellos },
        { "contrabass", RoleDoubleBasses }, { "double bass", RoleDoubleBasses }, { "wood", RoleWoodwinds }, { "flute", RoleWoodwinds },
        { "clarinet", RoleWoodwinds }, { "oboe", RoleWoodwinds }, { "bassoon", RoleWoodwinds }, { "trumpet", RoleTrumpets },
        { "horn", RoleHorns }, { "trombone", RoleLowBrass }, { "tuba", RoleLowBrass }, { "low brass", RoleLowBrass },
        { "harp", RoleHarp }, { "timp", RoleTimpani }, { "perc", RoleOrchPerc }, { "choir", RoleChoir }, { "other", RoleOther } };
    // longest key first so "double bass" beats "bass"
    int best = -1; int bestLen = 0;
    for (auto& a : al) { const int n = (int) strlen(a.k); if (l.contains(a.k) && n > bestLen) { best = a.r; bestLen = n; } }
    return best;
}

struct TrackSnap {
    int slot = -1;
    String name;
    uint64_t uid = 0;
    int role = 0, group = 1, style = 0, layer = 0, section = -1;
    bool locked = false, hostNamed = false;
    float priority = 0.4f, activeSec = 0, rmsDb = -120, capRmsDb = -120, capPeakDb = -120, amount = 1, movement = 1;
    std::array<float, kBands> cap {}, live {}, capPeak {};
    std::array<EqBandData, kEqBands> plan {};
    // 0.5
    int mode = 0; float trimParam = 0, bpm = 0; int meterNum = 4, meterDen = 4;
    TrimPlan trim {};
    HarmPlan harm {};
    float matchDb = 0;
    struct Health { int bars = 0; float corr[kBars][kHBands], mono[kBars][kHBands], en[kBars][kHBands], tp[kBars]; float lufsI = -120, lufsS = -120, tpMax = -120, spMax = -120; };
    std::shared_ptr<Health> health; // master only
    int tlBars = 0;
    std::shared_ptr<Timeline> tl;           // dB per bar per coarse band
    std::array<float, kBars> tlLufs {};
    bool master() const { return mode == 1; }
};

const char* kInstructions = R"(CLEARING is an automatic mix EQ plug-in for Logic Pro. One instance sits on each track that takes part; this server sees those instances (not Logic itself). Work like a mix engineer sitting with the composer: ask before you assume, explain in musical terms, keep moves small.
Everything you do shows in the plug-in window's Claude panel (activity list), so the user can follow along in Logic. Every change is undoable (undo), and compare lets the user hear before and after.
Workflow:
0. status: what's live and the next step.
1. list_tracks and get_brief. If no tracks appear, ask the user to insert CLEARING on each track (after its tone-shaping effects) and keep Logic open. If a brief already exists, confirm it's still right instead of re-asking.
2. Ask about the parts, in small batches (2-4 questions at a time, guesses pre-filled from track names and spectra): what each part is, which is the focus, which tracks are layered into one sound (give layered tracks the same layer number), anything that must not be touched (lock).
3. Ask about the arrangement: the sections with their bar ranges (intro, verse, build, chorus, bridge, outro...), and for each section how it should feel and what impact it should have (intimate, building, huge, sparse, tense, release...), and which part leads it. Record the answers with set_brief, and set_sections with amount and movement per section:
   - intimate / exposed lead / sparse: amount 0.8-1.0, movement 1.0 (supporting parts ease back around the lead).
   - building: movement 0.8-1.0, amount around 0.8-1.0.
   - huge / full / climax / wall of sound: amount 0.5-0.7 and movement 0.3-0.6, so the whole stack blooms and nothing ducks.
   - tense / dense: amount 1.0, movement 0.6-0.8.
   These are starting points; explain your choice.
   Also ask who leads each section. If a different part should take over somewhere (a bass synth at a trip hop drop, a horn feature, a beat), set_section_lead for that section: get_suggestions then makes everything else ease back in that part's range there, only while it plays (a bass lead owns the low end). The overall focus keeps leading the other sections.
4. set_track for roles, priorities, layers, locks. Then listen start; have the user play the busiest material (or a section where every part plays) for 30-60 s; listen stop. Every part needs 8 s or more; use listen status, and if some parts weren't heard, do another pass over a section where they play.
5. get_suggestions (style orchestral for orchestral or orchestral-aspiring synth work). It also proposes SELF bands: a part tames its own boom, harshness or sibilance only when it builds up in bursts. Read the notes too (layer report: near-silent layers, near-identical doubled layers, parts not heard). Explain each suggestion in musical terms, give your opinion, and let the user choose; apply the chosen ids.
   For a precise request the planner didn't cover ("the pads should duck under the bass below 120 Hz in the drop"), use add_dip; remove_dip takes it out again.
6. Suggest comparing (compare on/off) and adjusting set_amount / set_movement. Tell the user that Amount and Movement are automatable per track in Logic, and that sections already shape them by bar.
Arrangement and balance (0.5):
- Ask the user to put one CLEARING on the stereo out and set its Mode to Master. It changes nothing; it measures the finished mix.
- Listen from the top of the song (the playhead must move: data is kept per bar). For the cleanest level read, turn compare on with matched false while listening (CLEARING bypassed at original levels) and off afterwards. Then analyse_arrangement: it gives tempo, sections (detected if none are saved), who plays where, and the real post-fader level of each track. If sections were detected, show render_view arrangement, agree names, bar ranges and feel with the user, and save them with set_sections.
- Use render_view freely when it helps the user see the problem: masking for a section where a part is buried, spectrum for where the mix gets heavy or thin. Read the pictures yourself too.
- get_suggestions returns EQ and 'balance' (level) moves. Most of a mix is level: prefer a 1 dB level move over a narrow EQ cut when a part is simply too loud or quiet. Level moves are small (+2 / -2 dB) and say why. set_trim places one by hand.
Balancing levels (0.8):
- When the user wants a better balance: ask the style (industrial, rock, pop, electronic, hip hop, orchestral, ambient) and where the lead should sit (on top / blended / inside the wall), make sure CLEARING is on every track that plays and on the Stereo Out (Master out), listen with compare on and matched false through the whole song, then balance_mix. Show render_view balance. Explain the moves musically (whole-song moves first, then rides), apply the agreed ids, and confirm with another listen.
- Report balance_mix's gain_staging notes in plain terms.
Mix health and harmonics (0.6):
- After a listen with the stereo-out instance, call mix_health and report its warnings and findings in plain terms (wide low end, mono cancellation, parts cancelling in the low end, energy nobody measures, peaks near 0 dBTP). render_view stereo shows it.
- get_suggestions may return 'harmonics': harmonic bass (a low part that won't translate on small speakers) and presence fill (a buried lead). They add harmonics, not level. Explain them, apply by id; set_harmonics places or switches them off by hand. Suggest the user checks harmonic bass on small speakers or earbuds, and that the sub can come down 1-2 dB once it's in.
- compare is level-matched by default: tell the user they're judging tone at equal loudness.
Without a stereo-out instance, measurements are taken before the channel fader, pan and buses; re-listen after big balance changes. Moves are deliberately small (cuts up to 3 dB, 2 dB in orchestral style; dips up to 3 dB, 2 dB orchestral). Up to 32 tracks, six bands each.)";

const char* kTools = R"JSON([
 {"name":"list_tracks","description":"List the tracks that have CLEARING on them: name (from Logic where available), role, priority, layer, lock, amount, movement, level, seconds of audio captured, a plain-language spectrum summary, the EQ currently applied (including focus-following dips), and the current section at the playhead.","inputSchema":{"type":"object","properties":{"group":{"type":"integer","description":"Mix group 1-8 (default 1). 0 lists every group."}}}},
 {"name":"list_roles","description":"The roles a track can have, with their default priorities.","inputSchema":{"type":"object","properties":{}}},
 {"name":"set_track","description":"Set a track's role, priority (0-1, how much it must be heard), layer (0 = none, 1-8: tracks with the same layer are one instrument and are corrected together), lock (never change its tone), amount (0-1, how much of its EQ is applied), movement (0-1, how deep its focus-following dips go) and/or display name. Role accepts a role name (see list_roles) or a common word like 'vocal', 'cello', 'pad'. Setting role without priority applies that role's default priority.","inputSchema":{"type":"object","properties":{"slot":{"type":"integer"},"role":{"type":["string","integer"]},"priority":{"type":"number","minimum":0,"maximum":1},"layer":{"type":"integer","minimum":0,"maximum":8},"locked":{"type":"boolean"},"amount":{"type":"number","minimum":0,"maximum":1},"movement":{"type":"number","minimum":0,"maximum":1},"name":{"type":"string"}},"required":["slot"]}},
 {"name":"get_brief","description":"The saved arrangement brief and sections (stored in the Logic project with the plug-in).","inputSchema":{"type":"object","properties":{}}},
 {"name":"set_brief","description":"Save the arrangement brief: what the piece is, how it's built, what each section should feel like and what impact it should have, which part leads where. Plain text, up to about 2900 characters. Saved with the project.","inputSchema":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}},
 {"name":"set_sections","description":"Set the song's sections (up to 16), replacing any existing ones. Each scales every track's Amount and Movement for its bars (inclusive, counted from bar 1 at the project's time signature), gliding between sections. They multiply with Logic automation. Saved with the project.","inputSchema":{"type":"object","properties":{"sections":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"start_bar":{"type":"number"},"end_bar":{"type":"number"},"feel":{"type":"string","description":"Short: how it should feel and land"},"lead_slot":{"type":"integer","description":"Optional: the track that leads this section (see set_section_lead)"},"amount":{"type":"number","minimum":0,"maximum":1},"movement":{"type":"number","minimum":0,"maximum":1}},"required":["name","start_bar","end_bar"]}}},"required":["sections"]}},
 {"name":"set_section_lead","description":"Make one track lead a section (or clear it with slot -1). In that section everything else eases back in the lead's range only while it plays; a bass lead owns the low end. Takes effect at the next get_suggestions + apply. Saved with the project.","inputSchema":{"type":"object","properties":{"section":{"type":["string","integer"],"description":"Section name or 1-based number"},"slot":{"type":"integer"}},"required":["section","slot"]}},
 {"name":"add_dip","description":"Place a dip by hand: the follower tracks ease back by depth_db at freq (bell, or low/high shelf) only while the leader plays, optionally only in some sections. Returns a rule id. Undoable; remove_dip takes it out.","inputSchema":{"type":"object","properties":{"leader_slot":{"type":"integer"},"follower_slots":{"type":"array","items":{"type":"integer"}},"freq":{"type":"number"},"type":{"type":"string","enum":["bell","low shelf","high shelf"]},"q":{"type":"number"},"depth_db":{"type":"number","minimum":0.5,"maximum":6},"sections":{"type":"array","items":{"type":["string","integer"]}}},"required":["leader_slot","follower_slots","freq","depth_db"]}},
 {"name":"remove_dip","description":"Remove a hand-placed dip (by rule id) from every track. Undoable.","inputSchema":{"type":"object","properties":{"rule":{"type":"integer"}},"required":["rule"]}},
 {"name":"listen","description":"Start or stop capturing what every track plays, or get the capture status. Start clears the previous capture. Audio must actually play in Logic while listening.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","stop","status"]}},"required":["action"]}},
 {"name":"get_suggestions","description":"Plan small EQ changes and level moves from the captured audio. Level moves are in 'balance' (apply takes their ids too). Each suggestion has an id, the track, the move, a reason and a confidence 0-1. 'dynamic' suggestions are focus-following dips that move only while the focus plays. Also returns notes (layer report: near-silent layers, near-identical doubled layers, parts not heard long enough). Nothing is applied yet.","inputSchema":{"type":"object","properties":{"group":{"type":"integer"},"style":{"type":"string","enum":["band","orchestral"],"description":"Default: the style most instances are set to."},"bolder":{"type":"boolean","description":"Larger moves (strength 1.0 instead of 0.6)."},"movement":{"type":"boolean","description":"Propose focus-following dips (default true)."},"balance":{"type":"boolean","description":"Also propose level moves (default true), per saved section where sections exist."},"harmonics":{"type":"boolean","description":"Also propose harmonic bass and presence fill (default true)."}}}},
 {"name":"apply","description":"Apply suggestions by id (default: all from the last get_suggestions). Each affected track's CLEARING EQ is replaced by its chosen suggestions; other tracks are untouched. Undoable as one step.","inputSchema":{"type":"object","properties":{"ids":{"type":"array","items":{"type":"integer"}}}}},
 {"name":"clear_eq","description":"Return tracks to flat (all CLEARING EQ off). Undoable.","inputSchema":{"type":"object","properties":{"slots":{"type":"array","items":{"type":"integer"},"description":"Default: every track in group 1."},"levels":{"type":"boolean","description":"Also clear planned level moves (default false)."},"harmonics":{"type":"boolean","description":"Also clear harmonics (default: same as eq)."},"eq":{"type":"boolean","description":"Clear the EQ (default true). eq false + levels true clears only the levels."}}}},
 {"name":"undo","description":"Undo the last apply or clear_eq, for every track it changed.","inputSchema":{"type":"object","properties":{}}},
 {"name":"set_amount","description":"Set Amount (0-1, how much of the applied EQ is heard) on every track in group 1 at once. Per-track: use set_track. Saved with the project; the user can automate it per track in Logic.","inputSchema":{"type":"object","properties":{"amount":{"type":"number","minimum":0,"maximum":1}},"required":["amount"]}},
 {"name":"set_movement","description":"Set Movement (0-1, how deep the focus-following dips go) on every track in group 1 at once. Per-track: use set_track.","inputSchema":{"type":"object","properties":{"movement":{"type":"number","minimum":0,"maximum":1}},"required":["movement"]}},
 {"name":"analyse_arrangement","description":"Read the song over time from the per-bar data every instance records while listening: tempo, bars heard, the sections (saved, or detected from who plays where), who plays in each, the leader's share of its band, and, with a CLEARING on the stereo out in Master mode, each track's real level at the output (fader, pan, buses) and loudness per section. Stores the levels so get_suggestions plans from what's actually heard.","inputSchema":{"type":"object","properties":{}}},
 {"name":"render_view","description":"Draw a picture for you and the user (PNG, also saved to ~/Music/CLEARING Views). arrangement: tracks against bars with sections and the stereo-out loudness curve. masking: each track's share of each band, for one section or the whole song. spectrum: the stereo out's bands over time. stereo: correlation (mono-safety) per band and bar, plus true peak. balance: each part against its target level, per section, with the proposed moves (run balance_mix first).","inputSchema":{"type":"object","properties":{"view":{"type":"string","enum":["arrangement","masking","spectrum","stereo","balance"]},"section":{"type":["string","integer"],"description":"masking: section name or 1-based number (default: whole song)"}},"required":["view"]}},
 {"name":"set_trim","description":"Plan a level move by hand for one track: the whole song, or one saved section (-6 to +6 dB). It glides in, adds to the track's automatable Trim parameter, is saved with the project and is undoable.","inputSchema":{"type":"object","properties":{"slot":{"type":"integer"},"db":{"type":"number","minimum":-6,"maximum":6},"section":{"type":["string","integer"]}},"required":["slot","db"]}},
 {"name":"mix_health","description":"Check the finished mix from the stereo-out instance's last listen: integrated loudness, loudest 3 s, true and sample peak; per section and band, correlation and what survives a mono fold-down; warnings (wide or out-of-phase low end, mono cancellation, peaks near 0 dBTP); and findings from comparing the stereo out with the tracks: energy no CLEARING track accounts for (a part, return or bus effect) and low end lost where low parts overlap (phase).","inputSchema":{"type":"object","properties":{}}},
 {"name":"set_harmonics","description":"Harmonics by hand on one track. kind bass: harmonic bass, saturating only lo_hz-hi_hz and keeping the harmonics between hp_hz and lp_hz at mix_db relative to the band (drive 0-1; sub_db optionally lowers the sub, <= 0). kind presence: presence fill around freq_hz (q), harmonics above it at mix_db. on false switches that kind off. Undoable.","inputSchema":{"type":"object","properties":{"slot":{"type":"integer"},"kind":{"type":"string","enum":["bass","presence"]},"on":{"type":"boolean"},"lo_hz":{"type":"number"},"hi_hz":{"type":"number"},"hp_hz":{"type":"number"},"lp_hz":{"type":"number"},"freq_hz":{"type":"number"},"q":{"type":"number"},"mix_db":{"type":"number","minimum":-30,"maximum":0},"drive":{"type":"number","minimum":0,"maximum":1},"sub_db":{"type":"number","minimum":-6,"maximum":0}},"required":["slot","kind"]}},
 {"name":"balance_mix","description":"Balance the levels: how loud each part really is at the stereo out, section by section, against the balance a style calls for, and the fader moves (whole song, plus rides per saved section; up to max_db, default 6) that get there, each with a reason and an id for apply (they go into each track's Trim). Also checks gain staging. style: industrial, rock, pop, electronic, hip hop, orchestral, ambient. lead: on top (forward, pop/country), blended (clear but part of the band), inside (half-buried in the wall). Needs a listen; best with CLEARING on every track and on the Stereo Out in Master mode.","inputSchema":{"type":"object","properties":{"style":{"type":"string"},"lead":{"type":"string","enum":["on top","blended","inside"]},"max_db":{"type":"number","minimum":1,"maximum":6}}}},
 {"name":"status","description":"Start here: what CLEARING can see right now (version, live tracks, stereo-out instance, listening, bars heard) and the next step.","inputSchema":{"type":"object","properties":{}}},
 {"name":"compare","description":"Bypass (on=true) or restore (on=false) CLEARING on every track, to hear before/after. Level-matched by default: each track plays at the loudness CLEARING gives it, so the comparison is about tone. matched false: original levels (use this while listening for analyse_arrangement's level reading).","inputSchema":{"type":"object","properties":{"on":{"type":"boolean"},"matched":{"type":"boolean"}},"required":["on"]}}
])JSON";

static const char* kVersion = "0.8.0";

class Server {
public:
    Server() { hubOk = hub.open(); }

    // ---- presence and activity, shown in the plug-in window ----
    String clientName() const { return client; }
    void setClient(const String& raw)
    {
        String n = raw.trim();
        if (n.isEmpty()) n = "an MCP client";
        else if (n.containsIgnoreCase("claude")) n = "Claude";
        else if (n == "bridge") n = "the CLEARING bridge";
        client = n;
        auto* h = hub.get(); if (!h) return;
        seqWrite(h->llmSeq, [&] {
            std::strncpy(h->llmClient, client.toRawUTF8(), sizeof h->llmClient - 1);
            std::strncpy(h->llmVersion, kVersion, sizeof h->llmVersion - 1);
        });
        beat();
    }
    void beat() { if (auto* h = hub.get()) h->llmBeatNs.store(Hub::nowNs()); }
    void logAction(const String& what)
    {
        auto* h = hub.get(); if (!h || what.isEmpty()) return;
        seqWrite(h->llmSeq, [&] {
            auto& e = h->log[h->logCount % HubData::kLog];
            e.unixTime = (int64_t) std::time(nullptr);
            std::memset(e.text, 0, sizeof e.text);
            std::strncpy(e.text, what.toRawUTF8(), sizeof e.text - 1);
            ++h->logCount;
        });
    }

    var handle(const var& req)
    {
        const String method = req["method"].toString();
        const var id = req["id"];
        const bool notification = !req.hasProperty("id");
        var result;
        if (method == "initialize") {
            result = obj();
            const String pv = req["params"]["protocolVersion"].toString();
            put(result, "protocolVersion", pv.isNotEmpty() ? pv : String("2025-06-18"));
            var tc = obj(); put(tc, "listChanged", true);
            var caps = obj(); put(caps, "tools", tc); put(result, "capabilities", caps);
            var info = obj(); put(info, "name", "clearing"); put(info, "version", kVersion); put(result, "serverInfo", info);
            setClient(req["params"]["clientInfo"]["name"].toString());
            put(result, "instructions", String(kInstructions));
        } else if (method == "tools/list") {
            result = obj(); put(result, "tools", juce::JSON::parse(String(kTools)));
        } else if (method == "tools/call") {
            result = callTool(req["params"]["name"].toString(), req["params"]["arguments"]);
        } else if (method == "ping") {
            result = obj();
        } else {
            if (notification) return var();
            var err = obj(); put(err, "code", -32601); put(err, "message", "Method not found: " + method);
            var resp = obj(); put(resp, "jsonrpc", "2.0"); put(resp, "id", id); put(resp, "error", err);
            return resp;
        }
        if (notification) return var();
        var resp = obj(); put(resp, "jsonrpc", "2.0"); put(resp, "id", id); put(resp, "result", result);
        return resp;
    }

private:
    struct Sug { int id = 0; int slot = -1; String track; Change c; bool balance = false; float gainDb = 0; int section = -1; bool harm = false; HarmSuggestion h; };

    var text(const var& payload, bool error = false)
    {
        var r = obj(), item = obj();
        put(item, "type", "text");
        put(item, "text", payload.isString() ? payload.toString() : juce::JSON::toString(payload, false, 3));
        put(r, "content", juce::Array<var> { item });
        if (error) put(r, "isError", true);
        return r;
    }
    var fail(const String& msg) { return text(msg, true); }

    var callTool(const String& name, const var& args)
    {
        if (auto* h = hub.get()) { h->llmLastCallNs.store(Hub::nowNs()); beat(); }
        String before;
        if (name == "set_track" || name == "set_harmonics" || name == "set_trim") before = trackName((int) args["slot"]);
        var r = callToolInner(name, args);
        if (!(bool) r.getProperty("isError", false)) logAction(describe(name, args, r, before));
        return r;
    }
    String trackName(int slot)
    {
        for (auto& t : read(0)) if (t.slot == slot) return t.name;
        return "slot " + String(slot);
    }
    // One plain line per action for the window's activity list; read-only calls aren't logged.
    String describe(const String& name, const var& a, const var& r, const String& track)
    {
        const String body = r["content"][0]["text"].toString();
        var j; juce::JSON::parse(body, j);
        if (name == "listen") { const String x = a["action"].toString(); return x == "start" ? "started listening" : x == "stop" ? "stopped listening" : String(); }
        if (name == "set_track") {
            juce::StringArray w;
            if (a.hasProperty("role")) w.add("role " + j["role"].toString());
            if (a.hasProperty("priority")) w.add("priority " + String((double) a["priority"], 2));
            if (a.hasProperty("layer")) w.add((int) a["layer"] ? "layer L" + String((int) a["layer"]) : String("no layer"));
            if (a.hasProperty("locked")) w.add((bool) a["locked"] ? "locked" : "unlocked");
            if (a.hasProperty("amount")) w.add("amount " + String(juce::roundToInt((double) a["amount"] * 100)) + "%");
            if (a.hasProperty("movement")) w.add("movement " + String(juce::roundToInt((double) a["movement"] * 100)) + "%");
            if (a.hasProperty("name")) w.add("renamed");
            return track + ": " + w.joinIntoString(", ");
        }
        if (name == "get_suggestions") {
            const int n = j["suggestions"].size() + j["balance"].size() + j["harmonics"].size();
            return n ? "proposed " + String(n) + " change" + (n == 1 ? "" : "s") + " (nothing applied yet)" : String("checked the mix: nothing worth changing");
        }
        if (name == "apply") {
            juce::StringArray tr;
            if (auto* arr = j["applied"].getArray()) for (auto& x : *arr) tr.addIfNotAlreadyThere(x["track"].toString());
            const int n = j["applied"].size();
            return "applied " + String(n) + " change" + (n == 1 ? "" : "s") + (tr.isEmpty() ? String() : " on " + tr.joinIntoString(", "));
        }
        if (name == "undo") return "undid the last step";
        if (name == "clear_eq") return "cleared " + body.fromFirstOccurrenceOf("Cleared ", false, false).upToFirstOccurrenceOf(" on ", false, false);
        if (name == "set_brief") return "saved the arrangement brief";
        if (name == "set_sections") return "saved " + String(j["sections_set"].toString()) + " sections";
        if (name == "set_section_lead") return "set a section lead";
        if (name == "add_dip") return "placed a dip for " + j["leader"].toString();
        if (name == "remove_dip") return "removed a dip";
        if (name == "set_trim") return track + ": level " + String((double) a["db"], 1) + " dB" + (a.hasProperty("section") ? " in " + a["section"].toString() : String());
        if (name == "set_harmonics") return track + ": " + a.getProperty("kind", "bass").toString() + (a.getProperty("kind", "bass").toString() == "bass" ? " harmonics " : " fill ") + ((bool) a.getProperty("on", true) ? "on" : "off");
        if (name == "compare") return (bool) a["on"] ? String("compare on") + ((bool) a.getProperty("matched", true) ? " (level-matched)" : "") : String("compare off");
        if (name == "set_amount") return "amount " + String(juce::roundToInt((double) a["amount"] * 100)) + "% on every track";
        if (name == "set_movement") return "movement " + String(juce::roundToInt((double) a["movement"] * 100)) + "% on every track";
        if (name == "analyse_arrangement") return "read the arrangement (" + j["bars_heard"].toString() + " bars)";
        if (name == "render_view") return "drew a picture: " + a["view"].toString();
        if (name == "balance_mix") return "measured the balance: " + String(j["moves"].size()) + " level move" + (j["moves"].size() == 1 ? "" : "s") + " proposed (" + j["style"].toString() + ", lead " + j["lead"].toString() + "; nothing applied yet)";
        if (name == "mix_health") return "checked mix health (" + String(j["warnings"].size()) + " warning" + (j["warnings"].size() == 1 ? "" : "s") + ")";
        return String();
    }

    var callToolInner(const String& name, const var& args)
    {
        if (name == "status") return status();
        if (!hubOk) return fail("Cannot open CLEARING's shared hub. Is the plug-in installed, and is Logic running with CLEARING on some tracks?");
        if (name == "list_tracks") return listTracks(args);
        if (name == "list_roles") return listRoles();
        if (name == "set_track") return setTrack(args);
        if (name == "listen") return listen(args);
        if (name == "get_suggestions") return suggestions(args);
        if (name == "apply") return apply(args);
        if (name == "clear_eq") return clearEq(args);
        if (name == "undo") return undo();
        if (name == "get_brief") return getBrief();
        if (name == "set_brief") return setBrief(args);
        if (name == "set_sections") return setSections(args);
        if (name == "set_section_lead") return setSectionLead(args);
        if (name == "add_dip") return addDip(args);
        if (name == "remove_dip") return removeDip(args);
        if (name == "analyse_arrangement") return analyseArrangement(args);
        if (name == "render_view") return renderView(args);
        if (name == "set_trim") return setTrim(args);
        if (name == "set_harmonics") return setHarmonics(args);
        if (name == "mix_health") return mixHealth(args);
        if (name == "balance_mix") return balanceMix(args);
        if (name == "set_amount" || name == "set_movement") {
            const bool amt = name == "set_amount";
            const float v = juce::jlimit(0.0f, 1.0f, (float) (double) args[amt ? "amount" : "movement"]);
            int n = 0;
            for (auto& t : read(1)) { edit(t, -1, amt ? v : -1.0f, amt ? -1.0f : v); ++n; }
            return text(String(amt ? "Amount" : "Movement") + " set to " + String(juce::roundToInt(v * 100)) + "% on " + String(n)
                        + " track(s). It's a per-track parameter, so it's saved with the project and can be automated in Logic.");
        }
        if (name == "compare") {
            const bool on = (bool) args["on"];
            const bool matched = (bool) args.getProperty("matched", true);
            hub.get()->compareMatched.store(matched ? 1 : 0);
            hub.get()->bypassAll.store(on ? 1 : 0);
            return text(on ? String(matched ? "CLEARING bypassed on every track, each at the loudness CLEARING gives it (level-matched: you're judging tone, not volume)."
                                            : "CLEARING bypassed on every track at its original level (not level-matched).")
                           : String("CLEARING active again on every track."));
        }
        return fail("Unknown tool: " + name);
    }

    std::vector<TrackSnap> read(int group)
    {
        std::vector<TrackSnap> out;
        auto* h = hub.get();
        for (int i = 0; i < kSlots; ++i) {
            if (!hub.live(i)) continue;
            auto& s = h->slots[i];
            TrackSnap t; t.slot = i;
            char nm[24] {};
            int locked = 0, hn = 0;
            if (!seqRead(s.measSeq, [&] {
                    std::memcpy(nm, s.name, sizeof nm); t.role = s.role; t.group = s.group; t.style = s.style; locked = s.locked;
                    hn = s.hostNamed; t.priority = s.priority; t.activeSec = s.activeSec; t.rmsDb = s.rmsDb;
                    t.capRmsDb = s.capRmsDb; t.capPeakDb = s.capPeakDb; t.layer = s.layer; t.section = s.section; t.amount = s.amount; t.movement = s.movement;
                    std::copy(s.cap, s.cap + kBands, t.cap.begin()); std::copy(s.live, s.live + kBands, t.live.begin()); std::copy(s.capPeak, s.capPeak + kBands, t.capPeak.begin());
                    t.mode = s.mode; t.trimParam = s.trimParamDb; t.bpm = s.bpm; t.meterNum = s.meterNum; t.meterDen = s.meterDen; t.matchDb = s.matchDb;
                })) continue;
            seqRead(s.planSeq, [&] { std::copy(s.plan, s.plan + kEqBands, t.plan.begin()); t.trim = s.trim; t.harm = s.harm; });
            if (t.mode == 1) {
                t.health = std::make_shared<TrackSnap::Health>();
                auto& H = *t.health;
                seqRead(s.hSeq, [&] {
                    H.bars = juce::jlimit(0, kBars, (int) s.hBars);
                    for (int b = 0; b < H.bars; ++b) { for (int k = 0; k < kHBands; ++k) { H.corr[b][k] = s.hCorr[b][k]; H.mono[b][k] = s.hMono[b][k]; H.en[b][k] = s.hEn[b][k]; } H.tp[b] = s.hTp[b]; }
                    H.lufsI = s.hLufsI; H.lufsS = s.hLufsSmax; H.tpMax = s.hTpMax; H.spMax = s.hSpMax;
                });
            }
            t.tl = std::make_shared<Timeline>();
            for (auto& r : *t.tl) r.fill(kFloorDb);
            t.tlLufs.fill(kFloorDb);
            seqRead(s.tlSeq, [&] {
                t.tlBars = juce::jlimit(0, kBars, (int) s.tlBars);
                for (int b = 0; b < t.tlBars; ++b) { std::copy(s.tl[b], s.tl[b] + kCoarse, (*t.tl)[(size_t) b].begin()); t.tlLufs[(size_t) b] = s.tlLufs[b]; }
            });
            nm[sizeof nm - 1] = 0;
            t.name = String::fromUTF8(nm); t.locked = locked != 0; t.hostNamed = hn != 0; t.uid = s.owner.load();
            if (group == 0 || t.group == group) out.push_back(t);
        }
        return out;
    }

    static float regionDb(const std::array<float, kBands>& d, float lo, float hi)
    {
        double p = 0; int n = 0;
        for (int b = 0; b < kBands; ++b) { const float f = bandCentre(b); if (f >= lo && f < hi) { p += std::pow(10.0, d[(size_t) b] / 10.0); ++n; } }
        return n ? (float) (10.0 * std::log10(p / n + 1e-14)) : kFloorDb;
    }

    var spectrumSummary(const TrackSnap& t)
    {
        const bool cap = t.activeSec > 0;
        const auto& d = cap ? t.cap : t.live;
        struct R { const char* key; const char* label; float lo, hi; } rs[] = {
            { "sub", "sub (20-60 Hz)", 20, 60 }, { "low", "low (60-150 Hz)", 60, 150 }, { "low_mid", "low-mid (150-500 Hz)", 150, 500 },
            { "mid", "mid (500 Hz-2 kHz)", 500, 2000 }, { "presence", "presence (2-6 kHz)", 2000, 6000 }, { "air", "air (6-18 kHz)", 6000, 18001 } };
        float v[6], top = kFloorDb; int topI = 0;
        for (int i = 0; i < 6; ++i) { v[i] = regionDb(d, rs[i].lo, rs[i].hi); if (v[i] > top) { top = v[i]; topI = i; } }
        int pk = 0; for (int b = 1; b < kBands; ++b) if (d[(size_t) b] > d[(size_t) pk]) pk = b;
        var s = obj();
        put(s, "source", cap ? "captured while listening" : "live (not captured yet)");
        if (top < -90.0f) { put(s, "description", "silent or nearly silent"); return s; }
        var rel = obj();
        for (int i = 0; i < 6; ++i) put(rel, rs[i].key, rnd(v[i] - top, 0));
        put(s, "region_db_relative_to_strongest", rel);
        put(s, "peak", hz(bandCentre(pk)));
        String desc = "Strongest in the " + String(rs[topI].label) + ", peak near " + hz(bandCentre(pk)) + ".";
        juce::StringArray weak;
        for (int i = 0; i < 6; ++i) if (i != topI && v[i] - top < -30.0f) weak.add(String(rs[i].key).replace("_", "-"));
        if (!weak.isEmpty()) desc << " Little energy in: " << weak.joinIntoString(", ") << ".";
        put(s, "description", desc);
        return s;
    }

    var eqVar(const std::array<EqBandData, kEqBands>& plan, const std::vector<TrackSnap>& all)
    {
        juce::Array<var> a;
        for (auto& b : plan) {
            if (!b.enabled) continue;
            var e = obj();
            put(e, "type", b.self ? "self (moves when this track builds up)" : b.dynamic ? (b.type == (int) BandType::LowShelf ? "low-shelf dip (moves with its leader)" : "dip (moves with its leader)") : b.type == (int) BandType::Bell ? "bell" : b.type == (int) BandType::HighShelf ? "high shelf" : b.type == (int) BandType::LowShelf ? "low shelf" : "high pass");
            if (b.sectionMask) put(e, "sections", sectionsText(b.sectionMask));
            if (b.ruleId > 0) put(e, "rule", (int) b.ruleId);
            if (b.dynamic && !b.self) {
                String lead = "(leader track not open)";
                for (auto& t : all) if (t.uid == b.leaderUid) lead = t.name;
                put(e, "follows", lead);
            }
            put(e, "freq", hz(b.freq)); put(e, "gain_db", rnd(b.gainDb, 1)); put(e, "q", rnd(b.q, 2));
            a.add(e);
        }
        return a;
    }

    var listTracks(const var& args)
    {
        const int group = args.hasProperty("group") ? (int) args["group"] : 1;
        auto ts = read(group);
        const auto all = read(0);
        float ref = -120; for (auto& t : ts) ref = std::max(ref, t.capRmsDb > -119 ? t.capRmsDb : t.rmsDb);
        juce::Array<var> arr;
        int section = -1;
        for (auto& t : ts) {
            if (t.section >= 0) section = t.section;
            var o = obj();
            put(o, "slot", t.slot); put(o, "name", t.name);
            put(o, "name_from_logic", t.hostNamed);
            put(o, "role", roleName(t.role)); put(o, "priority", rnd(t.priority, 2)); put(o, "locked", t.locked);
            put(o, "layer", t.layer); put(o, "amount", rnd(t.amount, 2)); put(o, "movement", rnd(t.movement, 2));
            if (t.capRmsDb > -119) put(o, "captured_level_db_vs_loudest", rnd(t.capRmsDb - ref, 0));
            put(o, "group", t.group); put(o, "style", t.style == StyleOrchestral ? "orchestral" : "band");
            put(o, "level_db", rnd(t.rmsDb, 0)); put(o, "captured_seconds", rnd(t.activeSec, 1));
            put(o, "spectrum", spectrumSummary(t));
            put(o, "applied_eq", eqVar(t.plan, all));
            if (t.master()) {
                put(o, "mode", "master (stereo out): measures only, never planned");
                if (t.health && t.health->bars > 0) {
                    var hs = obj(); put(hs, "integrated_lufs", rnd(t.health->lufsI, 1)); put(hs, "true_peak_dbtp", rnd(t.health->tpMax, 1));
                    put(hs, "note", "from the last listen; mix_health has the detail"); put(o, "health", hs);
                }
            }
            if (auto hv = harmVar(t.harm); hv.size() > 0) put(o, "harmonics", hv);
            if (!t.master() && std::fabs(t.matchDb) > 0.05f) put(o, "compare_match_db", rnd(t.matchDb, 1));
            if (std::fabs(t.trimParam) > 0.01f) put(o, "trim_param_db", rnd(t.trimParam, 1));
            if (auto tv = trimVar(t.trim); tv.isObject()) put(o, "planned_level", tv);
            auto lo = levelOffset.find(t.uid);
            if (lo != levelOffset.end()) put(o, "post_fader_offset_db", rnd(lo->second, 1));
            arr.add(o);
        }
        var r = obj();
        put(r, "tracks", arr);
        auto* h = hub.get();
        put(r, "listening", h->listening.load() != 0);
        put(r, "compare_bypass", h->bypassAll.load() != 0);
        if (section >= 0) { auto secs = readSections(); if (section < (int) secs.size()) put(r, "current_section", secs[(size_t) section].toString()); }
        if (ts.empty()) put(r, "note", "No CLEARING instances found in this group. Insert CLEARING on the tracks to include (Audio FX > Audio Units > Rob Cawley > CLEARING) and keep Logic open.");
        return text(r);
    }

    var listRoles()
    {
        juce::Array<var> a;
        for (int r = 0; r < RoleCount; ++r) {
            var o = obj(); put(o, "role", roleName(r)); put(o, "default_priority", rnd(roleDefaultPriority(r), 2));
            put(o, "family", roleIsOrchestral(r) ? "orchestral" : "band"); a.add(o);
        }
        return text(a);
    }

    var setTrack(const var& args)
    {
        const int slot = (int) args["slot"];
        TrackSnap cur;
        bool found = false;
        for (auto& t : read(0)) if (t.slot == slot) { cur = t; found = true; }
        if (!found) return fail("No live CLEARING instance in slot " + String(slot) + ". Call list_tracks for valid slots.");
        int layer = -1; float amount = -1, movement = -1;
        if (args.hasProperty("layer")) layer = juce::jlimit(0, 8, (int) args["layer"]);
        if (args.hasProperty("amount")) amount = juce::jlimit(0.0f, 1.0f, (float) (double) args["amount"]);
        if (args.hasProperty("movement")) movement = juce::jlimit(0.0f, 1.0f, (float) (double) args["movement"]);
        Pending p = base(cur);
        if (args.hasProperty("role")) {
            const int r = roleFromVar(args["role"]);
            if (r < 0) return fail("Unknown role '" + args["role"].toString() + "'. Call list_roles.");
            p.role = r;
            if (!args.hasProperty("priority")) p.prio = roleDefaultPriority(r);
        }
        if (args.hasProperty("priority")) p.prio = juce::jlimit(0.0f, 1.0f, (float) (double) args["priority"]);
        if (args.hasProperty("locked")) p.locked = (bool) args["locked"];
        if (args.hasProperty("name")) p.name = args["name"].toString().trim().substring(0, 22);
        if (layer >= 0) p.layer = layer;
        if (amount >= 0) p.amount = amount;
        if (movement >= 0) p.movement = movement;
        p.ms = juce::Time::getMillisecondCounter();
        pending[slot] = p;
        sendCtrl(slot, p);
        var o = obj();
        put(o, "slot", slot); put(o, "name", p.name); put(o, "role", roleName(p.role)); put(o, "priority", rnd(p.prio, 2)); put(o, "locked", p.locked);
        if (layer >= 0) put(o, "layer", layer);
        if (amount >= 0) put(o, "amount", rnd(amount, 2));
        if (movement >= 0) put(o, "movement", rnd(movement, 2));
        put(o, "note", "Sent. The instance applies it within about a tenth of a second.");
        return text(o);
    }

    var listen(const var& args)
    {
        auto* h = hub.get();
        const String a = args["action"].toString();
        if (a == "start") {
            last.clear();
            h->listenEpoch.fetch_add(1);
            h->listening.store(1);
            return text("Listening. Have the busiest 30-60 seconds of the song play in Logic, then call listen with action stop.");
        }
        if (a == "stop") h->listening.store(0);
        else if (a != "status") return fail("action must be start, stop or status");
        var r = obj();
        put(r, "listening", h->listening.load() != 0);
        juce::Array<var> arr; float mx = 0;
        for (auto& t : read(0)) {
            var o = obj(); put(o, "slot", t.slot); put(o, "name", t.name); put(o, "captured_seconds", rnd(t.activeSec, 1));
            mx = std::max(mx, t.activeSec); arr.add(o);
        }
        put(r, "tracks", arr);
        if (mx < 8.0f) put(r, "note", "Under 8 s captured. Tracks need at least 8 s of audible material before CLEARING will suggest anything for them. Is Logic playing?");
        return text(r);
    }

    var suggestions(const var& args)
    {
        const int group = args.hasProperty("group") ? (int) args["group"] : 1;
        auto ts = read(group);
        std::vector<TrackMeas> in;
        int orch = 0, usedOffsets = 0;
        for (auto& t : ts) {
            if (t.master()) continue;
            if (t.style == StyleOrchestral) ++orch;
            if (t.activeSec <= 0) continue;
            TrackMeas m; m.id = t.slot; m.name = t.name.toStdString(); m.role = t.role; m.priority = t.priority;
            m.locked = t.locked; m.layer = t.layer; m.db = t.cap; m.peakDb = t.capPeak; m.levelDb = t.capRmsDb; m.activeSec = t.activeSec;
            if (auto lo = levelOffset.find(t.uid); lo != levelOffset.end() && lo->second > kFloorDb + 1) {
                // what the stereo out hears: insert level plus fader/pan/bus offset (and planned trims)
                const float off = lo->second + t.trim.baseDb;
                for (auto& d : m.db) if (d > kFloorDb + 1) d += off;
                for (auto& d : m.peakDb) if (d > kFloorDb + 1) d += off;
                m.levelDb += off;
                ++usedOffsets;
            }
            in.push_back(m);
        }
        if (in.empty()) return fail("Nothing captured yet. Use listen start, play the song in Logic, then listen stop.");
        PlannerSettings st;
        st.strength = (bool) args.getProperty("bolder", false) ? 1.0f : 0.6f;
        st.dynamics = (bool) args.getProperty("movement", true);
        for (auto& sec : readSections()) {
            st.sectionNames.push_back(sec.name.toStdString());
            int L = -1; if (sec.lead) for (auto& t : ts) if (t.uid == sec.lead) L = t.slot;
            st.sectionLeads.push_back(L);
        }
        const String style = args["style"].toString();
        st.style = style == "orchestral" ? StyleOrchestral : style == "band" ? StyleBand : (orch * 2 > (int) ts.size() ? StyleOrchestral : StyleBand);
        last.clear();
        juce::Array<var> arr;
        auto res = planFull(in, st);
        for (auto& c : res.changes) {
            Sug s; s.id = nextId++; s.slot = c.track; s.c = c;
            for (auto& t : ts) if (t.slot == c.track) s.track = t.name;
            last.push_back(s);
            var o = obj();
            put(o, "id", s.id); put(o, "slot", s.slot); put(o, "track", s.track);
            put(o, "move", String(c.gainDb > 0 ? "+" : "") + String(c.gainDb, 1) + " dB " + (c.self ? "self-triggered cut" : c.dynamic ? (c.type == BandType::LowShelf ? "low-shelf dip" : "dip") : c.type == BandType::Bell ? "bell" : "high shelf") + (c.type == BandType::LowShelf && c.dynamic ? " below " : " at ") + hz(c.freq) + ", Q " + String(c.q, 1));
            put(o, "dynamic", c.dynamic); put(o, "self", c.self);
            if (c.sectionMask) put(o, "sections", sectionsText(c.sectionMask));
            put(o, "confidence", rnd(c.confidence, 2)); put(o, "reason", String(c.reason));
            if (!c.cause.empty()) put(o, "protects", String(c.cause));
            arr.add(o);
        }
        // Harmonics: harmonic bass where the low end won't translate, presence fill for buried leads.
        juce::Array<var> harmArr;
        if ((bool) args.getProperty("harmonics", true)) {
            std::vector<int> leads;
            { int best = -1; float bp = -1; for (auto& m : in) if (!m.locked && m.priority > bp) { bp = m.priority; best = m.id; } if (best >= 0) leads.push_back(best); }
            for (int L : st.sectionLeads) if (L >= 0 && std::find(leads.begin(), leads.end(), L) == leads.end()) leads.push_back(L);
            for (auto& hsug : planHarmonics(in, leads, st.style)) {
                Sug s; s.id = nextId++; s.slot = hsug.track; s.harm = true; s.h = hsug;
                for (auto& t : ts) if (t.slot == hsug.track) s.track = t.name;
                last.push_back(s);
                var o = obj();
                put(o, "id", s.id); put(o, "slot", s.slot); put(o, "track", s.track);
                put(o, "move", hsug.bass ? "harmonic bass: harmonics of " + hz(hsug.plan.bassLo) + "-" + hz(hsug.plan.bassHi) + " kept " + hz(hsug.plan.bassHp) + "-" + hz(hsug.plan.bassLp) + " at " + String(hsug.plan.bassMixDb, 0) + " dB"
                                         : "presence fill: harmonics above " + hz(hsug.plan.presFreq) + " at " + String(hsug.plan.presMixDb, 0) + " dB");
                put(o, "confidence", rnd(hsug.confidence, 2)); put(o, "reason", String(hsug.reason));
                harmArr.add(o);
            }
        }
        // Balance: level moves from the per-bar timelines.
        juce::Array<var> notes;
        juce::Array<var> bal;
        if ((bool) args.getProperty("balance", true)) {
            auto tt = timelines(ts);
            auto saved = readSections();
            std::vector<SectionSpan> spans;
            for (auto& s : saved) {
                SectionSpan sp; sp.name = s.name.toStdString(); sp.start = (int) s.start; sp.end = (int) s.end;
                if (s.lead) for (auto& t : ts) if (t.uid == s.lead) sp.leadId = t.slot;
                spans.push_back(sp);
            }
            std::vector<std::string> bn;
            if (!tt.empty()) {
                for (auto& m : planBalance(tt, spans, st.style, &bn)) {
                    Sug s; s.id = nextId++; s.slot = m.track; s.balance = true; s.gainDb = m.gainDb; s.section = m.section;
                    for (auto& t : ts) if (t.slot == m.track) s.track = t.name;
                    last.push_back(s);
                    var o = obj();
                    put(o, "id", s.id); put(o, "slot", s.slot); put(o, "track", s.track);
                    put(o, "move", String(m.gainDb > 0 ? "+" : "") + String(m.gainDb, 1) + " dB level"
                                   + (m.section >= 0 && m.section < (int) saved.size() ? " in " + saved[(size_t) m.section].name : String(" (whole song)")));
                    put(o, "confidence", rnd(m.confidence, 2)); put(o, "reason", String(m.reason));
                    bal.add(o);
                }
                if (spans.empty()) notes.add(String("Level moves are for the whole song: no sections are saved. analyse_arrangement proposes sections; save them with set_sections for per-section moves."));
            } else {
                notes.add(String("No per-bar data yet, so no level moves. Listen again with Logic playing from the start of the song (the playhead must move for bars to register)."));
            }
            for (auto& n : bn) notes.add(String(n));
        }
        var r = obj();
        put(r, "style", st.style == StyleOrchestral ? "orchestral" : "band");
        put(r, "suggestions", arr);
        put(r, "balance", bal);
        put(r, "harmonics", harmArr);
        put(r, "levels", usedOffsets ? "planned from what the stereo out hears (post-fader offsets from analyse_arrangement)"
                                     : "planned from insert levels (pre-fader). Put CLEARING on the stereo out in Master mode and run analyse_arrangement for post-fader levels.");
        for (auto& n : res.notes) notes.add(String(n));
        put(r, "notes", notes);
        if (arr.isEmpty() && bal.isEmpty() && harmArr.isEmpty()) put(r, "note", "Nothing worth changing was found. That's a valid result: CLEARING leaves tracks alone rather than forcing moves.");
        return text(r);
    }

    String sectionsText(uint32_t mask)
    {
        auto secs = readSections();
        juce::StringArray s;
        for (int k = 0; k < 32; ++k) if (mask >> k & 1u) s.add(k < (int) secs.size() ? secs[(size_t) k].name : "section " + String(k + 1));
        return s.joinIntoString(", ");
    }
    int sectionIndex(const var& v)
    {
        auto secs = readSections();
        if (v.isInt() || v.isDouble()) { const int k = (int) v - 1; return k >= 0 && k < (int) secs.size() ? k : -1; }
        for (size_t k = 0; k < secs.size(); ++k) if (secs[k].name.equalsIgnoreCase(v.toString().trim())) return (int) k;
        return -1;
    }
    var setSectionLead(const var& args)
    {
        const int k = sectionIndex(args["section"]);
        if (k < 0) return fail("No section '" + args["section"].toString() + "'. Call get_brief for the section names.");
        const int slot = (int) args["slot"];
        uint64_t uid = 0; String name = "the overall focus";
        if (slot >= 0) {
            if (!hub.live(slot)) return fail("No live CLEARING instance in slot " + String(slot) + ".");
            uid = hub.get()->slots[slot].owner.load();
            for (auto& t : read(0)) if (t.slot == slot) name = t.name;
        }
        auto* h = hub.get();
        seqWrite(h->briefSeq, [&] { h->sections[k].leadUid = uid; });
        return text(readSections()[(size_t) k].name + " is now led by " + name
                    + ". Run get_suggestions (and apply) so the other parts make room for it there.");
    }
    var addDip(const var& args)
    {
        const int L = (int) args["leader_slot"];
        if (!hub.live(L)) return fail("No live CLEARING instance in slot " + String(L) + ".");
        auto* fol = args["follower_slots"].getArray();
        if (!fol || fol->isEmpty()) return fail("follower_slots must list at least one track.");
        uint32_t mask = 0;
        if (auto* sa = args["sections"].getArray())
            for (auto& v : *sa) { const int k = sectionIndex(v); if (k < 0) return fail("No section '" + v.toString() + "'."); mask |= 1u << k; }
        TrackSnap lead; for (auto& t : read(0)) if (t.slot == L) lead = t;
        const float thr = (lead.capRmsDb > -119 ? lead.capRmsDb : lead.rmsDb > -119 ? lead.rmsDb : -40.0f) - 12.0f;
        const String ty = args.getProperty("type", "bell").toString();
        int rule = 0;
        for (int i = 0; i < kSlots; ++i) if (hub.live(i)) for (auto& b : hub.get()->slots[i].plan) rule = std::max(rule, (int) b.ruleId);
        ++rule;
        std::vector<int> slots;
        for (auto& v : *fol) { const int s = (int) v; if (s == L) return fail("A track can't follow itself."); if (!hub.live(s)) return fail("No live instance in slot " + String(s) + "."); slots.push_back(s); }
        pushUndo(slots);
        juce::Array<var> placed, full;
        for (int s : slots) {
            std::array<EqBandData, kEqBands> p;
            auto& sl = hub.get()->slots[s];
            seqRead(sl.planSeq, [&] { std::copy(sl.plan, sl.plan + kEqBands, p.begin()); });
            int free = -1; for (int i = 0; i < kEqBands; ++i) if (!p[(size_t) i].enabled) { free = i; break; }
            String nm; for (auto& t : read(0)) if (t.slot == s) nm = t.name;
            if (free < 0) { full.add(nm); continue; }
            auto& b = p[(size_t) free];
            b = EqBandData {};
            b.freq = juce::jlimit(20.0f, 18000.0f, (float) (double) args["freq"]);
            b.gainDb = -juce::jlimit(0.5f, 6.0f, (float) (double) args["depth_db"]);
            b.type = ty == "low shelf" ? (int) BandType::LowShelf : ty == "high shelf" ? (int) BandType::HighShelf : (int) BandType::Bell;
            b.q = (float) (double) args.getProperty("q", ty == "bell" ? 0.9 : 0.7);
            b.enabled = 1; b.dynamic = 1; b.threshDb = thr; b.leaderUid = hub.get()->slots[L].owner.load(); b.sectionMask = mask; b.ruleId = rule;
            writePlan(s, p);
            placed.add(nm);
        }
        var r = obj(); put(r, "rule", rule); put(r, "placed_on", placed);
        if (!full.isEmpty()) put(r, "no_room", full);
        put(r, "leader", lead.name); put(r, "sections", mask ? sectionsText(mask) : String("everywhere"));
        return text(r);
    }
    var removeDip(const var& args)
    {
        const int rule = (int) args["rule"];
        std::vector<int> slots;
        for (int i = 0; i < kSlots; ++i) if (hub.live(i)) for (auto& b : hub.get()->slots[i].plan) if (b.enabled && b.ruleId == rule) { slots.push_back(i); break; }
        if (slots.empty()) return fail("No dip with rule " + String(rule) + ".");
        pushUndo(slots);
        for (int s : slots) {
            std::array<EqBandData, kEqBands> p;
            auto& sl = hub.get()->slots[s];
            seqRead(sl.planSeq, [&] { std::copy(sl.plan, sl.plan + kEqBands, p.begin()); });
            for (auto& b : p) if (b.ruleId == rule) b = EqBandData {};
            writePlan(s, p);
        }
        return text("Removed rule " + String(rule) + " from " + String((int) slots.size()) + " track(s).");
    }

    struct UndoEntry { int slot; std::array<EqBandData, kEqBands> plan; TrimPlan trim; HarmPlan harm; };
    void pushUndo(const std::vector<int>& slots)
    {
        std::vector<UndoEntry> snap;
        for (int sl : slots) {
            auto& s = hub.get()->slots[sl];
            UndoEntry e; e.slot = sl;
            seqRead(s.planSeq, [&] { std::copy(s.plan, s.plan + kEqBands, e.plan.begin()); e.trim = s.trim; e.harm = s.harm; });
            snap.push_back(e);
        }
        undoStack.push_back(std::move(snap));
    }
    TrimPlan readTrim(int sl)
    {
        auto& s = hub.get()->slots[sl];
        TrimPlan t; seqRead(s.planSeq, [&] { t = s.trim; });
        return t;
    }
    void writeTrim(int sl, const TrimPlan& t)
    {
        auto& s = hub.get()->slots[sl];
        seqWrite(s.planSeq, [&] { s.trim = t; });
    }
    HarmPlan readHarm(int sl)
    {
        auto& s = hub.get()->slots[sl];
        HarmPlan h; seqRead(s.planSeq, [&] { h = s.harm; });
        return h;
    }
    void writeHarm(int sl, const HarmPlan& h)
    {
        auto& s = hub.get()->slots[sl];
        seqWrite(s.planSeq, [&] { s.harm = h; });
    }
    var harmVar(const HarmPlan& h)
    {
        juce::Array<var> a;
        if (h.bassOn > 0) {
            var o = obj(); put(o, "type", "harmonic bass"); put(o, "band", hz(h.bassLo) + "-" + hz(h.bassHi));
            put(o, "harmonics_kept", hz(h.bassHp) + "-" + hz(h.bassLp)); put(o, "mix_db", rnd(h.bassMixDb, 1)); put(o, "drive", rnd(h.bassDrive, 2));
            if (h.subDb < -0.01f) put(o, "sub_db", rnd(h.subDb, 1));
            a.add(o);
        }
        if (h.presOn > 0) {
            var o = obj(); put(o, "type", "presence fill"); put(o, "freq", hz(h.presFreq)); put(o, "q", rnd(h.presQ, 2));
            put(o, "mix_db", rnd(h.presMixDb, 1)); put(o, "drive", rnd(h.presDrive, 2));
            a.add(o);
        }
        return a;
    }
    var trimVar(const TrimPlan& t)
    {
        var o = obj(); bool any = false;
        if (std::fabs(t.baseDb) > 0.01f) { put(o, "whole_song_db", rnd(t.baseDb, 1)); any = true; }
        auto secs = readSections();
        for (int k = 0; k < kSections; ++k)
            if (std::fabs(t.sectionDb[k]) > 0.01f) { put(o, (k < (int) secs.size() ? secs[(size_t) k].name : "section " + String(k + 1)).toRawUTF8(), rnd(t.sectionDb[k], 1)); any = true; }
        return any ? o : var();
    }
    void writePlan(int sl, const std::array<EqBandData, kEqBands>& p)
    {
        auto& s = hub.get()->slots[sl];
        seqWrite(s.planSeq, [&] { std::copy(p.begin(), p.end(), s.plan); });
    }

    var apply(const var& args)
    {
        if (last.empty()) return fail("No suggestions to apply. Call get_suggestions first.");
        std::vector<int> ids;
        if (auto* a = args["ids"].getArray()) for (auto& v : *a) ids.push_back((int) v);
        std::map<int, std::vector<const Sug*>> bySlot;
        for (auto& s : last)
            if (ids.empty() || std::find(ids.begin(), ids.end(), s.id) != ids.end()) bySlot[s.slot].push_back(&s);
        if (bySlot.empty()) return fail("None of those ids are in the last get_suggestions result.");
        std::vector<int> slots;
        for (auto& kv : bySlot) if (hub.live(kv.first)) slots.push_back(kv.first);
        pushUndo(slots);
        juce::Array<var> done;
        for (int sl : slots) {
            std::vector<const Sug*> eq, lv, hm;
            for (auto* s : bySlot[sl]) (s->harm ? hm : s->balance ? lv : eq).push_back(s);
            if (!hm.empty()) {
                auto hp = readHarm(sl);
                for (auto* s : hm) {
                    if (s->h.bass) { const auto keepP = hp; hp = s->h.plan; hp.presOn = keepP.presOn; hp.presFreq = keepP.presFreq; hp.presQ = keepP.presQ; hp.presDrive = keepP.presDrive; hp.presMixDb = keepP.presMixDb; }
                    else { hp.presOn = 1; hp.presFreq = s->h.plan.presFreq; hp.presQ = s->h.plan.presQ; hp.presDrive = s->h.plan.presDrive; hp.presMixDb = s->h.plan.presMixDb; }
                    var o = obj(); put(o, "id", s->id); put(o, "track", s->track); put(o, "harmonics", true); done.add(o);
                }
                writeHarm(sl, hp);
            }
            if (!lv.empty()) {
                auto tr = readTrim(sl);
                for (auto* s : lv) {
                    if (s->section < 0) tr.baseDb = s->gainDb; else if (s->section < kSections) tr.sectionDb[s->section] = s->gainDb;
                    var o = obj(); put(o, "id", s->id); put(o, "track", s->track); put(o, "level", true); done.add(o);
                }
                writeTrim(sl, tr);
            }
            if (eq.empty()) continue; // a level- or harmonics-only apply keeps the track's EQ
            std::array<EqBandData, kEqBands> p {};
            int n = 0;
            { // hand-placed dips (rules) stay
                std::array<EqBandData, kEqBands> cur;
                auto& s0 = hub.get()->slots[sl];
                seqRead(s0.planSeq, [&] { std::copy(s0.plan, s0.plan + kEqBands, cur.begin()); });
                for (auto& b : cur) if (b.enabled && b.ruleId > 0 && n < kEqBands) p[(size_t) n++] = b;
            }
            for (auto* s : eq) {
                if (n >= kEqBands) break;
                p[(size_t) n].freq = s->c.freq; p[(size_t) n].gainDb = s->c.gainDb; p[(size_t) n].q = s->c.q;
                p[(size_t) n].type = (int) s->c.type; p[(size_t) n].enabled = 1;
                if (s->c.dynamic) {
                    p[(size_t) n].dynamic = 1; p[(size_t) n].threshDb = s->c.threshDb;
                    p[(size_t) n].sectionMask = s->c.sectionMask; p[(size_t) n].self = s->c.self ? 1 : 0;
                    p[(size_t) n].leaderUid = s->c.leader >= 0 ? hub.get()->slots[s->c.leader].owner.load() : 0;
                }
                ++n;
                var o = obj(); put(o, "id", s->id); put(o, "track", s->track); done.add(o);
            }
            writePlan(sl, p);
        }
        var r = obj(); put(r, "applied", done);
        put(r, "note", "Applied. Suggest comparing with compare on/off; undo reverts this whole step.");
        return text(r);
    }

    var clearEq(const var& args)
    {
        std::vector<int> slots;
        if (auto* a = args["slots"].getArray()) for (auto& v : *a) { if (hub.live((int) v)) slots.push_back((int) v); }
        else for (auto& t : read(1)) if (!t.master()) slots.push_back(t.slot);
        if (slots.empty()) return fail("No matching live tracks.");
        pushUndo(slots);
        const bool levels = (bool) args.getProperty("levels", false), eq = (bool) args.getProperty("eq", true);
        const bool harmonics = (bool) args.getProperty("harmonics", eq);
        for (int sl : slots) { if (eq) writePlan(sl, {}); if (levels) writeTrim(sl, {}); if (harmonics) writeHarm(sl, {}); }
        return text(String(eq && levels ? "Cleared CLEARING's EQ and planned levels on " : eq ? "Cleared CLEARING's EQ (planned levels kept) on " : levels ? "Cleared planned levels (EQ kept) on " : "Changed nothing on ") + String((int) slots.size()) + " track(s).");
    }

    var undo()
    {
        if (undoStack.empty()) return fail("Nothing to undo in this session.");
        for (auto& e : undoStack.back())
            if (hub.live(e.slot)) { auto& s = hub.get()->slots[e.slot]; seqWrite(s.planSeq, [&] { std::copy(e.plan.begin(), e.plan.end(), s.plan); s.trim = e.trim; s.harm = e.harm; }); }
        undoStack.pop_back();
        return text("Undone. The affected tracks are back to their previous EQ and levels.");
    }

    struct Pending { int role = 0; float prio = 0.4f; bool locked = false; String name; juce::uint32 ms = 0;
                     int layer = -1; float amount = -1, movement = -1; }; // -1: not changed by us recently
    std::map<int, Pending> pending;

    // Our own recent edit, if the instance may not have published it yet; else what it reports.
    Pending base(const TrackSnap& t)
    {
        auto it = pending.find(t.slot);
        if (it != pending.end() && juce::Time::getMillisecondCounter() - it->second.ms < 2000) return it->second;
        Pending p; p.role = t.role; p.prio = t.priority; p.locked = t.locked; p.name = t.name;
        return p;
    }
    // Write a full control block for a track. layer / amount / movement: -1 = leave unchanged.
    void sendCtrl(int slot, const Pending& p)
    {
        auto& s = hub.get()->slots[slot];
        seqWrite(s.ctrlSeq, [&] {
            std::strncpy(s.ctrlName, p.name.toRawUTF8(), sizeof s.ctrlName - 1); s.ctrlName[sizeof s.ctrlName - 1] = 0;
            s.ctrlRole = p.role; s.ctrlPriority = p.prio; s.ctrlLocked = p.locked ? 1 : 0;
            s.ctrlLayer = p.layer; s.ctrlAmount = p.amount; s.ctrlMovement = p.movement;
        });
    }
    void edit(const TrackSnap& t, int layer, float amount, float movement)
    {
        Pending p = base(t);
        if (layer >= 0) p.layer = layer;
        if (amount >= 0) p.amount = amount;
        if (movement >= 0) p.movement = movement;
        p.ms = juce::Time::getMillisecondCounter();
        pending[t.slot] = p;
        sendCtrl(t.slot, p);
    }

    struct Sec { String name, feel; float start = 1, end = 1, amount = 1, movement = 1; uint64_t lead = 0;
                 String toString() const { return name + " (bars " + String((int) start) + "-" + String((int) end) + ")" + (feel.isNotEmpty() ? ": " + feel : String()); } };
    std::vector<Sec> readSections(String* briefOut = nullptr)
    {
        std::vector<Sec> out;
        auto* h = hub.get();
        char brief[kBriefLen]; SectionData secs[kSections]; int n = 0;
        if (!seqRead(h->briefSeq, [&] { std::memcpy(brief, h->brief, sizeof brief); n = juce::jlimit(0, kSections, (int) h->numSections);
                                        std::copy(h->sections, h->sections + kSections, secs); })) return out;
        brief[kBriefLen - 1] = 0;
        if (briefOut) *briefOut = String::fromUTF8(brief);
        for (int i = 0; i < n; ++i) {
            Sec s; secs[i].name[23] = 0; secs[i].feel[95] = 0;
            s.name = String::fromUTF8(secs[i].name); s.feel = String::fromUTF8(secs[i].feel);
            s.start = secs[i].startBar; s.end = secs[i].endBar; s.amount = secs[i].amount; s.movement = secs[i].movement; s.lead = secs[i].leadUid;
            out.push_back(s);
        }
        return out;
    }
    var getBrief()
    {
        String brief;
        auto secs = readSections(&brief);
        var r = obj();
        put(r, "brief", brief);
        juce::Array<var> a;
        for (auto& s : secs) {
            var o = obj(); put(o, "name", s.name); put(o, "start_bar", s.start); put(o, "end_bar", s.end); put(o, "feel", s.feel);
            put(o, "amount", rnd(s.amount, 2)); put(o, "movement", rnd(s.movement, 2));
            if (s.lead) { String ln = "(track not open)"; for (auto& t : read(0)) if (t.uid == s.lead) ln = t.name; put(o, "lead", ln); }
            a.add(o);
        }
        put(r, "sections", a);
        if (brief.isEmpty() && secs.empty()) put(r, "note", "No brief yet. Ask the user about the arrangement and how each section should feel, then set_brief and set_sections.");
        return text(r);
    }
    var setBrief(const var& args)
    {
        const String t = args["text"].toString();
        auto* h = hub.get();
        seqWrite(h->briefSeq, [&] { std::strncpy(h->brief, t.toRawUTF8(), kBriefLen - 1); h->brief[kBriefLen - 1] = 0; });
        return text("Brief saved (" + String(t.getNumBytesAsUTF8()) + (t.getNumBytesAsUTF8() >= (size_t) kBriefLen - 1 ? " bytes, truncated" : " bytes")
                    + "). Every CLEARING instance stores it with the project once the project is saved.");
    }
    var setSections(const var& args)
    {
        auto* a = args["sections"].getArray();
        if (!a) return fail("sections must be an array");
        std::vector<SectionData> secs;
        for (auto& v : *a) {
            if ((int) secs.size() >= kSections) break;
            SectionData s;
            std::strncpy(s.name, v["name"].toString().toRawUTF8(), sizeof s.name - 1);
            std::strncpy(s.feel, v["feel"].toString().toRawUTF8(), sizeof s.feel - 1);
            s.startBar = (float) (double) v["start_bar"]; s.endBar = (float) (double) v["end_bar"];
            if (s.endBar < s.startBar) return fail("Section '" + v["name"].toString() + "' ends before it starts.");
            s.amount = juce::jlimit(0.0f, 1.0f, (float) (double) v.getProperty("amount", 1.0));
            s.movement = juce::jlimit(0.0f, 1.0f, (float) (double) v.getProperty("movement", 1.0));
            if (v.hasProperty("lead_slot") && (int) v["lead_slot"] >= 0 && hub.live((int) v["lead_slot"])) s.leadUid = hub.get()->slots[(int) v["lead_slot"]].owner.load();
            secs.push_back(s);
        }
        auto* h = hub.get();
        seqWrite(h->briefSeq, [&] {
            h->numSections = (int32_t) secs.size();
            for (size_t i = 0; i < secs.size(); ++i) h->sections[i] = secs[i];
        });
        var r = obj();
        put(r, "sections_set", (int) secs.size());
        put(r, "note", "Every CLEARING instance now scales its Amount and Movement by these sections as Logic plays, gliding between them, and saves them with the project.");
        return text(r);
    }
    // ---------- 0.5: arrangement over time, pictures, levels ----------
    std::vector<TrackTimeline> timelines(const std::vector<TrackSnap>& ts)
    {
        std::vector<TrackTimeline> out;
        for (auto& t : ts) {
            if (t.master() || t.tlBars <= 0 || !t.tl) continue;
            TrackTimeline x; x.id = t.slot; x.name = t.name.toStdString(); x.role = t.role; x.priority = t.priority;
            x.layer = t.layer; x.locked = t.locked; x.db = *t.tl;
            x.lufs.assign(t.tlLufs.begin(), t.tlLufs.end());
            auto lo = levelOffset.find(t.uid);
            x.offsetDb = lo != levelOffset.end() && lo->second > kFloorDb + 1 ? lo->second : 0.0f;
            out.push_back(x);
        }
        return out;
    }
    static const TrackSnap* findMaster(const std::vector<TrackSnap>& ts)
    {
        const TrackSnap* m = nullptr;
        for (auto& t : ts) if (t.master() && (!m || t.tlBars > m->tlBars)) m = &t;
        return m;
    }
    static int barsHeard(const std::vector<TrackSnap>& ts)
    {
        int n = 0; for (auto& t : ts) n = std::max(n, t.tlBars); return n;
    }
    // Sections to report and draw: the saved ones, else detected from the timelines.
    std::vector<SectionSpan> spansFor(const std::vector<TrackSnap>& ts, const std::vector<TrackTimeline>& tt, int bars, bool* detected)
    {
        std::vector<SectionSpan> sp;
        auto saved = readSections();
        if (!saved.empty()) {
            if (detected) *detected = false;
            for (auto& s : saved) {
                SectionSpan x; x.name = s.name.toStdString(); x.start = (int) s.start; x.end = std::max(x.start, (int) s.end);
                if (s.lead) for (auto& t : ts) if (t.uid == s.lead) x.leadId = t.slot;
                for (auto& t : tt) {
                    float pk = kFloorDb; for (int b = 0; b < kBars; ++b) pk = std::max(pk, barTotalDb(t.db, b));
                    int on = 0, len = 0;
                    for (int b = x.start - 1; b < std::min(std::min(x.end, kBars), std::max(bars, 1)); ++b, ++len) if (pk > kFloorDb + 1 && barTotalDb(t.db, b) > pk - 25.0f) ++on;
                    if (len && on * 2 >= len) x.active.push_back(t.name);
                }
                sp.push_back(x);
            }
            return sp;
        }
        if (detected) *detected = true;
        return detectSections(tt, bars);
    }
    static float meanLufs(const TrackSnap& m, int a, int b)
    {
        double e = 0; int c = 0;
        for (int x = std::max(1, a); x <= std::min(b, m.tlBars); ++x) { const float l = m.tlLufs[(size_t) x - 1]; if (l > -100) { e += std::pow(10.0, l / 10.0); ++c; } }
        return c ? (float) (10.0 * std::log10(e / c)) : kFloorDb;
    }

    struct FitResult { std::vector<TrackTimeline> tt, units; std::vector<int> unitOf; std::map<int, int> layerUnit; LevelFit fit; };
    // The stereo out against every track. Layered tracks play together, so the stereo out can't tell
    // them apart: each layer is fitted as one sound. units carry the fitted offsets.
    FitResult fitLevels(const std::vector<TrackSnap>& ts, const TrackSnap& m)
    {
        FitResult F;
        F.tt = timelines(ts);
        for (auto& x : F.tt) x.offsetDb = 0;
        auto& tt = F.tt; auto& units = F.units; auto& layerUnit = F.layerUnit;
        F.unitOf.assign(tt.size(), -1);
        for (size_t i = 0; i < tt.size(); ++i) {
            if (tt[i].layer > 0 && layerUnit.count(tt[i].layer)) {
                auto& u = units[(size_t) layerUnit[tt[i].layer]];
                for (int b = 0; b < kBars; ++b) for (int k = 0; k < kCoarse; ++k) {
                    const double s = (u.db[(size_t) b][(size_t) k] > kFloorDb + 1 ? std::pow(10.0, u.db[(size_t) b][(size_t) k] / 10.0) : 0.0)
                                   + (tt[i].db[(size_t) b][(size_t) k] > kFloorDb + 1 ? std::pow(10.0, tt[i].db[(size_t) b][(size_t) k] / 10.0) : 0.0);
                    u.db[(size_t) b][(size_t) k] = s > 0 ? (float) (10.0 * std::log10(s)) : kFloorDb;
                }
                if (u.name.find(" layer") == std::string::npos) u.name += " layer";
                F.unitOf[i] = layerUnit[tt[i].layer];
                continue;
            }
            if (tt[i].layer > 0) layerUnit[tt[i].layer] = (int) units.size();
            F.unitOf[i] = (int) units.size();
            units.push_back(tt[i]);
        }
        auto ufit = estimateLevels(units, *m.tl);
        F.fit.r2 = ufit.r2; F.fit.bars = ufit.bars; F.fit.excluded = ufit.excluded;
        for (size_t i = 0; i < tt.size(); ++i) F.fit.offsetDb.push_back(ufit.offsetDb[(size_t) F.unitOf[i]]);
        for (size_t u = 0; u < units.size(); ++u) units[u].offsetDb = ufit.offsetDb[u] > kFloorDb + 1 ? ufit.offsetDb[u] : -60.0f;
        return F;
    }

    var analyseArrangement(const var&)
    {
        auto ts = read(0);
        const int bars = barsHeard(ts);
        if (bars <= 0) return fail("No per-bar data yet. Use listen start, play the song in Logic from the top (the playhead must move), then listen stop.");
        float bpm = 0; int num = 4, den = 4;
        for (auto& t : ts) if (t.bpm > 0) { bpm = t.bpm; num = t.meterNum; den = t.meterDen; }
        var r = obj();
        if (bpm > 0) { put(r, "tempo_bpm", rnd(bpm, 1)); put(r, "meter", String(num) + "/" + String(den)); }
        put(r, "bars_heard", bars);
        if (bpm > 0) put(r, "seconds_heard", rnd(bars * num * (4.0 / den) * 60.0 / bpm, 0));

        const auto* m = findMaster(ts);
        // Level fit: the stereo out against every track.
        if (m && m->tlBars >= 8) {
            auto F = fitLevels(ts, *m);
            auto& tt = F.tt; auto& fit = F.fit; auto& layerUnit = F.layerUnit;
            var lf = obj();
            put(lf, "fit_quality_r2", rnd(fit.r2, 2)); put(lf, "bars_used", fit.bars);
            if (fit.excluded) put(lf, "cells_left_out", String(fit.excluded) + " bar/band cells where the stereo out has far more than the tracks explain (see mix_health)");
            juce::Array<var> lv;
            const bool good = fit.r2 >= 0.6f && fit.bars >= 8;
            for (size_t i = 0; i < tt.size(); ++i) {
                const TrackSnap* t = nullptr; for (auto& s : ts) if (s.slot == tt[i].id) t = &s;
                if (!t) continue;
                var o = obj(); put(o, "slot", t->slot); put(o, "track", t->name);
                const float off = fit.offsetDb[i];
                if (off <= kFloorDb + 1) put(o, "post_fader_offset_db", "not found in the stereo out (muted, routed elsewhere, or not playing)");
                else put(o, "post_fader_offset_db", rnd(off, 1));
                if (tt[i].layer > 0 && layerUnit.count(tt[i].layer)) {
                    int members = 0; for (auto& x : tt) if (x.layer == tt[i].layer) ++members;
                    if (members > 1) put(o, "fitted_as", "layer " + String(tt[i].layer) + " (one sound; its tracks share this offset)");
                }
                lv.add(o);
                if (good) levelOffset[t->uid] = off > kFloorDb + 1 ? off - t->trim.baseDb : kFloorDb; // keep the fader part; planned trims are added on top
            }
            put(lf, "tracks", lv);
            put(lf, "note", good ? "Each offset is how much louder (+) or quieter (-) the track is at the stereo out than at its CLEARING insert: fader, pan, sends and buses together (and CLEARING's own EQ and levels unless compare was on while listening). get_suggestions now plans from these."
                                 : "The fit is too loose to trust (effects, parallel buses or a short listen). Planning stays on insert levels. Listen longer over a section where most parts play.");
            put(r, "levels", lf);
            var ld = obj();
            float mx = kFloorDb; int mxBar = 0;
            for (int b = 1; b <= m->tlBars; ++b) if (m->tlLufs[(size_t) b - 1] > mx) { mx = m->tlLufs[(size_t) b - 1]; mxBar = b; }
            put(ld, "song_lufs_approx", rnd(meanLufs(*m, 1, m->tlBars), 1));
            put(ld, "loudest_bar", mxBar); put(ld, "loudest_bar_lufs_approx", rnd(mx, 1));
            put(r, "loudness", ld);
        } else {
            put(r, "levels", m ? "The stereo-out instance has heard under 8 bars. Listen longer." : "No CLEARING on the stereo out. Add one in Master mode for post-fader levels and loudness per bar; until then, levels are measured at each insert (before faders).");
        }

        auto tt = timelines(ts);
        bool detected = false;
        auto spans = spansFor(ts, tt, bars, &detected);
        auto desc = describeSections(tt, spans);
        juce::Array<var> sa;
        for (size_t i = 0; i < spans.size(); ++i) {
            auto& s = spans[i];
            var o = obj();
            put(o, "name", String(s.name)); put(o, "bars", String(s.start) + "-" + String(s.end));
            juce::Array<var> pl; for (auto& n : s.active) pl.add(String(n)); put(o, "playing", pl);
            if (i < desc.size() && desc[i].active) {
                put(o, "leader", String(desc[i].leader)); put(o, "leader_band", String(desc[i].band));
                put(o, "leader_share_of_band", rnd(desc[i].share, 2));
                juce::Array<var> ot;
                for (size_t k = 0; k < desc[i].others.size() && k < 3; ++k) { var q = obj(); put(q, "track", String(desc[i].others[k].first)); put(q, "share", rnd(desc[i].others[k].second, 2)); ot.add(q); }
                put(o, "also_in_that_band", ot);
            }
            if (m) put(o, "lufs_approx", rnd(meanLufs(*m, s.start, s.end), 1));
            sa.add(o);
        }
        put(r, "sections_source", detected ? "detected from who plays where (not saved; names are placeholders)" : "saved sections");
        put(r, "sections", sa);
        put(r, "note", detected ? String("These sections were found from the audio. Check them with the user (render_view arrangement shows the map), name them and give each a feel, then save with set_sections. Per-section level moves need saved sections.")
                                : String("render_view arrangement draws this; render_view masking with a section shows who shares which band there."));
        return text(r);
    }

    var setTrim(const var& args)
    {
        const int slot = (int) args["slot"];
        if (!hub.live(slot)) return fail("No live CLEARING instance in slot " + String(slot) + ".");
        const float db = juce::jlimit(-6.0f, 6.0f, (float) (double) args["db"]);
        pushUndo({ slot });
        auto tr = readTrim(slot);
        String where = "for the whole song";
        if (args.hasProperty("section")) {
            const int k = sectionIndex(args["section"]);
            if (k < 0) return fail("No section '" + args["section"].toString() + "'. Call get_brief for the section names.");
            tr.sectionDb[k] = db; where = "in " + readSections()[(size_t) k].name;
        } else tr.baseDb = db;
        writeTrim(slot, tr);
        return text("Planned level for slot " + String(slot) + " set to " + String(db > 0 ? "+" : "") + String(db, 1) + " dB " + where
                    + ". It glides in, adds to the Trim parameter, and undo reverts it.");
    }

    // ---------- pictures ----------
    static juce::Font font(float h, bool mono = false)
    {
        static juce::Typeface::Ptr semi = juce::Typeface::createSystemTypefaceFor(BinaryData::RajdhaniSemiBold_ttf, BinaryData::RajdhaniSemiBold_ttfSize);
        static juce::Typeface::Ptr mo = juce::Typeface::createSystemTypefaceFor(BinaryData::ShareTechMonoRegular_ttf, BinaryData::ShareTechMonoRegular_ttfSize);
        return juce::Font(juce::FontOptions(mono ? mo : semi).withHeight(h));
    }
    static juce::Colour heat(float x) // 0..1, dark -> teal -> amber
    {
        x = juce::jlimit(0.0f, 1.0f, x);
        const juce::Colour a(0xff10151c), b(0xff1f7f73), c(0xff35e0c0), d(0xffffb454);
        if (x < 0.5f) return a.interpolatedWith(b, x / 0.5f);
        if (x < 0.8f) return b.interpolatedWith(c, (x - 0.5f) / 0.3f);
        return c.interpolatedWith(d, (x - 0.8f) / 0.2f);
    }

    var renderView(const var& args)
    {
        using namespace juce;
        const String view = args.getProperty("view", "arrangement").toString();
        auto ts = read(0);
        const int bars = barsHeard(ts);
        if (bars <= 0) return fail("No per-bar data yet. Listen while Logic plays from the top, then try again.");
        auto tt = timelines(ts);
        bool detected = false;
        auto spans = spansFor(ts, tt, bars, &detected);
        const auto* m = findMaster(ts);
        const Colour bg(0xff0a0d12), line(0xff242c38), txt(0xffe7edf5), dim(0xff7e8b9e), acc(0xff35e0c0), warn(0xffffb454);
        const int W = 1400;
        std::unique_ptr<Image> img;
        String title, caption;

        auto sectionBand = [&](Graphics& g, Rectangle<int> r, int x0, float bw) {
            for (size_t i = 0; i < spans.size(); ++i) {
                auto& s = spans[i];
                const int a = x0 + (int) ((s.start - 1) * bw), b = x0 + (int) (s.end * bw);
                g.setColour(i % 2 ? Colour(0xff161c25) : Colour(0xff10151c)); g.fillRect(a, r.getY(), b - a, r.getHeight());
                g.setColour(acc); g.fillRect(a, r.getY(), 2, r.getHeight());
                g.setColour(txt); g.setFont(font(15));
                g.drawText(String(s.name), a + 6, r.getY(), b - a - 8, r.getHeight(), Justification::centredLeft, true);
            }
        };
        auto barAxis = [&](Graphics& g, int y, int x0, float bw) {
            g.setFont(font(12, true)); g.setColour(dim);
            const int step = bars > 96 ? 16 : bars > 48 ? 8 : 4;
            for (int b = 1; b <= bars; b += (b == 1 ? step - 1 : step)) g.drawText(String(b), x0 + (int) ((b - 1) * bw) - 10, y, 40, 14, Justification::centredLeft);
        };

        if (view == "arrangement") {
            std::vector<const TrackTimeline*> rows; for (auto& t : tt) rows.push_back(&t);
            const int rowH = 22, x0 = 210, top = 70, H = top + 30 + (int) rows.size() * rowH + (m ? 90 : 0) + 50;
            img = std::make_unique<Image>(Image::ARGB, W, H, true, SoftwareImageType());
            Graphics g(*img); g.fillAll(bg);
            const float bw = (float) (W - x0 - 20) / (float) bars;
            title = "Arrangement: who plays where";
            g.setColour(txt); g.setFont(font(22)); g.drawText(title, 20, 12, W - 40, 26, Justification::centredLeft);
            sectionBand(g, { 0, 44, W, 24 }, x0, bw);
            barAxis(g, top + 2, x0, bw);
            int y = top + 20;
            // each row: brightness = level at the stereo out where known (insert level + offset), relative to the loudest cell
            float ref = kFloorDb;
            for (auto* t : rows) for (int b = 0; b < bars; ++b) ref = std::max(ref, barTotalDb(t->db, b) + t->offsetDb);
            for (auto* t : rows) {
                g.setColour(txt); g.setFont(font(15));
                g.drawText(String(t->name), 20, y, x0 - 30, rowH, Justification::centredLeft, true);
                for (int b = 0; b < bars; ++b) {
                    const float d = barTotalDb(t->db, b);
                    if (d <= kFloorDb + 1) continue;
                    g.setColour(heat(1.0f - (ref - (d + t->offsetDb)) / 36.0f));
                    g.fillRect((float) x0 + b * bw + 0.5f, (float) y + 3, std::max(1.0f, bw - 1.0f), (float) rowH - 6);
                }
                g.setColour(line); g.drawHorizontalLine(y + rowH - 1, 20.0f, (float) W - 20);
                y += rowH;
            }
            if (m) {
                y += 14;
                g.setColour(txt); g.setFont(font(15)); g.drawText("Stereo out loudness", 20, y, x0 - 30, 20, Justification::centredLeft);
                float lo = 0, hi = -100;
                for (int b = 0; b < m->tlBars; ++b) { const float l = m->tlLufs[(size_t) b]; if (l > -100) { lo = std::min(lo, l); hi = std::max(hi, l); } }
                lo = std::max(std::min(lo, hi - 12), hi - 24); // the useful range, not the fade-out floor
                Path p; bool started = false;
                for (int b = 0; b < std::min(bars, m->tlBars); ++b) {
                    const float l = std::max(lo, m->tlLufs[(size_t) b]); if (m->tlLufs[(size_t) b] <= -100) continue;
                    const float px = x0 + (b + 0.5f) * bw, py = (float) y + 70 - (l - lo) / (hi - lo + 1e-3f) * 64;
                    if (!started) { p.startNewSubPath(px, py); started = true; } else p.lineTo(px, py);
                }
                g.setColour(warn); g.strokePath(p, PathStrokeType(2.0f));
                g.setColour(dim); g.setFont(font(12, true));
                g.drawText(String(hi, 1) + " LUFS", x0 - 90, y, 80, 14, Justification::centredRight);
                g.drawText(String(lo, 1), x0 - 90, y + 58, 80, 14, Justification::centredRight);
                y += 80;
            }
            g.setColour(dim); g.setFont(font(13));
            g.drawText(String("Brightness: level per bar") + (levelOffset.empty() ? " at each insert (before faders)." : " at the stereo out (insert + estimated fader offset).")
                       + (detected ? "  Sections detected, not saved." : ""), 20, H - 30, W - 40, 18, Justification::centredLeft);
            caption = "Rows are tracks, columns are bars " + String(1) + "-" + String(bars) + "; brighter = louder.";
        } else if (view == "masking") {
            int si = -1;
            if (args.hasProperty("section")) {
                const var sv = args["section"];
                if (sv.isInt() || sv.isDouble()) si = (int) sv - 1;
                else for (size_t i = 0; i < spans.size(); ++i) if (String(spans[i].name).equalsIgnoreCase(sv.toString().trim())) si = (int) i;
                if (si < 0 || si >= (int) spans.size()) return fail("No section '" + sv.toString() + "'. Use a name or number from analyse_arrangement.");
            }
            const int a = si >= 0 ? spans[(size_t) si].start : 1, b = si >= 0 ? spans[(size_t) si].end : bars;
            // energy per track per band in the span, at the stereo out where known
            std::vector<std::array<double, kCoarse>> e(tt.size());
            std::array<double, kCoarse> tot {};
            for (size_t t = 0; t < tt.size(); ++t)
                for (int x = a - 1; x < std::min(b, kBars); ++x)
                    for (int k = 0; k < kCoarse; ++k) {
                        const float d = tt[t].db[(size_t) x][(size_t) k];
                        if (d <= kFloorDb + 1) continue;
                        const double v = std::pow(10.0, (d + tt[t].offsetDb) / 10.0);
                        e[t][(size_t) k] += v; tot[(size_t) k] += v;
                    }
            std::vector<size_t> order;
            for (size_t t = 0; t < tt.size(); ++t) { double s = 0; for (auto v : e[t]) s += v; if (s > 0) order.push_back(t); }
            const int rowH = 30, x0 = 230, colW = (W - x0 - 30) / kCoarse, top = 92, H = top + (int) order.size() * rowH + 60;
            img = std::make_unique<Image>(Image::ARGB, W, H, true, SoftwareImageType());
            Graphics g(*img); g.fillAll(bg);
            title = "Who owns each band" + (si >= 0 ? " in " + String(spans[(size_t) si].name) : String(" (whole song)")) + "  - bars " + String(a) + "-" + String(b);
            g.setColour(txt); g.setFont(font(22)); g.drawText(title, 20, 12, W - 40, 26, Justification::centredLeft);
            int leadSlot = si >= 0 ? spans[(size_t) si].leadId : -1;
            for (int k = 0; k < kCoarse; ++k) {
                g.setColour(txt); g.setFont(font(16));
                g.drawText(String(coarseName(k)), x0 + k * colW, 48, colW, 20, Justification::centred);
                g.setColour(dim); g.setFont(font(12, true));
                g.drawText(hz(coarseEdge(k)) + "-" + hz(coarseEdge(k + 1)), x0 + k * colW, 68, colW, 16, Justification::centred);
            }
            int y = top;
            for (size_t t : order) {
                const bool lead = tt[t].id == leadSlot;
                g.setColour(lead ? acc : txt); g.setFont(font(16));
                g.drawText(String(tt[t].name) + (lead ? "  (leads)" : ""), 20, y, x0 - 30, rowH, Justification::centredLeft, true);
                for (int k = 0; k < kCoarse; ++k) {
                    const float sh = tot[(size_t) k] > 0 ? (float) (e[t][(size_t) k] / tot[(size_t) k]) : 0.0f;
                    auto cell = Rectangle<float>((float) (x0 + k * colW) + 2, (float) y + 2, (float) colW - 4, (float) rowH - 4);
                    g.setColour(heat(std::sqrt(sh))); g.fillRect(cell);
                    if (sh >= 0.05f) { g.setColour(sh > 0.45f ? bg : txt); g.setFont(font(14, true)); g.drawText(String(roundToInt(sh * 100)) + "%", cell, Justification::centred); }
                }
                y += rowH;
            }
            g.setColour(dim); g.setFont(font(13));
            g.drawText("Each cell: the track's share of that band's energy" + String(levelOffset.empty() ? " (insert levels, before faders)." : " at the stereo out."), 20, H - 34, W - 40, 18, Justification::centredLeft);
            caption = "Share of each band per track, bars " + String(a) + "-" + String(b) + ".";
        } else if (view == "spectrum") {
            // the stereo out's bands over time, or the sum of the tracks if no master instance
            Timeline src; for (auto& r : src) r.fill(kFloorDb);
            String from;
            if (m) { src = *m->tl; from = "the stereo out"; }
            else {
                from = "the sum of the tracks (no stereo-out instance)";
                for (int b = 0; b < bars; ++b) for (int k = 0; k < kCoarse; ++k) {
                    double s = 0; for (auto& t : tt) { const float d = t.db[(size_t) b][(size_t) k]; if (d > kFloorDb + 1) s += std::pow(10.0, (d + t.offsetDb) / 10.0); }
                    src[(size_t) b][(size_t) k] = s > 0 ? (float) (10.0 * std::log10(s)) : kFloorDb;
                }
            }
            const int x0 = 130, rowH = 44, top = 92, H = top + kCoarse * rowH + 60;
            img = std::make_unique<Image>(Image::ARGB, W, H, true, SoftwareImageType());
            Graphics g(*img); g.fillAll(bg);
            const float bw = (float) (W - x0 - 20) / (float) bars;
            title = "Spectrum over time: " + from;
            g.setColour(txt); g.setFont(font(22)); g.drawText(title, 20, 12, W - 40, 26, Justification::centredLeft);
            sectionBand(g, { 0, 44, W, 24 }, x0, bw);
            barAxis(g, 72, x0, bw);
            // each band relative to its own loudest bar, so movement shows (absolute balance is the masking view)
            for (int k = kCoarse - 1, row = 0; k >= 0; --k, ++row) {
                const int y = top + row * rowH;
                float pk = kFloorDb; for (int b = 0; b < bars; ++b) pk = std::max(pk, src[(size_t) b][(size_t) k]);
                g.setColour(txt); g.setFont(font(15)); g.drawText(String(coarseName(k)), 20, y, x0 - 30, rowH / 2, Justification::centredLeft);
                g.setColour(dim); g.setFont(font(11, true)); g.drawText(hz(coarseEdge(k)) + "-" + hz(coarseEdge(k + 1)), 20, y + rowH / 2 - 4, x0 - 20, 14, Justification::centredLeft);
                for (int b = 0; b < bars; ++b) {
                    const float d = src[(size_t) b][(size_t) k];
                    if (d <= kFloorDb + 1) continue;
                    g.setColour(heat(1.0f - (pk - d) / 30.0f));
                    g.fillRect((float) x0 + b * bw + 0.5f, (float) y + 2, std::max(1.0f, bw - 1.0f), (float) rowH - 4);
                }
            }
            g.setColour(dim); g.setFont(font(13));
            g.drawText("Each band is scaled to its own loudest bar (bright = that band at its fullest), so you see where each range swells and drops.", 20, H - 34, W - 40, 18, Justification::centredLeft);
            caption = "Seven bands over bars 1-" + String(bars) + ", from " + from + ".";
        } else if (view == "stereo") {
            if (!m || !m->health || m->health->bars <= 0) return fail("No mix-health data: put CLEARING on the stereo out in Master mode and listen first.");
            const auto& H = *m->health;
            const int hb = std::min(bars > 0 ? bars : H.bars, H.bars);
            const int x0 = 130, rowH = 44, top = 92, H2 = top + kHBands * rowH + 120;
            img = std::make_unique<Image>(Image::ARGB, W, H2, true, SoftwareImageType());
            Graphics g(*img); g.fillAll(bg);
            const float bw = (float) (W - x0 - 20) / (float) std::max(1, hb);
            title = "Stereo and phase on the stereo out";
            g.setColour(txt); g.setFont(font(22)); g.drawText(title, 20, 12, W - 40, 26, Justification::centredLeft);
            sectionBand(g, { 0, 44, W, 24 }, x0, bw);
            barAxis(g, 72, x0, bw);
            for (int k = kHBands - 1, row = 0; k >= 0; --k, ++row) {
                const int y = top + row * rowH;
                g.setColour(txt); g.setFont(font(15)); g.drawText(String(healthName(k)), 20, y, x0 - 30, rowH / 2, Justification::centredLeft);
                g.setColour(dim); g.setFont(font(11, true)); g.drawText(String((int) healthEdge(k)) + "-" + String((int) healthEdge(k + 1)) + " Hz", 20, y + rowH / 2 - 4, x0 - 20, 14, Justification::centredLeft);
                float pk = kFloorDb; for (int b = 0; b < hb; ++b) pk = std::max(pk, H.en[b][k]);
                for (int b = 0; b < hb; ++b) {
                    if (H.en[b][k] < pk - 40.0f) continue;
                    const float c = H.corr[b][k];
                    // 1 = mono (teal) ... 0 = wide (amber) ... negative = out of phase (red)
                    const Colour col = c >= 0.5f ? Colour(0xff1f7f73).interpolatedWith(acc, (c - 0.5f) / 0.5f)
                                     : c >= 0.0f ? warn.interpolatedWith(Colour(0xff1f7f73), c / 0.5f)
                                                 : Colour(0xffff5d6c).interpolatedWith(warn, 1.0f + c);
                    g.setColour(col.withAlpha(juce::jlimit(0.35f, 1.0f, 1.0f - (pk - H.en[b][k]) / 40.0f)));
                    g.fillRect((float) x0 + b * bw + 0.5f, (float) y + 2, std::max(1.0f, bw - 1.0f), (float) rowH - 4);
                }
            }
            // true peak per bar
            const int ty = top + kHBands * rowH + 16;
            g.setColour(txt); g.setFont(font(15)); g.drawText("True peak", 20, ty, x0 - 30, 20, Justification::centredLeft);
            g.setColour(Colour(0xff242c38)); g.drawHorizontalLine(ty + 10, (float) x0, (float) W - 20);
            for (int b = 0; b < hb; ++b) {
                const float t = H.tp[b]; if (t < -60) continue;
                const float hgt = juce::jlimit(1.0f, 50.0f, (t + 24.0f) / 24.0f * 50.0f);
                g.setColour(t > -1.0f ? Colour(0xffff5d6c) : t > -3.0f ? warn : Colour(0xff1f7f73));
                g.fillRect((float) x0 + b * bw + 0.5f, (float) ty + 60 - hgt, std::max(1.0f, bw - 1.0f), hgt);
            }
            g.setColour(dim); g.setFont(font(12, true));
            g.drawText("0 dBTP", x0 - 70, ty + 4, 60, 14, Justification::centredRight); g.drawText("-24", x0 - 70, ty + 50, 60, 14, Justification::centredRight);
            g.setFont(font(13));
            g.drawText("Teal = mono and solid, amber = wide, red = out of phase (cancels in mono). Faded = quiet. " + String(H.lufsI, 1) + " LUFS integrated, true peak " + String(H.tpMax, 1) + " dBTP.", 20, H2 - 30, W - 40, 18, Justification::centredLeft);
            caption = "Correlation per band and bar, plus true peak, from the stereo out.";
        } else if (view == "balance") {
            if (!lastMix) { var a = obj(); balanceMix(a); }
            if (!lastMix || lastMix->sections.empty()) return fail("No balance yet: run balance_mix after a listen.");
            const auto& mb = *lastMix;
            std::vector<std::string> names;
            for (auto& sb : mb.sections) for (auto& p : sb.parts) if (std::find(names.begin(), names.end(), p.name) == names.end()) names.push_back(p.name);
            const int x0 = 250, top = 104, rowH = 34, nS = (int) mb.sections.size();
            const int colW = std::max(60, (W - x0 - 20) / std::max(1, nS));
            const int H3 = top + (int) names.size() * rowH + 90;
            img = std::make_unique<Image>(Image::ARGB, W, H3, true, SoftwareImageType());
            Graphics g(*img); g.fillAll(bg);
            title = "Balance: each part against the " + String(mb.style) + " target (lead " + leadPlaceName(mb.lead) + ")";
            g.setColour(txt); g.setFont(font(22)); g.drawText(title, 20, 12, W - 40, 26, Justification::centredLeft);
            g.setColour(dim); g.setFont(font(14));
            g.drawText("Off target by " + String(mb.spreadBefore, 1) + " dB now, " + String(mb.spreadAfter, 1) + " dB after the proposed moves. Each cell: how far the part is from where the style puts it (+ = needs to come up).",
                       20, 40, W - 40, 20, Justification::centredLeft);
            for (int s = 0; s < nS; ++s) {
                const auto& sb = mb.sections[(size_t) s];
                g.setColour(txt); g.setFont(font(15));
                g.drawText(String(sb.name), x0 + s * colW, 70, colW, 18, Justification::centred, true);
                g.setColour(dim); g.setFont(font(11, true));
                g.drawText("bars " + String(sb.start) + "-" + String(sb.end), x0 + s * colW, 86, colW, 14, Justification::centred);
            }
            int y = top;
            for (auto& nm : names) {
                g.setColour(txt); g.setFont(font(16));
                g.drawText(String(nm), 20, y, x0 - 30, rowH, Justification::centredLeft, true);
                for (int s = 0; s < nS; ++s) {
                    const auto& sb = mb.sections[(size_t) s];
                    auto cell = Rectangle<float>((float) (x0 + s * colW) + 2, (float) y + 2, (float) colW - 4, (float) rowH - 4);
                    const PartLoudness* p = nullptr; for (auto& q : sb.parts) if (q.name == nm) p = &q;
                    if (!p) { g.setColour(Colour(0xff10151c)); g.fillRect(cell); continue; }
                    const float d = p->target - p->lufs; // + = needs to come up
                    const float a = std::fabs(d);
                    const Colour col = a < 1.0f ? Colour(0xff1f7f73) : a < 3.0f ? Colour(0xff6b7d3a) : a < 5.0f ? warn : Colour(0xffff5d6c);
                    g.setColour(col.withAlpha(0.85f)); g.fillRect(cell);
                    g.setColour(a < 3.0f ? txt : bg); g.setFont(font(15, true));
                    String t = (d > 0 ? "+" : "") + String(d, 1);
                    if (p->lead) t << "  LEAD";
                    if (p->locked) t << "  LOCK";
                    g.drawText(t, cell, Justification::centred);
                }
                y += rowH;
            }
            // the moves
            g.setColour(txt); g.setFont(font(15));
            String mvTxt = "Proposed: ";
            int shown = 0;
            for (auto& m : mb.moves) {
                if (shown++ >= 6) { mvTxt << " ..."; break; }
                mvTxt << String(m.name) << " " << (m.gainDb > 0 ? "+" : "") << String(m.gainDb, 1) << " dB"
                      << (m.section >= 0 && m.section < nS ? " in " + String(mb.sections[(size_t) m.section].name) : String()) << ";  ";
            }
            if (mb.moves.empty()) mvTxt = "No moves needed: the balance is close to the target.";
            g.drawFittedText(mvTxt, 20, H3 - 70, W - 40, 40, Justification::centredLeft, 2);
            g.setColour(dim); g.setFont(font(13));
            g.drawText("Teal: within 1 dB.  Olive: 1-3 dB.  Amber: 3-5 dB.  Red: over 5 dB off.  Grey: not playing.", 20, H3 - 26, W - 40, 18, Justification::centredLeft);
            caption = "Distance from the target balance per part and section, and the proposed moves.";
        } else return fail("view must be arrangement, masking, spectrum, stereo or balance");

        MemoryOutputStream png;
        PNGImageFormat().writeImageToStream(*img, png);
        auto dir = File::getSpecialLocation(File::userMusicDirectory).getChildFile("CLEARING Views");
        dir.createDirectory();
        auto f = dir.getChildFile(view + "-" + Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".png");
        f.replaceWithData(png.getData(), png.getDataSize());
        var r = obj(), im = obj(), tx = obj();
        put(im, "type", "image"); put(im, "data", Base64::toBase64(png.getData(), png.getDataSize())); put(im, "mimeType", "image/png");
        put(tx, "type", "text"); put(tx, "text", title + ". " + caption + " Saved to " + f.getFullPathName());
        put(r, "content", Array<var> { tx, im });
        return r;
    }

    // ---------- 0.8: balance the levels ----------
    std::unique_ptr<MixBalance> lastMix;
    String lastMixStyle = "rock", lastMixLead = "blended";

    static LeadPlace leadPlaceOf(const String& s)
    {
        const String x = s.toLowerCase();
        if (x.contains("top") || x.contains("forward") || x.contains("front")) return LeadPlace::OnTop;
        if (x.contains("inside") || x.contains("buried") || x.contains("in the wall")) return LeadPlace::Inside;
        return LeadPlace::Blended;
    }
    static String leadPlaceName(LeadPlace p) { return p == LeadPlace::OnTop ? "on top" : p == LeadPlace::Inside ? "inside the wall" : "blended"; }

    // Real levels need the stereo-out fit; run it if the last listen hasn't been analysed yet.
    bool ensureOffsets(const std::vector<TrackSnap>& ts, String& how)
    {
        const auto* m = findMaster(ts);
        if (!m || m->tlBars < 8) { how = "insert levels (no CLEARING on the Stereo Out in Master mode, so faders, pans and buses aren't included: treat the moves as relative)"; return false; }
        bool have = false;
        for (auto& t : ts) if (!t.master() && levelOffset.count(t.uid)) have = true;
        if (!have) {
            auto F = fitLevels(ts, *m);
            if (F.fit.r2 < 0.6f) { how = "insert levels (the stereo-out comparison was too loose to trust, fit " + String(F.fit.r2, 2) + ")"; return false; }
            for (size_t i = 0; i < F.tt.size(); ++i)
                for (auto& t : ts) if (t.slot == F.tt[i].id) levelOffset[t.uid] = F.fit.offsetDb[i] > kFloorDb + 1 ? F.fit.offsetDb[i] - t.trim.baseDb : kFloorDb;
        }
        how = "levels at the stereo out (after faders, pans and buses)";
        return true;
    }

    var balanceMix(const var& args)
    {
        auto ts = read(0);
        if (barsHeard(ts) <= 0) return fail("No per-bar data yet. Ideally: compare on with matched false, listen start, play the whole song, listen stop.");
        if (args.hasProperty("style")) lastMixStyle = args["style"].toString();
        if (args.hasProperty("lead")) lastMixLead = args["lead"].toString();
        const float maxMove = juce::jlimit(1.0f, 6.0f, (float) (double) args.getProperty("max_db", 6.0));
        String how;
        ensureOffsets(ts, how);
        auto tt = timelines(ts);
        std::vector<SectionSpan> spans;
        auto saved = readSections();
        for (auto& s : saved) {
            SectionSpan sp; sp.name = s.name.toStdString(); sp.start = (int) s.start; sp.end = (int) s.end;
            if (s.lead) for (auto& t : ts) if (t.uid == s.lead) sp.leadId = t.slot;
            spans.push_back(sp);
        }
        const LeadPlace lp = leadPlaceOf(lastMixLead);
        lastMix = std::make_unique<MixBalance>(planMixBalance(tt, spans, lastMixStyle.toStdString(), lp, maxMove));
        const auto& mb = *lastMix;

        var r = obj();
        put(r, "style", String(mb.style)); put(r, "lead", leadPlaceName(lp));
        put(r, "measured_from", how);
        put(r, "distance_from_target_db", String(mb.spreadBefore, 1) + " now, " + String(mb.spreadAfter, 1) + " after these moves (RMS across parts and sections)");
        juce::Array<var> secs;
        for (auto& sb : mb.sections) {
            var so = obj(); put(so, "name", String(sb.name)); put(so, "bars", String(sb.start) + "-" + String(sb.end));
            const PartLoudness* ld = nullptr; for (auto& p : sb.parts) if (p.lead) ld = &p;
            juce::Array<var> parts;
            for (auto& p : sb.parts) {
                var po = obj(); put(po, "part", String(p.name)); put(po, "group", String(mixGroupName(p.group)));
                if (ld) { put(po, "now_vs_lead_db", rnd(p.lufs - ld->lufs, 1)); put(po, "target_vs_lead_db", rnd(p.target - ld->target, 1)); }
                put(po, "move_db", rnd(p.move, 1));
                if (p.lead) put(po, "lead", true);
                if (p.locked) put(po, "locked", true);
                parts.add(po);
            }
            put(so, "parts", parts);
            secs.add(so);
        }
        put(r, "sections", secs);
        juce::Array<var> mv;
        for (auto& m : mb.moves) {
            const int id = nextId++;
            for (int member : m.members) {
                Sug s; s.id = id; s.slot = member; s.balance = true; s.gainDb = m.gainDb; s.section = m.section;
                for (auto& t : ts) if (t.slot == member) s.track = t.name;
                last.push_back(s);
            }
            var o = obj(); put(o, "id", id); put(o, "track", String(m.name));
            put(o, "move", String(m.gainDb > 0 ? "+" : "") + String(m.gainDb, 1) + " dB" + (m.section >= 0 && m.section < (int) saved.size() ? " in " + saved[(size_t) m.section].name : String(" for the whole song")));
            put(o, "confidence", rnd(m.confidence, 2)); put(o, "reason", String(m.reason));
            mv.add(o);
        }
        put(r, "moves", mv);
        // gain staging
        juce::Array<var> gs;
        for (auto& t : ts) {
            if (t.master() || t.activeSec <= 0) continue;
            if (t.capPeakDb > -0.5f) gs.add(t.name + " peaks at " + String(t.capPeakDb, 1) + " dBFS at its CLEARING insert. Inside Logic that's safe (floating point), but plug-ins that model analogue gear react to level: if it's meant to sit there, fine; if not, pull it down earlier in the chain (clip gain or the first plug-in's input).");
            if (t.capRmsDb > -119 && t.capRmsDb < -45.0f) gs.add(t.name + " is very quiet at its insert (about " + String(t.capRmsDb, 0) + " dBFS on average). Raise it at the source so the plug-ins after it see a healthy level (around -18 dBFS average is the classic target).");
            if (auto lo = levelOffset.find(t.uid); lo != levelOffset.end() && lo->second > kFloorDb + 1 && lo->second < -10.0f)
                gs.add(t.name + " reaches the stereo out " + String(-lo->second, 1) + " dB below its insert: its fader (or bus) is pulled far down. Lower the source instead (clip gain, or the instrument's output) and bring the fader back towards 0, so there's room to ride it.");
        }
        if (const auto* m = findMaster(ts); m && m->health && m->health->bars > 0 && m->health->tpMax > -1.0f)
            gs.add("The stereo out peaks at " + String(m->health->tpMax, 1) + " dBTP. After balancing, keep the mix peaking below -1 dBTP (pull the master fader down if needed) and leave limiting to mastering.");
        put(r, "gain_staging", gs);
        juce::Array<var> notes; for (auto& n : mb.notes) notes.add(String(n));
        put(r, "notes", notes);
        put(r, "next", mv.isEmpty() ? String("The balance is already close to the target for this style.")
                                    : String("Explain the moves in musical terms, apply the ones the user agrees to (apply with their ids: they go into each track's Trim), then listen again with compare on (matched false) and run balance_mix to confirm. render_view balance shows the table."));
        return text(r);
    }

    // ---------- 0.7: where things stand, for any LLM's first call ----------
    var status()
    {
        var r = obj();
        put(r, "clearing_mcp_version", kVersion);
        put(r, "hub", hubOk ? String("connected (") + hub.name() + ")" : String("not available"));
        if (!hubOk) { put(r, "next", "The shared hub couldn't be opened. Make sure CLEARING is installed and Logic is running with it on some tracks."); return text(r); }
        auto ts = read(0);
        int tracks = 0; String master;
        juce::Array<var> names;
        for (auto& t : ts) { if (t.master()) master = t.name; else { ++tracks; names.add(t.name); } }
        put(r, "tracks_with_clearing", tracks); put(r, "tracks", names);
        put(r, "stereo_out_instance", master.isNotEmpty() ? master : String("none"));
        put(r, "listening", hub.get()->listening.load() != 0);
        put(r, "compare", hub.get()->bypassAll.load() != 0);
        int bars = 0; for (auto& t : ts) bars = std::max(bars, t.tlBars);
        put(r, "bars_heard_last_listen", bars);
        juce::StringArray next;
        if (tracks == 0) next.add("No CLEARING instances are live. Ask the user to open Logic with CLEARING on the tracks to include (Audio FX > Audio Units > Rob Cawley > CLEARING). If it's on tracks but none show, quit and reopen Logic: an update needs a fresh start.");
        else {
            if (master.isEmpty()) next.add("Suggest one CLEARING on the Stereo Out with Mode set to Master out: it gives real levels, loudness and mix health.");
            next.add("list_tracks, then ask the user what each part is, which are layered, and how the arrangement should feel (get_brief / set_brief / set_sections).");
            next.add(bars > 0 ? "There's per-bar data: analyse_arrangement, mix_health, get_suggestions." : "listen start, have the user play from the top, listen stop.");
        }
        put(r, "next", next.joinIntoString(" "));
        return text(r);
    }

    // ---------- 0.6: harmonics by hand, mix health ----------
    var setHarmonics(const var& args)
    {
        const int slot = (int) args["slot"];
        if (!hub.live(slot)) return fail("No live CLEARING instance in slot " + String(slot) + ".");
        const String kind = args.getProperty("kind", "bass").toString();
        const bool on = (bool) args.getProperty("on", true);
        pushUndo({ slot });
        auto hp = readHarm(slot);
        auto f = [&](const char* k, float d) { return args.hasProperty(k) ? (float) (double) args[k] : d; };
        if (kind == "bass") {
            hp.bassOn = on ? 1 : 0;
            if (on) {
                hp.bassLo = juce::jlimit(20.0f, 150.0f, f("lo_hz", hp.bassLo)); hp.bassHi = juce::jlimit(hp.bassLo + 10.0f, 200.0f, f("hi_hz", hp.bassHi));
                hp.bassHp = juce::jlimit(hp.bassHi, 400.0f, f("hp_hz", std::max(hp.bassHp, hp.bassHi * 1.05f)));
                hp.bassLp = juce::jlimit(hp.bassHp + 50.0f, 2000.0f, f("lp_hz", hp.bassLp));
                hp.bassMixDb = juce::jlimit(-30.0f, 0.0f, f("mix_db", hp.bassMixDb)); hp.bassDrive = juce::jlimit(0.0f, 1.0f, f("drive", hp.bassDrive));
                hp.subDb = juce::jlimit(-6.0f, 0.0f, f("sub_db", hp.subDb));
            }
        } else if (kind == "presence") {
            hp.presOn = on ? 1 : 0;
            if (on) {
                hp.presFreq = juce::jlimit(300.0f, 8000.0f, f("freq_hz", hp.presFreq)); hp.presQ = juce::jlimit(0.5f, 4.0f, f("q", hp.presQ));
                hp.presMixDb = juce::jlimit(-30.0f, 0.0f, f("mix_db", hp.presMixDb)); hp.presDrive = juce::jlimit(0.0f, 1.0f, f("drive", hp.presDrive));
            }
        } else return fail("kind must be bass or presence");
        writeHarm(slot, hp);
        var r = obj(); put(r, "slot", slot); put(r, "harmonics", harmVar(hp));
        put(r, "note", "Sent; it glides in, follows Amount, and COMPARE switches it off. undo reverts it.");
        return text(r);
    }

    var mixHealth(const var&)
    {
        auto ts = read(0);
        const auto* m = findMaster(ts);
        if (!m) return fail("No CLEARING on the stereo out. Insert one on the Stereo Out, set its Mode to Master out, then listen while the song plays.");
        if (!m->health || m->health->bars <= 0) return fail("The stereo-out instance hasn't heard a listen yet. Use listen start, play from the top, listen stop.");
        const auto& H = *m->health;
        var r = obj();
        var loud = obj();
        put(loud, "integrated_lufs", rnd(H.lufsI, 1)); put(loud, "loudest_3s_lufs", rnd(H.lufsS, 1));
        put(loud, "true_peak_dbtp", rnd(H.tpMax, 1)); put(loud, "sample_peak_dbfs", rnd(H.spMax, 1));
        if (H.lufsI > -100 && H.tpMax > -100) put(loud, "peak_to_loudness_db", rnd(H.tpMax - H.lufsI, 1));
        put(r, "loudness", loud);
        juce::Array<var> warn;
        if (H.tpMax > -1.0f) warn.add("True peak reaches " + String(H.tpMax, 1) + " dBTP. Keep at least 1 dB below 0 (streaming encoders add peaks); leave the limiting to mastering or lower the mix.");
        // per section: energy-weighted correlation and mono loss per band
        auto tt = timelines(ts);
        bool detected = false;
        auto spans = spansFor(ts, tt, H.bars, &detected);
        if (spans.empty()) { SectionSpan s; s.name = "the whole song"; s.start = 1; s.end = H.bars; spans.push_back(s); }
        juce::Array<var> secs;
        for (auto& s : spans) {
            const int a = std::max(1, s.start), b = std::min(s.end, H.bars);
            if (b < a) continue;
            var o = obj(); put(o, "name", String(s.name)); put(o, "bars", String(a) + "-" + String(b));
            var bands = obj();
            float peakEn = kFloorDb;
            for (int x = a; x <= b; ++x) for (int k = 0; k < kHBands; ++k) peakEn = std::max(peakEn, H.en[x - 1][k]);
            for (int k = 0; k < kHBands; ++k) {
                double w = 0, c = 0, ml = 0; float worst = 1;
                for (int x = a; x <= b; ++x) {
                    const float e = H.en[x - 1][k];
                    if (e < peakEn - 40.0f) continue;
                    const double wt = std::pow(10.0, e / 10.0);
                    w += wt; c += wt * H.corr[x - 1][k]; ml += wt * H.mono[x - 1][k]; worst = std::min(worst, H.corr[x - 1][k]);
                }
                if (w <= 0) continue;
                var bo = obj(); put(bo, "correlation", rnd(c / w, 2)); put(bo, "mono_db", rnd(ml / w, 1)); put(bo, "worst_bar_correlation", rnd(worst, 2));
                put(bands, healthName(k), bo);
                const String where = String(healthName(k)) + " (" + String((int) healthEdge(k)) + "-" + String((int) healthEdge(k + 1)) + " Hz) in " + String(s.name);
                if (k <= 1 && c / w < 0.8) warn.add("The " + where + " is wide or partly out of phase (correlation " + String(c / w, 2) + "). Low end should be near mono: centre the bass and kick, narrow any stereo synth below ~150 Hz.");
                else if (ml / w < -3.0) warn.add("The " + where + " loses " + String(-ml / w, 1) + " dB when folded to mono (phones, club PAs, some speakers). Look for stereo wideners or phase-flipped doubles there.");
            }
            put(o, "bands", bands);
            secs.add(o);
        }
        put(r, "sections", secs);
        // what the stereo out shows that the tracks don't
        juce::Array<var> found;
        if (m->tlBars >= 8 && !tt.empty()) {
            auto F = fitLevels(ts, *m);
            for (auto& fdg : findResiduals(F.units, *m->tl)) {
                var o = obj(); put(o, "kind", String(fdg.kind)); put(o, "bars", String(fdg.start) + "-" + String(fdg.end));
                put(o, "band", String(coarseName(fdg.band))); put(o, "db", rnd(fdg.db, 1)); put(o, "text", String(fdg.text));
                found.add(o);
                warn.add(String(fdg.text));
            }
            if (F.fit.r2 < 0.6f) put(r, "fit_note", "Few of the parts carry CLEARING, so the comparison with the stereo out is rough (fit " + String(F.fit.r2, 2) + "). Put CLEARING on every track that plays for a firm read.");
        }
        put(r, "findings", found);
        put(r, "warnings", warn);
        if (warn.isEmpty()) put(r, "note", "Nothing alarming: low end centred, nothing cancelling in mono, peaks clear of 0 dBTP.");
        put(r, "how_measured", "Per band and bar while listening, on the stereo out. correlation: 1 = mono, 0 = unrelated sides, negative = out of phase. mono_db: what's kept when L and R are summed (0 = all, -3 = uncorrelated). Loudness is BS.1770-style gated integrated loudness; true peak is 4x oversampled.");
        return text(r);
    }

    Hub hub;
    String client;
    bool hubOk = false;
    std::vector<Sug> last;
    int nextId = 1;
    std::vector<std::vector<UndoEntry>> undoStack;
    std::map<uint64_t, float> levelOffset; // instance uid -> estimated insert-to-stereo-out offset (dB)
};

} // namespace

// The path of this executable, to notice when an install replaces it.
static std::string selfPath()
{
    char buf[4096]; uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return {};
    char real[4096];
    return realpath(buf, real) ? std::string(real) : std::string(buf);
}
static bool fileId(const std::string& p, uint64_t& ino, int64_t& mt, int64_t& size)
{
    struct stat st {};
    if (p.empty() || stat(p.c_str(), &st) != 0) return false;
    ino = (uint64_t) st.st_ino; mt = (int64_t) st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec; size = (int64_t) st.st_size;
    return (st.st_mode & S_IXUSR) != 0;
}

int main(int argc, char** argv)
{
    std::ios::sync_with_stdio(false);
    Server server;
    const std::string self = selfPath();
    uint64_t ino0 = 0; int64_t mt0 = 0, sz0 = 0;
    fileId(self, ino0, mt0, sz0);
    // Restarted after an update: the client already did the handshake; tell it the tools may have changed.
    if (const char* re = std::getenv("CLEARING_MCP_REEXEC"); re && *re) {
        server.setClient(String::fromUTF8(std::getenv("CLEARING_MCP_CLIENT") ? std::getenv("CLEARING_MCP_CLIENT") : ""));
        server.logAction(String("CLEARING connection updated to ") + kVersion);
        std::cout << "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n";
        std::cout.flush();
    }
    // Heartbeat (the window shows the LLM as connected) and update watch, once a second.
    std::atomic<bool> running { true }, updateReady { false };
    std::thread watcher([&] {
        uint64_t pino = ino0; int64_t pmt = mt0, psz = sz0; int stable = 0;
        while (running.load()) {
            server.beat();
            uint64_t ino; int64_t mt, sz;
            if (fileId(self, ino, mt, sz) && (ino != ino0 || mt != mt0 || sz != sz0)) {
                // changed: wait until it has stayed the same for 3 s (install copies, then signs)
                if (ino == pino && mt == pmt && sz == psz) { if (++stable >= 3) updateReady.store(true); }
                else stable = 0;
                pino = ino; pmt = mt; psz = sz;
            }
            for (int i = 0; i < 10 && running.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::string buf;
    char chunk[65536];
    bool eof = false;
    while (!eof) {
        // between requests: if an update has landed, become it (stdin/stdout carry over)
        if (updateReady.load() && buf.find('\n') == std::string::npos && buf.empty()) {
            running.store(false); watcher.join();
            setenv("CLEARING_MCP_REEXEC", "1", 1);
            setenv("CLEARING_MCP_CLIENT", server.clientName().toRawUTF8(), 1);
            execv(self.c_str(), argv);
            // exec failed: carry on as we are
            running.store(true); updateReady.store(false);
            fileId(self, ino0, mt0, sz0);
            watcher = std::thread([&] { while (running.load()) { server.beat(); std::this_thread::sleep_for(std::chrono::seconds(1)); } });
        }
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            var req;
            const auto res = juce::JSON::parse(String::fromUTF8(line.c_str()), req);
            var resp;
            if (res.failed() || !req.isObject()) {
                resp = obj(); put(resp, "jsonrpc", "2.0"); put(resp, "id", var());
                var err = obj(); put(err, "code", -32700); put(err, "message", "Parse error"); put(resp, "error", err);
            } else {
                resp = server.handle(req);
            }
            if (!resp.isVoid()) {
                std::cout << juce::JSON::toString(resp, true).toStdString() << "\n";
                std::cout.flush();
            }
        }
        pollfd pf { 0, POLLIN, 0 };
        const int pr = poll(&pf, 1, 500);
        if (pr > 0) {
            const ssize_t n = ::read(0, chunk, sizeof chunk);
            if (n <= 0) eof = true; else buf.append(chunk, (size_t) n);
        }
    }
    running.store(false);
    if (watcher.joinable()) watcher.join();
    return 0;
}
