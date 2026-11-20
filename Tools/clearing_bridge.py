#!/usr/bin/env python3
"""CLEARING bridge: use CLEARING from anything, not only MCP apps.

Keeps one clearing-mcp running in the background (so suggestion ids and undo carry across calls),
restarts it if it stops, and answers on a unix socket and, optionally, on http://127.0.0.1:<port>.

  clearing status                      what's live and the next step
  clearing tools                       every tool with what it does
  clearing call <tool> [json | k=v ...] run a tool, e.g.  clearing call set_track slot=2 role=bass
  clearing prompt                      a briefing to paste into any AI assistant (instructions + tools)
  clearing serve [--http PORT]         run the bridge in the foreground (call/status start it if needed)
  clearing stop                        stop the background bridge

HTTP (only with --http, bound to this Mac only):
  GET  /status   GET /tools   GET /instructions   POST /call {"tool": "...", "args": {...}}
"""
import base64, json, os, signal, socket, subprocess, sys, threading, time

HOME = os.path.expanduser("~")
ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))     # .../CLEARING
BIN = os.environ.get("CLEARING_MCP", os.path.join(ROOT, "bin", "clearing-mcp"))
RUN = os.environ.get("CLEARING_BRIDGE_DIR", os.path.join(HOME, "Library", "Application Support", "CLEARING"))
SOCK = os.path.join(RUN, "bridge.sock")
PIDF = os.path.join(RUN, "bridge.pid")
LOG = os.path.join(RUN, "bridge.log")
VIEWS = os.path.join(HOME, "Music", "CLEARING Views")


class Mcp:
    """One clearing-mcp child, restarted when it stops."""
    def __init__(self):
        self.p = None; self.rid = 0; self.lock = threading.Lock(); self.info = {}

    def _start(self):
        self.p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.info = self._rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                             "clientInfo": {"name": "bridge", "version": "1"}})["result"]
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n"); self.p.stdin.flush()

    def _rpc(self, method, params):
        self.rid += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.rid, "method": method, "params": params}) + "\n"); self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line: raise RuntimeError("clearing-mcp stopped")
            msg = json.loads(line)
            if msg.get("id") == self.rid: return msg
            # notifications (e.g. tools/list_changed after it updated itself) are skipped

    def rpc(self, method, params):
        with self.lock:
            for attempt in range(2):
                try:
                    if self.p is None or self.p.poll() is not None: self._start()
                    return self._rpc(method, params)
                except (RuntimeError, BrokenPipeError, ValueError):
                    self.p = None
                    if attempt: raise

    def call(self, tool, args):
        r = self.rpc("tools/call", {"name": tool, "arguments": args or {}})
        return r.get("result", {"isError": True, "content": [{"type": "text", "text": json.dumps(r.get("error"))}]})


def result_text(res):
    """Text for a terminal: text parts as they are, images as the file they were saved to."""
    out = []
    for c in res.get("content", []):
        if c.get("type") == "text": out.append(c["text"])
        elif c.get("type") == "image":
            os.makedirs(VIEWS, exist_ok=True)
            path = os.path.join(VIEWS, "bridge-" + time.strftime("%Y%m%d-%H%M%S") + ".png")
            with open(path, "wb") as f: f.write(base64.b64decode(c["data"]))
            out.append("[picture saved: " + path + "]")
    return ("ERROR: " if res.get("isError") else "") + "\n".join(out)


def serve(http_port=None):
    os.makedirs(RUN, exist_ok=True)
    mcp = Mcp()
    mcp.rpc("ping", {})
    if os.path.exists(SOCK): os.unlink(SOCK)
    with open(PIDF, "w") as f: f.write(str(os.getpid()))

    def handle(req):
        kind = req.get("kind", "call")
        if kind == "tools": return mcp.rpc("tools/list", {})["result"]
        if kind == "instructions": return {"instructions": mcp.info.get("instructions", ""), "server": mcp.info.get("serverInfo", {})}
        if kind == "status": return mcp.call("status", {})
        return mcp.call(req["tool"], req.get("args") or {})

    def unix_loop():
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind(SOCK); os.chmod(SOCK, 0o600); s.listen(8)
        while True:
            c, _ = s.accept()
            threading.Thread(target=unix_client, args=(c,), daemon=True).start()

    def unix_client(c):
        try:
            data = b""
            while not data.endswith(b"\n"):
                chunk = c.recv(1 << 16)
                if not chunk: break
                data += chunk
            try: out = handle(json.loads(data))
            except Exception as e: out = {"isError": True, "content": [{"type": "text", "text": str(e)}]}
            c.sendall(json.dumps(out).encode() + b"\n")
        finally: c.close()

    if http_port:
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        class H(BaseHTTPRequestHandler):
            def _send(self, obj, code=200):
                b = json.dumps(obj).encode()
                self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
            def do_GET(self):
                k = self.path.strip("/").split("?")[0]
                if k in ("status", "tools", "instructions"): self._send(handle({"kind": k}))
                else: self._send({"error": "use GET /status, /tools, /instructions or POST /call"}, 404)
            def do_POST(self):
                if self.path.strip("/") != "call": return self._send({"error": "POST /call"}, 404)
                n = int(self.headers.get("Content-Length", "0"))
                try: req = json.loads(self.rfile.read(n) or b"{}")
                except ValueError: return self._send({"error": "body must be JSON"}, 400)
                if "tool" not in req: return self._send({"error": "missing 'tool'"}, 400)
                self._send(handle(req))
            def log_message(self, *a): pass
        srv = ThreadingHTTPServer(("127.0.0.1", int(http_port)), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        print(f"CLEARING bridge: http://127.0.0.1:{http_port}", flush=True)
    print("CLEARING bridge: " + SOCK, flush=True)
    unix_loop()


def running():
    if not os.path.exists(SOCK): return False
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(1); s.connect(SOCK); s.close(); return True
    except OSError: return False


def ensure():
    if running(): return
    os.makedirs(RUN, exist_ok=True)
    with open(LOG, "a") as log:
        subprocess.Popen([sys.executable, os.path.realpath(__file__), "serve"], stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        if running(): return
        time.sleep(0.1)
    sys.exit("CLEARING bridge didn't start; see " + LOG)


def request(req):
    ensure()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SOCK)
    s.sendall((json.dumps(req) + "\n").encode())
    data = b""
    while True:
        chunk = s.recv(1 << 16)
        if not chunk: break
        data += chunk
    return json.loads(data)


def parse_args(rest):
    if not rest: return {}
    if len(rest) == 1 and rest[0].lstrip().startswith("{"): return json.loads(rest[0])
    out = {}
    for kv in rest:
        k, _, v = kv.partition("=")
        try: out[k] = json.loads(v)
        except ValueError: out[k] = v
    return out


def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help", "help"): print(__doc__); return
    cmd = a[0]
    if cmd == "serve":
        port = a[a.index("--http") + 1] if "--http" in a else None
        if running() and not port: print("already running"); return
        serve(port)
    elif cmd == "stop":
        try: os.kill(int(open(PIDF).read()), signal.SIGTERM); print("stopped")
        except Exception: print("not running")
        for f in (SOCK, PIDF):
            if os.path.exists(f): os.unlink(f)
    elif cmd == "status": print(result_text(request({"kind": "status"})))
    elif cmd == "tools":
        for t in request({"kind": "tools"}).get("tools", []):
            props = ", ".join(t.get("inputSchema", {}).get("properties", {}).keys())
            print(f"{t['name']}({props})\n    {t['description']}\n")
    elif cmd == "call":
        if len(a) < 2: sys.exit("clearing call <tool> [json | key=value ...]")
        res = request({"tool": a[1], "args": parse_args(a[2:])})
        print(result_text(res)); sys.exit(1 if res.get("isError") else 0)
    elif cmd == "prompt":
        info = request({"kind": "instructions"}); tools = request({"kind": "tools"}).get("tools", [])
        print("You can operate CLEARING, an automatic mix plug-in in the user's Logic Pro session, by asking the user to run\n"
              "  clearing call <tool> '<json arguments>'\nin Terminal and paste back the output (or, if you can run commands, run it yourself).\n")
        print(info.get("instructions", "")); print("\nTOOLS")
        for t in tools: print(f"- {t['name']}: {t['description']}\n  arguments: {json.dumps(t.get('inputSchema', {}).get('properties', {}))}")
    else: sys.exit("unknown command; see clearing --help")


if __name__ == "__main__":
    main()
