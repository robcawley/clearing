#!/bin/bash
# Build, install, sign and validate CLEARING.
set -e
cd "$(dirname "$0")"
cmake --build build --target Clearing_AU ClearingMcp ClearingFakeSession ClearingSnapshot ClearingCoreCheck ClearingHubCheck -j8
mkdir -p bin && rm -f bin/clearing-mcp && cp build/clearing-mcp bin/clearing-mcp && codesign --force -s - bin/clearing-mcp  # replace, never overwrite in place: macOS kills a binary whose signature changed under it
DEST=~/Library/Audio/Plug-Ins/Components/CLEARING.component
rm -rf "$DEST"
ditto build/Clearing_artefacts/Release/AU/CLEARING.component "$DEST"
codesign --force --deep -s - "$DEST"
killall -9 AudioComponentRegistrar 2>/dev/null || true
sleep 3
auval -v aufx Clrg Rcwl

# The bridge (CLEARING from any tool): bin/clearing, also on the PATH as `clearing`.
rm -f bin/clearing && cp Tools/clearing_bridge.py bin/clearing && chmod +x bin/clearing
mkdir -p ~/.local/bin && ln -sf "$PWD/bin/clearing" ~/.local/bin/clearing
bin/clearing stop >/dev/null 2>&1 || true   # a running bridge restarts on next use with the new code

# Register the connector with the Claude desktop app (only if it isn't there; backs the config up first).
python3 - "$PWD/bin/clearing-mcp" <<'PY'
import json, os, shutil, sys, time
cfg = os.path.expanduser("~/Library/Application Support/Claude/claude_desktop_config.json")
if not os.path.isdir(os.path.dirname(cfg)): sys.exit(0)            # Claude desktop not installed
data = json.load(open(cfg)) if os.path.exists(cfg) else {}
servers = data.setdefault("mcpServers", {})
if servers.get("clearing", {}).get("command") == sys.argv[1]: sys.exit(0)
if os.path.exists(cfg): shutil.copy(cfg, cfg + time.strftime(".%Y%m%d-%H%M%S.bak"))
servers["clearing"] = {"command": sys.argv[1], "args": []}
json.dump(data, open(cfg, "w"), indent=2)
print("Added the clearing connector to Claude; quit and reopen Claude once.")
PY
echo "CLEARING installed. The connection in Claude updates itself; quit and reopen Logic if the hub version changed."
