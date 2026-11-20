#!/usr/bin/env python3
"""Drives clearing-mcp against FakeSession the way an LLM would. Usage: mcp_session_test.py <build dir>"""
import json, os, subprocess, sys, time

build = sys.argv[1]
env = dict(os.environ, CLEARING_HUB="/clearing_mcp_test")
fails = []
def check(ok, msg):
    print(("ok:   " if ok else "FAIL: ") + msg, flush=True)
    if not ok: fails.append(msg)

stopf = f"/tmp/clearing_fake_stop_{os.getpid()}"
if os.path.exists(stopf): os.remove(stopf)
sess = subprocess.Popen([f"{build}/ClearingFakeSession_artefacts/Release/ClearingFakeSession", "240", stopf], env=env,
                        stdout=subprocess.PIPE, text=True)
assert "session up" in sess.stdout.readline()
time.sleep(1.0)
mcp = subprocess.Popen([f"{build}/clearing-mcp"], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
rid = 0
def rpc(method, params=None, notify=False):
    global rid
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None: msg["params"] = params
    if not notify: rid += 1; msg["id"] = rid
    mcp.stdin.write(json.dumps(msg) + "\n"); mcp.stdin.flush()
    if notify: return None
    resp = json.loads(mcp.stdout.readline())
    assert resp.get("id") == rid, resp
    return resp
def tool(name, args=None, raw=False):
    r = rpc("tools/call", {"name": name, "arguments": args or {}})["result"]
    if raw: return r
    body = r["content"][0]["text"]
    try: body = json.loads(body)
    except ValueError: pass
    return body, r.get("isError", False)

init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
check(init["result"]["serverInfo"]["name"] == "clearing" and "instructions" in init["result"], "initialize")
rpc("notifications/initialized", notify=True)
names = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
check(set(names) >= {"list_tracks", "set_track", "listen", "get_suggestions", "apply", "undo", "compare", "set_amount", "set_movement",
                     "clear_eq", "list_roles", "get_brief", "set_brief", "set_sections", "set_section_lead", "add_dip", "remove_dip", "analyse_arrangement", "render_view", "set_trim", "mix_health", "set_harmonics", "status", "balance_mix"}, "tools/list: " + ", ".join(names))
check("arrangement" in init["result"]["instructions"] and "feel" in init["result"]["instructions"], "instructions tell the LLM to ask about sections and feel")

tracks, _ = tool("list_tracks")
allby = {t["name"]: t for t in tracks["tracks"]}
check("Stereo Out" in allby and "master" in allby["Stereo Out"].get("mode", ""), "the stereo-out instance shows as the master")
tracks["tracks"] = [t for t in tracks["tracks"] if t["name"] != "Stereo Out"]
by = {t["name"]: t for t in tracks["tracks"]}
check(set(by) == {"Kick", "Bass DI", "Lead Vox", "Gtr", "Rhodes"}, "list_tracks sees the session with Logic's track names: " + ", ".join(by))
check(all(t["name_from_logic"] for t in tracks["tracks"]), "names flagged as coming from Logic")
print("  spectrum of Bass DI:", by["Bass DI"]["spectrum"]["description"])

# The LLM asks the user and sets roles from the answers (forgiving role words).
answers = {"Kick": "kick", "Bass DI": "bass", "Lead Vox": "vocal", "Gtr": "guitar", "Rhodes": "keys"}
for n, role in answers.items():
    r, err = tool("set_track", {"slot": by[n]["slot"], "role": role})
    check(not err, f"set_track {n} -> {r['role'] if not err else r}")
_, err = tool("set_track", {"slot": by["Gtr"]["slot"], "name": "Rhythm Gtr"})
_, err2 = tool("set_track", {"slot": 99, "role": "kick"})
check(err2, "bad slot is refused with an error")
want = {"Kick": "Kick", "Bass DI": "Bass", "Lead Vox": "Lead Vocal", "Rhythm Gtr": "Electric Gtr", "Rhodes": "Piano / Keys"}
t0 = time.time(); roles = {}
while time.time() - t0 < 1.5:
    tracks, _ = tool("list_tracks")
    tracks["tracks"] = [t for t in tracks["tracks"] if t["name"] != "Stereo Out"]
    roles = {t["name"]: t["role"] for t in tracks["tracks"]}
    if roles == want: break
    time.sleep(0.05)
check(roles == want, f"all roles and the rename visible after {time.time() - t0:.2f} s: {roles}")
check(not [t for t in tracks["tracks"] if t["name"] == "Rhythm Gtr"][0]["name_from_logic"], "renamed track no longer flagged as Logic's name")

# Arrangement: the LLM records what the user said about sections and how they should feel.
b, err = tool("get_brief")
check(not err and b["brief"] == "" and "note" in b, "get_brief on a fresh session asks for the arrangement")
tool("set_brief", {"text": "Ballad. Verse intimate, vocal exposed. Chorus huge, everything blooms."})
r, err = tool("set_sections", {"sections": [
    {"name": "Verse", "start_bar": 1, "end_bar": 16, "feel": "intimate, vocal exposed", "amount": 1.0, "movement": 1.0},
    {"name": "Chorus", "start_bar": 17, "end_bar": 32, "feel": "huge, everything blooms", "amount": 0.6, "movement": 0.4}]})
check(not err and r["sections_set"] == 2, "set_sections")
_, err = tool("set_sections", {"sections": [{"name": "Bad", "start_bar": 9, "end_bar": 2}]})
check(err, "a section that ends before it starts is refused")
b, _ = tool("get_brief")
check(b["brief"].startswith("Ballad") and [s["name"] for s in b["sections"]] == ["Verse", "Chorus"] and b["sections"][1]["movement"] == 0.4, "get_brief returns the brief and sections")
# Layers: keys and guitar as one sound, then quick successive edits on the same track.
tool("set_track", {"slot": by["Rhodes"]["slot"], "layer": 3})
tool("set_track", {"slot": by["Rhodes"]["slot"], "priority": 0.5})
tool("set_track", {"slot": by["Gtr"]["slot"], "layer": 3})
t0 = time.time()
while time.time() - t0 < 1.5:
    tr, _ = tool("list_tracks"); tr["tracks"] = [t for t in tr["tracks"] if t["name"] != "Stereo Out"]; lay = {t["name"]: t["layer"] for t in tr["tracks"]}; pr = {t["name"]: t["priority"] for t in tr["tracks"]}
    if lay.get("Rhodes") == 3 and lay.get("Rhythm Gtr") == 3 and pr.get("Rhodes") == 0.5: break
    time.sleep(0.05)
check(lay.get("Rhodes") == 3 and lay.get("Rhythm Gtr") == 3 and pr.get("Rhodes") == 0.5, f"layers set and kept through a quick follow-up edit: {lay}")

tool("listen", {"action": "start"})
time.sleep(9)
st, _ = tool("listen", {"action": "stop"})
caps = [t["captured_seconds"] for t in st["tracks"] if t["name"] != "Stereo Out"]
check(min(caps) >= 8, f"captured seconds per track: {caps}")

sug, err = tool("get_suggestions")
check(not err and len(sug["suggestions"]) > 0, f"{len(sug['suggestions'])} suggestions ({sug['style']})")
for s in sug["suggestions"]: print(f"  #{s['id']} {s['track']}: {s['move']}  conf {s['confidence']}")
for n in sug.get("notes", []): print("  note:", n)
check("notes" in sug, "suggestions come with notes")
dips = [s for s in sug["suggestions"] if s["dynamic"]]
check(len(dips) > 0, f"{len(dips)} focus-following dip(s) proposed")
# Section lead: the keys lead the Chorus; the others make room for them there.
r, err = tool("set_section_lead", {"section": "Chorus", "slot": by["Rhodes"]["slot"]})
check(not err, "set_section_lead")
b, _ = tool("get_brief")
check(b["sections"][1].get("lead") == "Rhodes", "the section shows its lead")
sug2, _ = tool("get_suggestions")
led = [s for s in sug2["suggestions"] if s.get("sections") == "Chorus" and s["dynamic"]]
for s in led: print(f"  chorus: #{s['id']} {s['track']}: {s['move']}")
check(len(led) > 0, f"{len(led)} dip(s) limited to the Chorus, led by the keys")
check(not any(s["track"] in ("Rhodes", "Rhythm Gtr") and not s["dynamic"] for s in sug2["suggestions"]), "the section lead (a layer) gets no static cuts")
tool("set_section_lead", {"section": "Chorus", "slot": -1})
sug, _ = tool("get_suggestions")
lay_moves = {}
for s in sug["suggestions"]:
    if s["track"] in ("Rhodes", "Rhythm Gtr"): lay_moves.setdefault(s["track"], []).append(s["move"])
check(lay_moves.get("Rhodes") == lay_moves.get("Rhythm Gtr"), f"the layered pair gets identical moves: {lay_moves}")
bass_ids = [s["id"] for s in sug["suggestions"] if s["track"] == "Bass DI"]
check(any("Hz" in s["move"] and s["track"] == "Bass DI" for s in sug["suggestions"]), "bass gets a cut")

r, err = tool("apply", {"ids": bass_ids})
check(not err and len(r["applied"]) == len(bass_ids), "apply chosen ids only")
time.sleep(0.6)
tracks, _ = tool("list_tracks")
eq = {t["name"]: t["applied_eq"] for t in tracks["tracks"]}
check(len(eq["Bass DI"]) == len(bass_ids) and all(len(v) == 0 for k, v in eq.items() if k != "Bass DI"), f"only bass carries EQ: {eq['Bass DI']}")
tool("undo"); time.sleep(0.4)
tracks, _ = tool("list_tracks")
check(all(len(t["applied_eq"]) == 0 for t in tracks["tracks"]), "undo returns every track to flat")
tool("apply"); time.sleep(0.4)
tool("set_amount", {"amount": 0.5}); tool("set_movement", {"movement": 0.7}); tool("compare", {"on": True})
t0 = time.time()
while time.time() - t0 < 1.5:
    t2, _ = tool("list_tracks")
    if all(t["amount"] == 0.5 and t["movement"] == 0.7 for t in t2["tracks"]): break
    time.sleep(0.05)
check(all(t["amount"] == 0.5 and t["movement"] == 0.7 for t in t2["tracks"]) and t2["compare_bypass"], "amount, movement and compare reach every track")
follows = [e.get("follows") for t in t2["tracks"] for e in t["applied_eq"] if "dip" in e["type"]]
check(len(follows) > 0 and all(f == "Lead Vox" for f in follows), f"applied dips follow the focus: {follows}")
tool("compare", {"on": False})
# Hand-placed dip, kept through a later apply, then removed.
r, err = tool("add_dip", {"leader_slot": by["Bass DI"]["slot"], "follower_slots": [by["Rhodes"]["slot"], by["Gtr"]["slot"]], "freq": 120, "type": "low shelf", "depth_db": 2, "sections": ["Chorus"]})
check(not err and r["rule"] >= 1 and len(r["placed_on"]) == 2 and r["sections"] == "Chorus", f"add_dip: {r}")
rule = r["rule"]
tool("get_suggestions"); tool("apply"); time.sleep(0.3)
t3, _ = tool("list_tracks")
kept = [e for t in t3["tracks"] for e in t["applied_eq"] if e.get("rule") == rule]
check(len(kept) == 2 and all(e["sections"] == "Chorus" for e in kept), "a hand-placed dip survives a later apply")
r, err = tool("remove_dip", {"rule": rule}); time.sleep(0.3)
t3, _ = tool("list_tracks")
check(not err and not any(e.get("rule") == rule for t in t3["tracks"] for e in t["applied_eq"]), "remove_dip takes it out")
check(not [e for t in t3["tracks"] if t["name"] == "Stereo Out" for e in t["applied_eq"]], "the master never gets EQ")

# 0.5: arrangement over time, levels at the stereo out, pictures, balance moves.
import base64
tool("clear_eq", {"levels": True, "eq": False})
tool("set_sections", {"sections": []})
tool("compare", {"on": True, "matched": False})   # hear the faders only, not our own EQ
tool("listen", {"action": "start"})   # the fake transport restarts at bar 1
for k in range(8):
    time.sleep(4)
    st, _ = tool("listen", {"action": "status"})
    if k == 7: break
    print("  listening", k, [(t["name"], t["captured_seconds"]) for t in st["tracks"]], "session", "alive" if sess.poll() is None else sess.returncode, flush=True)
tool("listen", {"action": "stop"})
tool("compare", {"on": False})
ar, err = tool("analyse_arrangement")
if not isinstance(ar, dict): print("analyse_arrangement returned:", ar[:3000]); ar = {}
check(not err and ar.get("tempo_bpm") == 120 and ar.get("bars_heard", 0) >= 24, f"tempo and bars heard: {ar.get('tempo_bpm')} bpm, {ar.get('bars_heard')} bars")
levels = ar.get("levels") if isinstance(ar.get("levels"), dict) else {}
lv = {t["track"]: t["post_fader_offset_db"] for t in levels.get("tracks", [])}
print("  level fit r2:", levels.get("fit_quality_r2"), lv)
want_lv = {"Kick": 0, "Bass DI": -6, "Lead Vox": 3}
check(all(isinstance(lv.get(k), (int, float)) and abs(lv[k] - v) <= 1.0 for k, v in want_lv.items()), "post-fader levels recovered from the stereo out (Kick 0, Bass -6, Vox +3)")
check(isinstance(lv.get("Rhodes"), (int, float)) and lv.get("Rhodes") == lv.get("Rhythm Gtr") and -3.2 <= lv["Rhodes"] <= 0.2, "the layered pair (Gtr -3, Rhodes 0) is fitted as one sound")
secs = ar.get("sections", [])
for s in secs: print(f"  section {s['name']} bars {s['bars']}: {', '.join(s['playing'])}; leader {s.get('leader')} {s.get('leader_share_of_band')} of {s.get('leader_band')}, {s.get('lufs_approx')} LUFS")
starts = [int(s["bars"].split("-")[0]) for s in secs]
check("detected" in ar.get("sections_source", "") and starts[:3] == [1, 9, 17], f"sections detected from who plays where: starts {starts}")
check("loudness" in ar, f"loudness from the master: {ar.get('loudness')}")
for v in ("arrangement", "masking", "spectrum"):
    r = tool("render_view", {"view": v, "section": 2} if v == "masking" else {"view": v}, raw=True)
    imgs = [x for x in r["content"] if x["type"] == "image"]
    data = base64.b64decode(imgs[0]["data"]) if imgs else b""
    check(data[:8] == b"\x89PNG\r\n\x1a\n" and not r.get("isError"), f"render_view {v}: PNG {len(data)} bytes; {r['content'][0]['text'][:120]}")
tool("set_sections", {"sections": [{"name": "Full", "start_bar": 1, "end_bar": 8}, {"name": "Vox", "start_bar": 9, "end_bar": 16},
                                    {"name": "Inst", "start_bar": 17, "end_bar": 24, "lead_slot": by["Rhodes"]["slot"]}]})
sug, err = tool("get_suggestions")
check(not err and "balance" in sug and "stereo out" in sug["levels"], f"get_suggestions plans from post-fader levels, {len(sug.get('balance', []))} level move(s)")
for s in sug.get("balance", []): print(f"  level #{s['id']} {s['track']}: {s['move']} - {s['reason']}")
for n in sug.get("notes", []): print("  note:", n)
if sug.get("balance"):
    ids = [s["id"] for s in sug["balance"]]
    eq_before = {t["name"]: t["applied_eq"] for t in tool("list_tracks")[0]["tracks"]}
    r, err = tool("apply", {"ids": ids}); time.sleep(0.4)
    tl, _ = tool("list_tracks")
    check(not err and any("planned_level" in t for t in tl["tracks"]), "balance moves land as planned levels")
    check({t["name"]: t["applied_eq"] for t in tl["tracks"]} == eq_before, "a level-only apply keeps every track's EQ")
    tool("undo"); time.sleep(0.4)
    tl, _ = tool("list_tracks")
    check(not any("planned_level" in t for t in tl["tracks"]), "undo reverts level moves")
r, err = tool("set_trim", {"slot": by["Lead Vox"]["slot"], "db": 1.5, "section": "Vox"})
tool("set_trim", {"slot": by["Bass DI"]["slot"], "db": -1})
check(not err, "set_trim")
time.sleep(1.0)
tl, _ = tool("list_tracks")
pl = {t["name"]: t.get("planned_level") for t in tl["tracks"]}
check(pl.get("Lead Vox") == {"Vox": 1.5} and pl.get("Bass DI") == {"whole_song_db": -1}, f"planned levels visible: {pl}")
# 0.6: mix health, harmonics, matched compare.
mh, err = tool("mix_health")
if not isinstance(mh, dict): print("mix_health returned:", str(mh)[:2000]); mh = {}
print("  loudness:", mh.get("loudness"))
for w in mh.get("warnings", []): print("  warning:", w)
for f in mh.get("findings", []): print("  finding:", f)
check(not err and mh.get("loudness", {}).get("integrated_lufs", -200) > -60 and mh.get("loudness", {}).get("true_peak_dbtp", -200) > -60, "mix_health: loudness and true peak from the stereo out")
found = mh.get("findings", [])
check(any(f["kind"] == "unexplained" and f["band"] in ("mid", "upper-mid", "presence") and f["bars"].startswith("17") for f in found), "mix_health finds the pad no CLEARING measures (bars 17-24)")
inst = [s for s in mh.get("sections", []) if s["name"] == "Inst"]
check(inst and inst[0]["bands"].get("mid", {}).get("correlation", 1) < 0.9, f"the wide pad shows as lower correlation in Inst: {inst[0]['bands'].get('mid') if inst else None}")
r = tool("render_view", {"view": "stereo"}, raw=True)
imgs = [x for x in r["content"] if x["type"] == "image"]
check(bool(imgs) and not r.get("isError"), "render_view stereo")
sug, _ = tool("get_suggestions")
check("harmonics" in sug, f"get_suggestions has a harmonics list ({len(sug.get('harmonics', []))})")
for s in sug.get("harmonics", []): print(f"  harmonics #{s['id']} {s['track']}: {s['move']} - {s['reason'][:120]}")
tl0, _ = tool("list_tracks")
before = {t["name"]: [h["type"] for h in t.get("harmonics", [])] for t in tl0["tracks"]}
r, err = tool("set_harmonics", {"slot": by["Bass DI"]["slot"], "kind": "bass", "lo_hz": 40, "hi_hz": 100, "mix_db": -12})
r2, err2 = tool("set_harmonics", {"slot": by["Lead Vox"]["slot"], "kind": "presence", "freq_hz": 2500})
check(not err and not err2, "set_harmonics")
time.sleep(0.6)
tl, _ = tool("list_tracks")
hm = {t["name"]: [h["type"] for h in t.get("harmonics", [])] for t in tl["tracks"]}
check("harmonic bass" in hm.get("Bass DI", []) and "presence fill" in hm.get("Lead Vox", []), f"harmonics visible per track: {hm}")
tool("undo"); time.sleep(0.4)
tl, _ = tool("list_tracks")
hm2 = {t["name"]: [h["type"] for h in t.get("harmonics", [])] for t in tl["tracks"]}
check(hm2.get("Lead Vox", []) == before.get("Lead Vox", []) and "harmonic bass" in hm2.get("Bass DI", []), f"undo takes back the last harmonics step: {hm2} (before {before})")
r = tool("compare", {"on": True})
check("level-matched" in r[0], "compare is level-matched by default")
tool("compare", {"on": False})

# 0.8: balance the levels.
bm, err = tool("balance_mix", {"style": "pop", "lead": "on top"})
if not isinstance(bm, dict): print("balance_mix returned:", str(bm)[:1500]); bm = {}
print("  balance:", bm.get("measured_from"), "|", bm.get("distance_from_target_db"))
for m in bm.get("moves", []): print(f"  move #{m['id']} {m['track']}: {m['move']} - {m['reason'][:140]}")
for g in bm.get("gain_staging", []): print("  gain staging:", g[:140])
check(not err and bm.get("style") == "pop" and bm.get("lead") == "on top" and "stereo out" in bm.get("measured_from", ""), "balance_mix measures at the stereo out with the chosen style and lead")
secs = bm.get("sections", [])
check(len(secs) == 3 and all(s["parts"] for s in secs), f"balance per saved section ({[s['name'] for s in secs]})")
check(any("target_vs_lead_db" in p for s in secs for p in s["parts"]), "each part shows where it is and where the style puts it, against the lead")
check(isinstance(bm.get("gain_staging"), list), "gain staging is checked")
r = tool("render_view", {"view": "balance"}, raw=True)
check(any(x["type"] == "image" for x in r["content"]) and not r.get("isError"), "render_view balance")
if bm.get("moves"):
    ids = [m["id"] for m in bm["moves"]]
    r, err = tool("apply", {"ids": ids}); time.sleep(0.5)
    tl, _ = tool("list_tracks")
    check(not err and any("planned_level" in t for t in tl["tracks"]), "balance moves land as planned levels (Trim)")
    tool("undo"); time.sleep(0.4)
else:
    check(False, "balance_mix proposed no moves for a pop balance of the test mix")
applied_tracks = sorted({t["name"] for t in t2["tracks"] if t["applied_eq"]})
mcp.stdin.close(); mcp.wait(timeout=5)

open(stopf, "w").close()
out = sess.stdout.read(); sess.wait(timeout=60); os.remove(stopf)
print(out.strip())
finals = {l.split("|")[0][6:].strip(): l for l in out.splitlines() if l.startswith("FINAL")}
check("trim base -1.0" in finals.get("Bass DI", "") and "s1 +1.5" in finals.get("Lead Vox", ""), "the instances run the planned levels and keep them for the project")
check("MASTER mode=1" in out, "the master instance stayed in Master mode")
check("harm bass 40-100" in finals.get("Bass DI", ""), "the instance runs the harmonic bass and keeps it for the project")
applied_tracks = [n for n in applied_tracks if n != "Stereo Out"]
check(all("Hz" in finals[n] for n in applied_tracks), f"the instances themselves are running the applied EQ: {applied_tracks}")
check("role=Lead Vocal" in finals.get("Lead Vox", "") and "role=Electric Gtr" in finals.get("Rhythm Gtr", ""), "instance parameters changed, quick successive edits kept (Logic would save them)")
check(all("amount=0.50 movement=0.70" in l for l in finals.values()), "Amount and Movement landed on the instances' parameters (automatable, saved)")
check(any("(dip)" in l for l in finals.values()), "the instances are running focus-following dips")
print("MCP SESSION TEST " + ("FAILED" if fails else "OK"))
sys.exit(1 if fails else 0)
