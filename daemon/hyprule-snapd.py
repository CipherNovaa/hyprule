#!/usr/bin/env python3
# ═══════════════════════════════════════════════════════════════════════════════
# hyprule-snapd — snap categorized windows to their CRT "category" workspace.
#
# When a window whose class matches a category keyword (see
# ~/.config/hyprule/snap.json) lands on the CRT monitor, move it — silently, so
# your focus/view never jumps — to that category's workspace (e.g. any emulator
# → ws11 "Emulator", any media player → ws12 "Media").
#
# It ONLY ever acts on windows already on the CRT monitor, so the main display's
# normal window rules are never touched. Managed by the hyprule app; launched as
# a hyprland exec-once. Toggle with "enabled": false in snap.json (hot-reloaded).
#
# Log: ~/.cache/hyprule-snapd.log
# ═══════════════════════════════════════════════════════════════════════════════

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

CONFIG = Path.home() / ".config" / "hyprule" / "snap.json"
LOG = Path.home() / ".cache" / "hyprule-snapd.log"


def log(msg):
    try:
        with open(LOG, "a") as f:
            f.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")
    except Exception:
        pass


# ── Config (hot-reloaded on mtime change) ────────────────────────────────────
_cfg_cache = {"mtime": 0, "cfg": None}


def load_config():
    try:
        mt = CONFIG.stat().st_mtime
        if _cfg_cache["cfg"] is None or mt != _cfg_cache["mtime"]:
            with open(CONFIG) as f:
                _cfg_cache["cfg"] = json.load(f)
            _cfg_cache["mtime"] = mt
            c = _cfg_cache["cfg"]
            log(f"config loaded: enabled={c.get('enabled')} monitor={c.get('monitor')} "
                f"cats={ {k: v.get('ws') for k, v in c.get('categories', {}).items()} }")
    except Exception as e:
        if _cfg_cache["cfg"] is None:
            _cfg_cache["cfg"] = {"enabled": False, "monitor": "HDMI-A-1", "categories": {}}
        log(f"config load failed ({e}); using last-good")
    return _cfg_cache["cfg"]


# ── Hyprland helpers ─────────────────────────────────────────────────────────
def runtime_dir():
    return os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")


def instance_sig():
    sig = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
    if sig:
        return sig
    hyprdir = Path(runtime_dir()) / "hypr"
    try:
        sigs = sorted(hyprdir.glob("*"), key=lambda p: p.stat().st_mtime, reverse=True)
        return sigs[0].name if sigs else None
    except Exception:
        return None


def socket2_path():
    sig = instance_sig()
    if not sig:
        return None
    return Path(runtime_dir()) / "hypr" / sig / ".socket2.sock"


def hyprctl_json(args):
    try:
        return json.loads(subprocess.check_output(["hyprctl", *args, "-j"], text=True))
    except Exception as e:
        log(f"hyprctl {args} failed: {e}")
        return []


_mon = {"t": 0.0, "map": {}}


def monitor_id_to_name():
    now = time.time()
    if now - _mon["t"] > 5:  # refresh every 5s to survive hotplug
        mons = hyprctl_json(["monitors"])
        if mons:
            _mon["map"] = {m["id"]: m["name"] for m in mons}
            _mon["t"] = now
    return _mon["map"]


def dispatch(lua):
    try:
        subprocess.run(["hyprctl", "dispatch", lua],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as e:
        log(f"dispatch failed: {e}")


# ── Core: categorize + snap ──────────────────────────────────────────────────
def categorize(wclass, categories):
    c = (wclass or "").lower()
    for cat, spec in categories.items():
        for kw in spec.get("keywords", []):
            if kw.lower() in c:
                return cat, spec.get("ws")
    return None, None


def move_window(addr, ws, silent):
    opt = ", silent = true" if silent else ""
    dispatch(f'hl.dsp.window.move({{ workspace = {ws}, window = "address:{addr}"{opt} }})')


def snap_client(c, cfg):
    """Snap one client dict if it's on the CRT and matches a category."""
    monitor = cfg.get("monitor", "HDMI-A-1")
    mons = monitor_id_to_name()
    if mons.get(c.get("monitor")) != monitor:
        return  # not on the CRT — leave it alone (main-display rules untouched)
    cat, ws = categorize(c.get("class", ""), cfg.get("categories", {}))
    if not cat or ws is None:
        return
    cur = c.get("workspace", {}).get("id")
    if cur == ws:
        return  # already home
    log(f"snap {c.get('class')} ({c.get('address')}) ws{cur} -> {cat} ws{ws}")
    move_window(c.get("address"), ws, silent=not cfg.get("follow_focus", False))


def snap_by_addr(addr, cfg):
    for c in hyprctl_json(["clients"]):
        if c.get("address") == addr:
            snap_client(c, cfg)
            return


def reconcile(cfg):
    """One pass over all current windows — snap anything already misplaced on the CRT."""
    for c in hyprctl_json(["clients"]):
        snap_client(c, cfg)


# ── Event loop ───────────────────────────────────────────────────────────────
def norm_addr(a):
    a = a.strip()
    return a if a.startswith("0x") else "0x" + a


def main():
    cfg = load_config()
    sp = socket2_path()
    if not sp or not sp.exists():
        log(f"socket2 not found ({sp}); exiting")
        sys.exit(1)

    log("hyprule-snapd started")
    reconcile(cfg)  # fix current state on startup

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(str(sp))
    with s.makefile("r") as f:
        for line in f:
            cfg = load_config()  # cheap; hot-reloads only on mtime change
            if not cfg.get("enabled", True):
                continue
            ev, _, data = line.strip().partition(">>")
            # movewindowv2 = a window changed workspace (the "flung to CRT" signal);
            # openwindow    = a new window mapped (may open directly onto the CRT).
            if ev in ("movewindowv2", "openwindow"):
                first = data.split(",", 1)[0]
                if not first:
                    continue
                if ev == "openwindow":
                    time.sleep(0.12)  # let the window settle onto its monitor/ws
                try:
                    snap_by_addr(norm_addr(first), cfg)
                except Exception as e:
                    log(f"event error on '{line.strip()}': {e}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except Exception as e:
        log(f"fatal: {e}")
        raise
