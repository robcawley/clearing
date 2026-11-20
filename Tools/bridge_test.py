#!/usr/bin/env python3
"""Bridge and self-update test: a fake session, the bridge (CLI + HTTP), and clearing-mcp replacing itself."""
import json, os, shutil, subprocess, sys, tempfile, time, urllib.request

build = sys.argv[1] if len(sys.argv) > 1 else "build"
here = os.path.dirname(os.path.realpath(__file__))
tmp = tempfile.mkdtemp(prefix="clr-bridge-")
env = dict(os.environ, CLEARING_HUB="/clearing_bridge_test", CLEARING_BRIDGE_DIR=tmp,
           CLEARING_MCP=os.path.join(tmp, "clearing-mcp"))
shutil.copy(os.path.join(build, "clearing-mcp"), env["CLEARING_MCP"])
fails = 0
def check(ok, what):
    global fails
    print(("ok:   " if ok else "FAIL: ") + what, flush=True)
    if not ok: fails += 1

stopf = os.path.join(tmp, "stop")
sess = subprocess.Popen([f"{build}/ClearingFakeSession_artefacts/Release/ClearingFakeSession", "120", stopf], env=env, stdout=subprocess.PIPE, text=True)
sess.stdout.readline(); time.sleep(1.0)
bridge = [sys.executable, os.path.join(here, "clearing_bridge.py")]
def cli(*a):
    r = subprocess.run(bridge + list(a), env=env, capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.strip()

rc, out = cli("status")
st = json.loads(out) if out.startswith("{") else {}
check(rc == 0 and st.get("tracks_with_clearing") == 5 and "next" in st, f"clearing status: {st.get('tracks_with_clearing')} tracks; next: {st.get('next', out)[:80]}")
rc, out = cli("tools")
check(rc == 0 and "set_harmonics(" in out and "status(" in out, "clearing tools lists every tool")
rc, out = cli("call", "set_track", "slot=1", "role=bass")
check(rc == 0 and '"role": "Bass"' in out, "clearing call with key=value arguments")
rc, out = cli("call", "listen", '{"action": "start"}')
check(rc == 0 and out.startswith("Listening"), "clearing call with JSON arguments")
time.sleep(10)
cli("call", "listen", "action=stop")
rc, out = cli("call", "get_suggestions")
sug = json.loads(out) if out.startswith("{") else {}
ids = [s["id"] for s in sug.get("suggestions", [])][:1]
check(rc == 0 and ids, f"suggestions through the bridge ({len(sug.get('suggestions', []))})")
rc, out = cli("call", "apply", json.dumps({"ids": ids}))
check(rc == 0 and '"applied"' in out, "apply in a later call finds the ids (one shared connection)")
rc, out = cli("call", "undo")
check(rc == 0 and "Undone" in out, "undo in a later call")
rc, out = cli("call", "nonsense_tool")
check(rc != 0 and "ERROR" in out, "an unknown tool is an error, exit code 1")
rc, out = cli("prompt")
check(rc == 0 and "clearing call" in out and "TOOLS" in out and "- status:" in out, "clearing prompt makes a briefing")
# HTTP
cli("stop")
srv = subprocess.Popen(bridge + ["serve", "--http", "7899"], env=env, stdout=subprocess.PIPE, text=True)
srv.stdout.readline(); time.sleep(0.5)
st = json.loads(urllib.request.urlopen("http://127.0.0.1:7899/status").read())
check("content" in st and "tracks_with_clearing" in st["content"][0]["text"], "HTTP GET /status")
req = urllib.request.Request("http://127.0.0.1:7899/call", data=json.dumps({"tool": "list_tracks", "args": {"group": 0}}).encode(), headers={"Content-Type": "application/json"})
lt = json.loads(urllib.request.urlopen(req).read())
check("Kick" in lt["content"][0]["text"], "HTTP POST /call")
srv.terminate(); srv.wait()
cli("stop")

# Self-update: run clearing-mcp from a copy, replace the copy, and keep talking on the same pipe.
p = subprocess.Popen([env["CLEARING_MCP"]], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def send(obj): p.stdin.write(json.dumps(obj) + "\n"); p.stdin.flush()
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "claude-ai", "version": "1"}}})
init = json.loads(p.stdout.readline())
check(init["result"]["capabilities"]["tools"].get("listChanged") is True, "the server says its tool list can change")
pid = p.pid
os.remove(env["CLEARING_MCP"]); shutil.copy(os.path.join(build, "clearing-mcp"), env["CLEARING_MCP"])  # an "install"
time.sleep(6)
send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "status", "arguments": {}}})
lines = []
while True:
    l = json.loads(p.stdout.readline()); lines.append(l)
    if l.get("id") == 2: break
check(any(l.get("method") == "notifications/tools/list_changed" for l in lines), "after the update it tells the client its tools changed")
check("tracks_with_clearing" in lines[-1]["result"]["content"][0]["text"] and p.poll() is None and p.pid == pid,
      "and keeps answering on the same connection (same process id, new code)")
p.stdin.close(); p.wait(timeout=10)

open(stopf, "w").close(); sess.wait(timeout=30)
shutil.rmtree(tmp, ignore_errors=True)
print("BRIDGE TEST " + ("OK" if not fails else f"FAILED ({fails})"))
sys.exit(1 if fails else 0)
