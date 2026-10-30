# hyprule

A small, quick app made to ease the Hyprland transition to Lua. A GTK4 app for Hyprland's Lua config: see where every open window is sent, add or remove `hl.window_rule` entries for it, and rename workspaces in Hyprland and Waybar at once.

**Status:** working, and used daily by the author on a Lua-config Hyprland build. It is a personal tool made public: it assumes a config laid out one rule per line (see [Assumptions](#assumptions)), and the optional "CRT snap" feature reflects a two-monitor setup. No packaging yet; build from source.

<!-- MEDIA: screenshot of the Window Rules tab. Left pane: the running-window list with a few "→ wsN" badges visible. Right pane: one window selected, its "RULES SET FOR THIS APP" panel filled in, and the GENERATED RULE PREVIEW showing an hl.window_rule line. Save as docs/media/window-list.png -->

<!-- MEDIA: short GIF (5-10 s) of moving a window's rule: select a window that already has a rule, click "Remove This App's Rules", pick a different workspace in the dropdown, click "Add Rule", hit Refresh, and show the badge change from "→ wsA" to "→ wsB". Save as docs/media/move-rule.gif -->

## Why

Hyprland's newer builds can be configured in Lua instead of `hyprland.conf`. Rules become calls such as:

```lua
hl.window_rule({ name = "ws-pcsx2", match = { class = "^(pcsx2-qt)$" }, workspace = 11 })
hl.workspace_rule({ workspace = "11", monitor = "HDMI-A-1", default_name = "11: Emulator" })
```

That is more powerful, but there is no tooling yet for it. To send an app to a workspace you look up its class with `hyprctl clients`, hand-write the regex, find the right spot in a long file, reload, and hope you did not break a multi-line rule. Renaming a workspace means editing `hyprland.lua` and then every Waybar `format-icons` block by hand so the bar matches. hyprule does both from a window.

## What it does

- **Lists every running window** (`hyprctl clients`) with its class, title and a **"→ wsN" badge** showing where your existing rules already send it.
- **Shows the rules that match** the selected window, summarised ("ws-firefox: ws2 (Browser), float"), and **removes them** in one click.
- **Adds a rule** from a form: workspace (the dropdown is read live from your `hl.workspace_rule` lines, so it is never out of date), float, fullscreen, pin to the workspace's monitor. A live preview shows the exact Lua it will write.
- **Writes native-looking Lua.** The rule is inserted next to your other rules, formatted like them, with no "added by" comment. Hyprland can reload automatically.
- **Renames workspaces**: edit the names (nerd-font glyphs included), and one Apply updates `default_name` in `hyprland.lua` and every Waybar `format-icons` block, then reloads Hyprland and Waybar.
- **Optional snap categories** (with the companion daemon): tag a window as *Emulator* or *Media*, and whenever such a window lands on a chosen monitor it is moved, without stealing focus, to that category's workspace.
- **Themed from pywal** if you use it (optional, see below).

## How it works

- **Everything is read from the config at run time.** Workspaces come from parsing `hl.workspace_rule({...})` lines; the window badges come from matching each window's `class` and `initialClass` against every rule's `class = "^(...)$"` pattern with `std::regex_match`. An earlier version had hardcoded workspace names that went stale; this rebuild has none.
- **Inserting without breaking the file.** The new rule goes after the last *complete single-line* rule: a line that starts with `hl.window_rule(` *and* ends with `})`. The first attempt anchored on any line starting with `hl.window_rule(`, which split a multi-line rule block in two during testing. The stricter anchor fixed it, and it also lands new rules among the one-line workspace assignments where they belong. If no such line exists, the rule is appended.
- **Removal is exact.** "Remove This App's Rules" deletes precisely the lines the matcher found for that window, so it only ever touches what the panel showed you. It was checked against real configs with three, two, one and zero matching rules, and it cleans up duplicate rules too.
- **Lua values, quoted or not.** A small parser reads both `workspace = 11` and `workspace = "11"`, since hand-written and generated rules differ.
- **Waybar sync** walks every `"format-icons"` object in the Waybar config and replaces the value for the workspace id, so setups with more than one bar stay consistent. Waybar is refreshed with `SIGUSR2`. (Hyprland itself only shows a new `default_name` once the workspace is next created; Waybar updates immediately.)
- **JSON is never hand-edited from C++.** Snap categories live in `snap.json`; the app shells out to a tiny Python helper, `hyprule-snap-add CATEGORY CLASS`, which edits it idempotently.
- **The snap daemon** (`daemon/hyprule-snapd.py`) listens on Hyprland's event socket (`.socket2.sock`) for `openwindow` and `movewindowv2`. When a window whose class contains a category keyword is on the configured monitor, it moves it with `hl.dsp.window.move({ workspace = N, window = "address:0x...", silent = true })`, so focus never jumps. Things learned along the way:
  - `hyprctl clients` only reports an integer monitor id, and the ids are not in the order you might expect, so the daemon maps id to name from `hyprctl monitors` (refreshed every few seconds to survive hotplug).
  - It only ever acts on windows already on the chosen monitor, so rules on the main display are never fought. A negative test (a media window on the main monitor) confirmed it stays put.
  - It is idempotent (skips windows already on their target workspace, so it cannot loop), reconciles misplaced windows once at start-up, and hot-reloads `snap.json` when its modification time changes.
  - On this Lua build, raw string dispatchers are gone; everything goes through `hyprctl dispatch '<lua>'`.

## Workflow for this project

- The app was rebuilt from the ground up with an AI coding agent, working against the author's real `hyprland.lua` and Waybar config rather than a mock.
- The rule-removal logic was verified standalone by the agent against real rules for several apps (three, two, one and zero matches) before it was wired to the button.
- The multi-line-rule bug was caught in that testing and fixed with the "must end with `})`" anchor.
- The daemon's safety property (never touch windows on the main display) was checked with a deliberate negative test, not assumed.

## Requirements

- Hyprland configured in **Lua** (`~/.config/hypr/hyprland.lua`, with the compositor-provided `hl` API) and `hyprctl` on `PATH`.
- GTK 4 and a C++17 compiler, CMake 3.16+, `pkg-config`.
- Optional: Waybar (`~/.config/waybar/config.jsonc`) for name sync, Python 3 for the snap helper and daemon, pywal for theming.

On Arch: `sudo pacman -S gtk4 cmake pkgconf`.

## Install

```sh
git clone https://github.com/CipherNovaa/hyprule.git
cd hyprule
bash build.sh
```

`build.sh` builds a release binary, installs it and the `hyprule-snap-add` helper to `~/.local/bin/`, and writes a `hyprule.desktop` entry so it shows up in rofi or any launcher as "HyprRule". To build only:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Usage

Run `hyprule`. Pick a window, choose a workspace and options, check the preview, click **Add Rule**. Use **Refresh** after opening new windows. The **Workspace Names** tab edits names; **Apply** writes them everywhere.

Back up `hyprland.lua` and your Waybar config before first use: hyprule rewrites them in place.

### Paths

The app reads its config files under `$XDG_CONFIG_HOME` (default `~/.config`); the Python helper and daemon use `~/.config` directly:

| File | Used for |
|---|---|
| `hypr/hyprland.lua` | rules and workspace names (read and written) |
| `waybar/config.jsonc` | `format-icons` sync (read and written, optional) |
| `hyprule/snap.json` | snap categories and the snap monitor (optional) |
| `~/.cache/wal/colors.json` | pywal colours (optional, read only) |

### Theming (optional)

If `~/.cache/wal/colors.json` exists, the app takes its background, foreground and accent colours from pywal so it matches your wallpaper. Without pywal it uses a built-in dark palette; nothing else changes.

### Snap categories and the daemon (optional)

1. Copy `examples/snap.example.json` to `~/.config/hyprule/snap.json` and set `monitor` to the output that should get the behaviour (for example a second screen) and each category's `ws`. The app reads the same file for its monitor name and category workspaces.
2. Start the daemon from your Hyprland autostart, for example:

   ```lua
   hl.exec_cmd("python3 /path/to/hyprule/daemon/hyprule-snapd.py")
   ```
3. In hyprule, pick a window, set **Snap category**, and click **Add Rule**: the class is added to that category's keywords.

Set `"enabled": false` in `snap.json` to pause it (hot-reloaded). The daemon logs to `~/.cache/hyprule-snapd.log`.

### Assumptions

- Rules and workspace rules are written **one per line**, in the style shown above. Multi-line rules are left alone: they are not shown as matches and new rules are never inserted inside them.
- Only numeric workspaces appear in the workspace list.
- The two snap categories are *Emulator* and *Media*.

## Licence

MIT, see [LICENSE](LICENSE).
