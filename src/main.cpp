// ═══════════════════════════════════════════════════════════════════════════════
// HyprRule — Hyprland workspace & window-rule manager (GTK4)
//
// Two jobs:
//   1. Window Rules — pick a running window, assign it a workspace + options, and
//      write a native-looking hl.window_rule(...) into $XDG_CONFIG_HOME/hypr/hyprland.lua
//      (inserted with the other rules — no "Added by HyprRule" noise). Optionally
//      tag it as an Emulator/Media app so the snap daemon re-homes it on the CRT.
//   2. Workspace Names — rename any workspace; writes the hyprland.lua default_name
//      AND syncs every waybar format-icons block so the bars match. One click.
//
// Everything is read live from the configs, so the workspace list is never stale.
// Theming follows pywal (~/.cache/wal). Build with build.sh (GTK4).
// ═══════════════════════════════════════════════════════════════════════════════

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <regex>

// ─────────────────────────────────────────────────────────────────────────────
// Data
// ─────────────────────────────────────────────────────────────────────────────

struct WindowInfo {
    std::string wclass;
    std::string title;
    std::string initialClass;
    int         pid = 0;
    std::string assigned;   // short indicator of where its rules put it, e.g. "→ ws11"
};

// One existing hl.window_rule line that matches a given window's class
struct RuleHit {
    std::string text;       // the full config line
    std::string name;       // rule name (if any)
    std::string summary;    // human summary of what it sets
};

struct Workspace {
    int         id = 0;
    std::string name;       // default_name from hyprland.lua
    std::string monitor;    // e.g. "DP-3" / "HDMI-A-1"
    bool        is_crt = false;
};

// One editable row on the Workspace Names tab
struct WsNameRow {
    int        id = 0;
    std::string original;   // name as read from config
    GtkWidget* entry = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Application state
// ─────────────────────────────────────────────────────────────────────────────

static struct AppState {
    // Window Rules tab
    GtkWidget      *list_box    = nullptr;
    GtkWidget      *class_val   = nullptr;
    GtkWidget      *title_val   = nullptr;
    GtkWidget      *name_entry  = nullptr;
    GtkDropDown    *ws_drop     = nullptr;
    GtkDropDown    *snap_drop   = nullptr;   // None / Emulator / Media
    GtkCheckButton *float_cb    = nullptr;
    GtkCheckButton *fs_cb       = nullptr;
    GtkCheckButton *monitor_cb  = nullptr;
    GtkCheckButton *reload_cb   = nullptr;
    GtkWidget      *preview     = nullptr;
    GtkWidget      *status      = nullptr;
    GtkWidget      *rules_val   = nullptr;   // indicator: existing rules for selected window
    GtkWidget      *remove_btn  = nullptr;

    // Workspace Names tab
    GtkWidget      *rename_status = nullptr;

    std::vector<WindowInfo> windows;
    std::vector<Workspace>  workspaces;
    std::vector<WsNameRow>  ws_rows;
    int         selected    = -1;

    std::string config_path;   // hyprland.lua
    std::string waybar_path;   // waybar config.jsonc
    std::string snap_path;     // hyprule/snap.json (snap daemon config)
    std::string crt_monitor = "HDMI-A-1";   // snap.json "monitor"
    int         emu_ws   = 11;              // snap.json categories.Emulator.ws
    int         media_ws = 12;              // snap.json categories.Media.ws
} App;

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

static std::string run_cmd(const std::string& cmd) {
    std::string out;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}

// Flat JSON string extractor (finds the first "key": "value")
static std::string json_str(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    while (p < obj.size() && obj[p] == ' ') p++;
    if (p >= obj.size() || obj[p] != '"') return "";
    p++;
    std::string val;
    while (p < obj.size() && obj[p] != '"') {
        if (obj[p] == '\\') p++;
        if (p < obj.size()) val += obj[p++];
    }
    return val;
}

static int json_int(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return 0;
    p += needle.size();
    while (p < obj.size() && obj[p] == ' ') p++;
    std::string num;
    while (p < obj.size() && (std::isdigit((unsigned char)obj[p]) || obj[p] == '-'))
        num += obj[p++];
    return num.empty() ? 0 : std::stoi(num);
}

// Extract a Lua string field:  key = "value"
static std::string lua_field(const std::string& line, const std::string& key) {
    std::string needle = key + " = \"";
    size_t p = line.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    std::string val;
    while (p < line.size() && line[p] != '"') val += line[p++];
    return val;
}

// Extract any Lua value after `key = ` (quoted OR bare, e.g. workspace = 11 or "11").
static std::string lua_value(const std::string& line, const std::string& key) {
    std::string needle = key + " = ";
    size_t p = line.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    if (p < line.size() && line[p] == '"') {
        p++;
        std::string v;
        while (p < line.size() && line[p] != '"') v += line[p++];
        return v;
    }
    std::string v;
    while (p < line.size() && line[p] != ',' && line[p] != ' ' && line[p] != '}') v += line[p++];
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// hyprctl clients parsing (unchanged proven logic)
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<WindowInfo> parse_clients(const std::string& json) {
    std::vector<WindowInfo> result;
    int    depth = 0;
    bool   in_str = false, escape = false;
    size_t obj_start = std::string::npos;

    for (size_t i = 0; i < json.size(); i++) {
        char c = json[i];
        if (escape)              { escape = false; continue; }
        if (c == '\\' && in_str) { escape = true; continue; }
        if (c == '"')            { in_str = !in_str; continue; }
        if (in_str)              continue;

        if      (c == '[') depth++;
        else if (c == ']') depth--;
        else if (c == '{') { if (depth == 1) obj_start = i; depth++; }
        else if (c == '}') {
            depth--;
            if (depth == 1 && obj_start != std::string::npos) {
                std::string obj = json.substr(obj_start, i - obj_start + 1);
                WindowInfo w;
                w.wclass       = json_str(obj, "class");
                w.title        = json_str(obj, "title");
                w.initialClass = json_str(obj, "initialClass");
                w.pid          = json_int(obj, "pid");
                if (!w.wclass.empty()
                    && w.wclass != "waybar" && w.wclass != "rofi" && w.wclass != "dunst")
                    result.push_back(w);
                obj_start = std::string::npos;
            }
        }
    }
    return result;
}

static std::string make_rule_name(const std::string& wclass) {
    std::string base = wclass;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(dot + 1);
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);
    std::string clean;
    for (char c : base)
        if (std::isalnum((unsigned char)c) || c == '-') clean += c;
    return "ws-" + clean;
}

// ─────────────────────────────────────────────────────────────────────────────
// Workspace parsing — read hl.workspace_rule({...}) lines from hyprland.lua
// so the workspace list is ALWAYS current (no hardcoding).
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<Workspace> parse_workspaces(const std::string& cfg) {
    std::vector<Workspace> out;
    std::istringstream ss(cfg);
    std::string line;
    while (std::getline(ss, line)) {
        // skip commented lines
        size_t firstNonWs = line.find_first_not_of(" \t");
        if (firstNonWs != std::string::npos && line.compare(firstNonWs, 2, "--") == 0) continue;
        if (line.find("hl.workspace_rule(") == std::string::npos) continue;

        std::string ws = lua_field(line, "workspace");
        if (ws.empty()) continue;
        // numeric workspaces only (skip special/named like "w[tv1]")
        bool numeric = !ws.empty() && std::all_of(ws.begin(), ws.end(),
                          [](char c){ return std::isdigit((unsigned char)c); });
        if (!numeric) continue;

        Workspace w;
        w.id      = std::stoi(ws);
        w.monitor = lua_field(line, "monitor");
        w.name    = lua_field(line, "default_name");
        w.is_crt  = (w.monitor == App.crt_monitor);
        out.push_back(w);
    }
    std::sort(out.begin(), out.end(), [](const Workspace& a, const Workspace& b){ return a.id < b.id; });
    return out;
}

// Human-friendly dropdown label for a workspace
static std::string ws_label(const Workspace& w) {
    std::string nm = w.name.empty() ? ("Workspace " + std::to_string(w.id)) : w.name;
    return std::to_string(w.id) + "  —  " + nm + (w.is_crt ? "   [CRT]" : "");
}

// Short display name for a workspace id (strips a leading "N: " if present)
static std::string ws_name_for(int id) {
    for (const auto& w : App.workspaces)
        if (w.id == id) {
            std::string n = w.name;
            // trim leading glyph/prefix noise for compactness — keep it simple
            return n.empty() ? ("ws" + std::to_string(id)) : n;
        }
    return "ws" + std::to_string(id);
}

// Does a running window's class match a rule's class regex?
static bool class_matches(const std::string& pattern, const std::string& c1, const std::string& c2) {
    if (pattern.empty()) return false;
    try {
        std::regex re(pattern);
        if (!c1.empty() && std::regex_match(c1, re)) return true;
        if (!c2.empty() && std::regex_match(c2, re)) return true;
    } catch (...) {}
    return false;
}

// Find every hl.window_rule line in cfg whose class matches this window.
static std::vector<RuleHit> associated_rules(const std::string& cfg,
                                             const std::string& c1, const std::string& c2) {
    std::vector<RuleHit> hits;
    std::istringstream ss(cfg);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("hl.window_rule(") == std::string::npos) continue;
        std::string pat = lua_field(line, "class");
        if (!class_matches(pat, c1, c2)) continue;

        RuleHit h;
        h.text = line;
        h.name = lua_field(line, "name");
        std::string ws  = lua_value(line, "workspace");
        std::string mon = lua_value(line, "monitor");
        std::vector<std::string> parts;
        if (!ws.empty())  parts.push_back("ws" + ws + " (" + ws_name_for(std::atoi(ws.c_str())) + ")");
        if (!mon.empty()) parts.push_back("monitor " + mon);
        if (line.find("float = true") != std::string::npos)      parts.push_back("float");
        if (line.find("fullscreen = true") != std::string::npos) parts.push_back("fullscreen");
        std::string s;
        for (size_t i = 0; i < parts.size(); i++) s += (i ? ", " : "") + parts[i];
        h.summary = (h.name.empty() ? "" : (h.name + ": ")) + (s.empty() ? "(match only)" : s);
        hits.push_back(h);
    }
    return hits;
}

// The short "→ wsN" indicator for a window row (first workspace-assigning rule).
static std::string assignment_indicator(const std::string& cfg,
                                        const std::string& c1, const std::string& c2) {
    for (const auto& h : associated_rules(cfg, c1, c2)) {
        std::string ws = lua_value(h.text, "workspace");
        if (!ws.empty()) return "→ ws" + ws;
    }
    // no workspace rule, but maybe a monitor pin
    for (const auto& h : associated_rules(cfg, c1, c2)) {
        std::string mon = lua_value(h.text, "monitor");
        if (!mon.empty()) return "→ " + mon;
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// pywal theming
// ─────────────────────────────────────────────────────────────────────────────

static std::string pywal_color(const std::string& key, const std::string& fallback) {
    static std::string cache;
    if (cache.empty()) {
        const char* home = g_get_home_dir();
        cache = read_file(std::string(home) + "/.cache/wal/colors.json");
        if (cache.empty()) cache = " ";   // mark "tried"
    }
    std::string v = json_str(cache, key);
    return v.empty() ? fallback : v;
}

static std::string build_css() {
    std::string accent = pywal_color("color4", "#7aa2f7");
    std::string green  = pywal_color("color2", "#88c070");
    std::string bg     = pywal_color("background", "#1a1a1a");
    std::string fg     = pywal_color("foreground", "#e0e0e0");
    std::string bg2    = pywal_color("color0", bg);   // slightly-raised surface
    std::string line   = pywal_color("color8", "#3a3a3a");
    std::ostringstream css;
    // Dark, pywal-driven base so the app matches the wallpaper regardless of the
    // system GTK theme (paired with gtk-application-prefer-dark-theme in on_activate).
    css <<
    "window, .background, headerbar, notebook, notebook > header { background-color: " << bg << "; color: " << fg << "; }\n"
    "notebook > header tab { color: " << fg << "; }\n"
    "list, row, scrolledwindow { background-color: " << bg << "; color: " << fg << "; }\n"
    "row:selected { background-color: " << accent << "; color: " << bg << "; }\n"
    "entry, dropdown, dropdown > button, dropdown popover, dropdown popover contents { background-color: " << bg2 << "; color: " << fg << "; border: 1px solid " << line << "; }\n"
    "checkbutton { color: " << fg << "; }\n"
    ".window-class { font-weight: bold; font-size: 0.9em; }\n"
    ".window-title { font-size: 0.8em; opacity: 0.6; }\n"
    ".assigned-badge { font-size: 0.75em; color: " << accent << "; }\n"
    ".preview-box  { font-family: monospace; font-size: 0.85em; padding: 6px 8px; border-left: 3px solid " << accent << "; }\n"
    ".success { color: " << green  << "; }\n"
    ".error   { color: #cc6666; }\n"
    ".section-title { font-weight: bold; font-size: 0.8em; opacity: 0.55; margin-top: 8px; margin-bottom: 2px; }\n"
    ".ws-id { font-weight: bold; opacity: 0.7; }\n"
    ".crt-badge { font-size: 0.75em; opacity: 0.6; }\n";
    return css.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// UI helpers
// ─────────────────────────────────────────────────────────────────────────────

static void set_status(GtkWidget* label, const std::string& msg, bool error = false) {
    gtk_label_set_text(GTK_LABEL(label), msg.c_str());
    gtk_widget_remove_css_class(label, "success");
    gtk_widget_remove_css_class(label, "error");
    gtk_widget_add_css_class(label, error ? "error" : "success");
}

static GtkWidget* section_label(const char* text) {
    GtkWidget* lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_add_css_class(lbl, "section-title");
    return lbl;
}

static void grid_row(GtkWidget* grid, int row, const char* key, GtkWidget** val_out) {
    GtkWidget* key_lbl = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(key_lbl), 1.0f);
    gtk_widget_set_opacity(key_lbl, 0.6);
    gtk_widget_set_margin_end(key_lbl, 6);
    gtk_grid_attach(GTK_GRID(grid), key_lbl, 0, row, 1, 1);

    GtkWidget* val_lbl = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(val_lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(val_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(val_lbl), TRUE);
    gtk_grid_attach(GTK_GRID(grid), val_lbl, 1, row, 1, 1);
    *val_out = val_lbl;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rule preview + writing
// ─────────────────────────────────────────────────────────────────────────────

static std::string selected_class_match() {
    const WindowInfo& w = App.windows[App.selected];
    return w.initialClass.empty() ? w.wclass : w.initialClass;
}

static std::string build_rule_preview() {
    if (App.selected < 0 || App.selected >= (int)App.windows.size())
        return "(select a window first)";

    std::string name = gtk_editable_get_text(GTK_EDITABLE(App.name_entry));
    guint sel        = gtk_drop_down_get_selected(App.ws_drop);
    bool do_float    = gtk_check_button_get_active(App.float_cb);
    bool do_fs       = gtk_check_button_get_active(App.fs_cb);
    bool do_monitor  = gtk_check_button_get_active(App.monitor_cb);
    std::string cls  = selected_class_match();

    int ws_id = 1;
    if (sel != GTK_INVALID_LIST_POSITION && sel < App.workspaces.size())
        ws_id = App.workspaces[sel].id;

    std::ostringstream out;
    out << "hl.window_rule({ name = \"" << name << "\", "
        << "match = { class = \"^(" << cls << ")$\" }, workspace = " << ws_id << " })\n";
    if (do_monitor && sel < App.workspaces.size())
        out << "hl.window_rule({ name = \"" << name << "-mon\", "
            << "match = { class = \"^(" << cls << ")$\" }, monitor = \"" << App.workspaces[sel].monitor << "\" })\n";
    if (do_float)
        out << "hl.window_rule({ name = \"" << name << "-float\", "
            << "match = { class = \"^(" << cls << ")$\" }, float = true })\n";
    if (do_fs)
        out << "hl.window_rule({ name = \"" << name << "-fs\", "
            << "match = { class = \"^(" << cls << ")$\" }, fullscreen = true })\n";

    // Snap category note (informational; the keyword is written to snap.json on Add)
    guint snap = gtk_drop_down_get_selected(App.snap_drop);
    if (snap == 1) out << "-- snap category: Emulator (→ ~/.config/hyprule/snap.json)\n";
    if (snap == 2) out << "-- snap category: Media (→ ~/.config/hyprule/snap.json)\n";
    return out.str();
}

static void update_preview() {
    gtk_label_set_text(GTK_LABEL(App.preview), build_rule_preview().c_str());
}

// Insert the rule after the last existing hl.window_rule(...) line so it sits with
// the others (native formatting, no branding). Falls back to EOF append.
static bool insert_window_rule(const std::string& rule) {
    std::string cfg = read_file(App.config_path);
    if (cfg.empty()) return false;

    std::istringstream ss(cfg);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);

    // Anchor on the last COMPLETE single-line rule (starts with hl.window_rule(
    // AND ends with "})" on the same line). This avoids splitting a multi-line
    // rule like the tablemaster/float blocks, and lands the new rule among the
    // single-line workspace-assignment rules where it belongs.
    int last_rule = -1;
    for (int i = 0; i < (int)lines.size(); i++) {
        size_t nb = lines[i].find_first_not_of(" \t");
        if (nb == std::string::npos) continue;
        std::string t = lines[i].substr(nb);
        size_t te = t.find_last_not_of(" \t\r");
        if (te != std::string::npos) t = t.substr(0, te + 1);
        if (t.compare(0, 15, "hl.window_rule(") == 0 &&
            t.size() >= 2 && t.compare(t.size() - 2, 2, "})") == 0)
            last_rule = i;
    }

    // rule may contain multiple lines; split & strip trailing newline
    std::vector<std::string> rule_lines;
    std::istringstream rs(rule);
    std::string rl;
    while (std::getline(rs, rl)) rule_lines.push_back(rl);

    if (last_rule >= 0)
        lines.insert(lines.begin() + last_rule + 1, rule_lines.begin(), rule_lines.end());
    else
        lines.insert(lines.end(), rule_lines.begin(), rule_lines.end());

    std::ostringstream outs;
    for (size_t i = 0; i < lines.size(); i++) outs << lines[i] << "\n";
    return write_file(App.config_path, outs.str());
}

// ─────────────────────────────────────────────────────────────────────────────
// Window Rules callbacks
// ─────────────────────────────────────────────────────────────────────────────

static void update_rules_indicator();   // defined below

static void on_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
    if (!row) { App.selected = -1; update_rules_indicator(); return; }
    App.selected = gtk_list_box_row_get_index(row);
    if (App.selected < 0 || App.selected >= (int)App.windows.size()) return;
    const WindowInfo& w = App.windows[App.selected];
    gtk_label_set_text(GTK_LABEL(App.class_val), w.wclass.c_str());
    gtk_label_set_text(GTK_LABEL(App.title_val), w.title.c_str());
    gtk_editable_set_text(GTK_EDITABLE(App.name_entry), make_rule_name(w.wclass).c_str());
    update_preview();
    update_rules_indicator();
}

static void on_option_changed(GObject*, GParamSpec*, gpointer) { update_preview(); }
static void on_check_toggled(GtkCheckButton*, gpointer)        { update_preview(); }
static void on_entry_changed(GtkEditable*, gpointer)           { update_preview(); }

// Workspace dropdown → auto-tick the monitor pin when a CRT workspace is chosen
static void on_ws_changed(GObject*, GParamSpec*, gpointer) {
    guint sel = gtk_drop_down_get_selected(App.ws_drop);
    if (sel != GTK_INVALID_LIST_POSITION && sel < App.workspaces.size())
        gtk_check_button_set_active(App.monitor_cb, App.workspaces[sel].is_crt);
    update_preview();
}

// Snap category → jump the workspace dropdown to that category's ws (11 Emulator / 12 Media)
static void on_snap_changed(GObject*, GParamSpec*, gpointer) {
    guint snap = gtk_drop_down_get_selected(App.snap_drop);
    int target = (snap == 1) ? App.emu_ws : (snap == 2) ? App.media_ws : -1;
    if (target > 0) {
        for (size_t i = 0; i < App.workspaces.size(); i++)
            if (App.workspaces[i].id == target) { gtk_drop_down_set_selected(App.ws_drop, i); break; }
    }
    update_preview();
}

static void populate_list() {
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(App.list_box)) != nullptr)
        gtk_list_box_remove(GTK_LIST_BOX(App.list_box), child);
    App.selected = -1;

    for (const WindowInfo& w : App.windows) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 6);
        gtk_widget_set_margin_bottom(box, 6);
        GtkWidget* lc = gtk_label_new(w.wclass.c_str());
        gtk_label_set_xalign(GTK_LABEL(lc), 0.0f);
        gtk_widget_add_css_class(lc, "window-class");
        GtkWidget* lt = gtk_label_new(w.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(lt), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lt), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(lt, "window-title");
        gtk_box_append(GTK_BOX(box), lc);
        gtk_box_append(GTK_BOX(box), lt);
        if (!w.assigned.empty()) {
            GtkWidget* la = gtk_label_new(w.assigned.c_str());
            gtk_label_set_xalign(GTK_LABEL(la), 0.0f);
            gtk_widget_add_css_class(la, "assigned-badge");
            gtk_box_append(GTK_BOX(box), la);
        }
        gtk_list_box_append(GTK_LIST_BOX(App.list_box), box);
    }
}

static void on_refresh(GtkButton*, gpointer) {
    App.windows = parse_clients(run_cmd("hyprctl clients -j 2>/dev/null"));
    std::string cfg = read_file(App.config_path);
    for (auto& w : App.windows)
        w.assigned = assignment_indicator(cfg, w.wclass, w.initialClass);
    populate_list();
    set_status(App.status, "Refreshed — " + std::to_string(App.windows.size()) + " windows.");
    update_preview();
}

static void on_add_rule(GtkButton*, gpointer) {
    if (App.selected < 0) { set_status(App.status, "No window selected.", true); return; }

    // 1) snap category → snap.json (before writing the rule, so both are consistent)
    guint snap = gtk_drop_down_get_selected(App.snap_drop);
    if (snap == 1 || snap == 2) {
        std::string cat = (snap == 1) ? "Emulator" : "Media";
        std::string cls = selected_class_match();
        run_cmd("hyprule-snap-add " + cat + " '" + cls + "' 2>/dev/null");
    }

    // 2) the window rule
    std::string rule = build_rule_preview();
    if (rule.empty() || rule.rfind("(select", 0) == 0) {
        set_status(App.status, "Nothing to write.", true);
        return;
    }
    if (!insert_window_rule(rule)) {
        set_status(App.status, "Could not write " + App.config_path, true);
        return;
    }
    set_status(App.status, "Rule written to hyprland.lua.");

    if (gtk_check_button_get_active(App.reload_cb)) {
        int ret = system("hyprctl reload >/dev/null 2>&1");
        set_status(App.status, ret == 0 ? "Rule written and Hyprland reloaded."
                                        : "Rule written, but hyprctl reload failed.", ret != 0);
    }
}

// Refresh the "rules currently set for this window" indicator + Remove button state.
static void update_rules_indicator() {
    if (!App.rules_val) return;
    if (App.selected < 0 || App.selected >= (int)App.windows.size()) {
        gtk_label_set_text(GTK_LABEL(App.rules_val), "—");
        gtk_widget_set_sensitive(App.remove_btn, FALSE);
        return;
    }
    const WindowInfo& w = App.windows[App.selected];
    std::string cfg = read_file(App.config_path);
    auto hits = associated_rules(cfg, w.wclass, w.initialClass);
    if (hits.empty()) {
        gtk_label_set_text(GTK_LABEL(App.rules_val), "No rules set for this app.");
        gtk_widget_set_sensitive(App.remove_btn, FALSE);
    } else {
        std::string s;
        for (size_t i = 0; i < hits.size(); i++) s += (i ? "\n" : "") + std::string("• ") + hits[i].summary;
        gtk_label_set_text(GTK_LABEL(App.rules_val), s.c_str());
        gtk_widget_set_sensitive(App.remove_btn, TRUE);
    }
}

// Remove every hl.window_rule line matching the selected window's class.
static void on_remove_rules(GtkButton*, gpointer) {
    if (App.selected < 0 || App.selected >= (int)App.windows.size()) return;
    std::string wc = App.windows[App.selected].wclass;   // copy before refresh invalidates
    std::string ic = App.windows[App.selected].initialClass;

    std::string cfg = read_file(App.config_path);
    auto hits = associated_rules(cfg, wc, ic);
    if (hits.empty()) { set_status(App.status, "No rules to remove.", true); return; }

    std::istringstream ss(cfg);
    std::vector<std::string> lines, out;
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);
    int removed = 0;
    for (auto& l : lines) {
        bool drop = false;
        for (auto& h : hits) if (h.text == l) { drop = true; break; }
        if (drop) { removed++; continue; }
        out.push_back(l);
    }
    std::ostringstream o;
    for (auto& l : out) o << l << "\n";
    if (!write_file(App.config_path, o.str())) { set_status(App.status, "Write failed.", true); return; }

    system("hyprctl reload >/dev/null 2>&1");
    set_status(App.status, "Removed " + std::to_string(removed) + " rule(s) for " + wc + ", reloaded.");
    on_refresh(nullptr, nullptr);   // re-reads config → indicators refresh
    update_rules_indicator();
}

// ─────────────────────────────────────────────────────────────────────────────
// Workspace Names tab — rename → write hyprland.lua + sync waybar
// ─────────────────────────────────────────────────────────────────────────────

// Replace default_name value on the hl.workspace_rule line for ws id.
static bool set_default_name(std::string& cfg, int id, const std::string& newname) {
    std::istringstream ss(cfg);
    std::vector<std::string> lines;
    std::string line;
    bool changed = false;
    while (std::getline(ss, line)) lines.push_back(line);
    std::string want = "workspace = \"" + std::to_string(id) + "\"";
    for (auto& l : lines) {
        if (l.find("hl.workspace_rule(") == std::string::npos) continue;
        if (l.find(want) == std::string::npos) continue;
        size_t p = l.find("default_name = \"");
        if (p == std::string::npos) continue;
        size_t vs = p + std::string("default_name = \"").size();
        size_t ve = l.find('"', vs);
        if (ve == std::string::npos) continue;
        l = l.substr(0, vs) + newname + l.substr(ve);
        changed = true;
    }
    if (!changed) return false;
    std::ostringstream outs;
    for (auto& l : lines) outs << l << "\n";
    cfg = outs.str();
    return true;
}

// Replace "<id>": "<value>" inside every waybar format-icons block.
static bool set_waybar_icon(std::string& wb, int id, const std::string& newname) {
    std::string key = "\"" + std::to_string(id) + "\"";
    bool changed = false;
    size_t search = 0;
    while (true) {
        size_t fi = wb.find("\"format-icons\"", search);
        if (fi == std::string::npos) break;
        size_t brace = wb.find('{', fi);
        if (brace == std::string::npos) break;
        size_t end = wb.find('}', brace);
        if (end == std::string::npos) break;
        // within [brace, end] find  "id" : "value"
        size_t kp = wb.find(key + ":", brace);
        if (kp == std::string::npos) kp = wb.find(key + " :", brace);
        if (kp != std::string::npos && kp < end) {
            size_t q1 = wb.find('"', wb.find(':', kp));
            if (q1 != std::string::npos && q1 < end) {
                size_t q2 = wb.find('"', q1 + 1);
                if (q2 != std::string::npos && q2 <= end + newname.size()) {
                    wb = wb.substr(0, q1 + 1) + newname + wb.substr(q2);
                    changed = true;
                }
            }
        }
        search = end + 1;
    }
    return changed;
}

static void on_apply_names(GtkButton*, gpointer) {
    std::string cfg = read_file(App.config_path);
    std::string wb  = read_file(App.waybar_path);
    if (cfg.empty()) { set_status(App.rename_status, "Could not read hyprland.lua", true); return; }

    int n = 0;
    for (auto& r : App.ws_rows) {
        std::string cur = gtk_editable_get_text(GTK_EDITABLE(r.entry));
        if (cur == r.original) continue;
        bool a = set_default_name(cfg, r.id, cur);
        if (!wb.empty()) set_waybar_icon(wb, r.id, cur);
        if (a) { r.original = cur; n++; }
    }
    if (n == 0) { set_status(App.rename_status, "No changes to apply."); return; }

    bool okc = write_file(App.config_path, cfg);
    if (!wb.empty()) write_file(App.waybar_path, wb);
    if (!okc) { set_status(App.rename_status, "Write to hyprland.lua failed.", true); return; }

    system("hyprctl reload >/dev/null 2>&1");
    system("killall -SIGUSR2 waybar >/dev/null 2>&1");
    set_status(App.rename_status,
        "Renamed " + std::to_string(n) + " workspace(s). Hyprland reloaded, waybar refreshed. "
        "(hyprland names show fully after the ws is next re-created.)");
}

// ─────────────────────────────────────────────────────────────────────────────
// UI construction — tabs
// ─────────────────────────────────────────────────────────────────────────────

static GtkWidget* build_rules_tab() {
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 300);

    // Left: window list
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 280, -1);
    App.list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(App.list_box), GTK_SELECTION_SINGLE);
    gtk_widget_set_vexpand(App.list_box, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), App.list_box);
    gtk_paned_set_start_child(GTK_PANED(paned), scroll);

    // Right: options
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(right, 12); gtk_widget_set_margin_end(right, 12);
    gtk_widget_set_margin_top(right, 8);    gtk_widget_set_margin_bottom(right, 8);
    gtk_paned_set_end_child(GTK_PANED(paned), right);

    gtk_box_append(GTK_BOX(right), section_label("SELECTED WINDOW"));
    GtkWidget* info = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(info), 8);
    gtk_grid_set_row_spacing(GTK_GRID(info), 4);
    gtk_widget_set_margin_bottom(info, 8);
    grid_row(info, 0, "Class:", &App.class_val);
    grid_row(info, 1, "Title:", &App.title_val);
    gtk_box_append(GTK_BOX(right), info);

    gtk_box_append(GTK_BOX(right), section_label("RULES SET FOR THIS APP"));
    App.rules_val = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(App.rules_val), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(App.rules_val), TRUE);
    gtk_label_set_selectable(GTK_LABEL(App.rules_val), TRUE);
    gtk_widget_set_opacity(App.rules_val, 0.85);
    gtk_box_append(GTK_BOX(right), App.rules_val);
    App.remove_btn = gtk_button_new_with_label("Remove This App's Rules");
    gtk_widget_add_css_class(App.remove_btn, "destructive-action");
    gtk_widget_set_halign(App.remove_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top(App.remove_btn, 4);
    gtk_widget_set_margin_bottom(App.remove_btn, 8);
    gtk_widget_set_sensitive(App.remove_btn, FALSE);
    gtk_box_append(GTK_BOX(right), App.remove_btn);
    g_signal_connect(App.remove_btn, "clicked", G_CALLBACK(on_remove_rules), nullptr);

    gtk_box_append(GTK_BOX(right), section_label("RULE OPTIONS"));
    GtkWidget* og = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(og), 8);
    gtk_grid_set_row_spacing(GTK_GRID(og), 6);
    gtk_widget_set_margin_bottom(og, 8);

    GtkWidget* nl = gtk_label_new("Rule name:");
    gtk_label_set_xalign(GTK_LABEL(nl), 1.0f); gtk_widget_set_opacity(nl, 0.6); gtk_widget_set_margin_end(nl, 6);
    gtk_grid_attach(GTK_GRID(og), nl, 0, 0, 1, 1);
    App.name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(App.name_entry, TRUE);
    gtk_grid_attach(GTK_GRID(og), App.name_entry, 1, 0, 2, 1);

    GtkWidget* wl = gtk_label_new("Workspace:");
    gtk_label_set_xalign(GTK_LABEL(wl), 1.0f); gtk_widget_set_opacity(wl, 0.6); gtk_widget_set_margin_end(wl, 6);
    gtk_grid_attach(GTK_GRID(og), wl, 0, 1, 1, 1);
    GtkStringList* ws_list = gtk_string_list_new(nullptr);
    for (const auto& w : App.workspaces) gtk_string_list_append(ws_list, ws_label(w).c_str());
    App.ws_drop = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(ws_list), nullptr));
    gtk_drop_down_set_selected(App.ws_drop, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(App.ws_drop), TRUE);
    gtk_grid_attach(GTK_GRID(og), GTK_WIDGET(App.ws_drop), 1, 1, 2, 1);

    GtkWidget* sl = gtk_label_new("Snap category:");
    gtk_label_set_xalign(GTK_LABEL(sl), 1.0f); gtk_widget_set_opacity(sl, 0.6); gtk_widget_set_margin_end(sl, 6);
    gtk_grid_attach(GTK_GRID(og), sl, 0, 2, 1, 1);
    std::string emu_lbl   = "Emulator (CRT ws" + std::to_string(App.emu_ws) + ")";
    std::string media_lbl = "Media (CRT ws" + std::to_string(App.media_ws) + ")";
    const char* snaps[] = { "None", emu_lbl.c_str(), media_lbl.c_str(), nullptr };
    GtkStringList* snap_list = gtk_string_list_new(snaps);
    App.snap_drop = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(snap_list), nullptr));
    gtk_drop_down_set_selected(App.snap_drop, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(App.snap_drop), TRUE);
    gtk_grid_attach(GTK_GRID(og), GTK_WIDGET(App.snap_drop), 1, 2, 2, 1);
    gtk_box_append(GTK_BOX(right), og);

    App.float_cb   = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Float window"));
    App.fs_cb      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Fullscreen"));
    App.monitor_cb = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Also pin to that workspace's monitor"));
    App.reload_cb  = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Reload Hyprland after adding"));
    gtk_check_button_set_active(App.reload_cb, TRUE);
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(App.float_cb));
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(App.fs_cb));
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(App.monitor_cb));
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(App.reload_cb));

    gtk_box_append(GTK_BOX(right), section_label("GENERATED RULE PREVIEW"));
    GtkWidget* pscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(pscroll, TRUE);
    gtk_widget_set_size_request(pscroll, -1, 84);
    App.preview = gtk_label_new("(select a window from the list)");
    gtk_label_set_xalign(GTK_LABEL(App.preview), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(App.preview), 0.0f);
    gtk_widget_add_css_class(App.preview, "preview-box");
    gtk_label_set_selectable(GTK_LABEL(App.preview), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pscroll), App.preview);
    gtk_widget_set_margin_top(pscroll, 4);
    gtk_widget_set_margin_bottom(pscroll, 10);
    gtk_box_append(GTK_BOX(right), pscroll);

    GtkWidget* add = gtk_button_new_with_label("Add Rule");
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_margin_bottom(add, 6);
    gtk_box_append(GTK_BOX(right), add);

    App.status = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(App.status), 0.0f);
    gtk_widget_set_opacity(App.status, 0.75);
    gtk_label_set_wrap(GTK_LABEL(App.status), TRUE);
    gtk_box_append(GTK_BOX(right), App.status);

    g_signal_connect(App.list_box,   "row-selected",     G_CALLBACK(on_row_selected),  nullptr);
    g_signal_connect(add,            "clicked",          G_CALLBACK(on_add_rule),      nullptr);
    g_signal_connect(App.ws_drop,    "notify::selected", G_CALLBACK(on_ws_changed),    nullptr);
    g_signal_connect(App.snap_drop,  "notify::selected", G_CALLBACK(on_snap_changed),  nullptr);
    g_signal_connect(App.float_cb,   "toggled",          G_CALLBACK(on_check_toggled), nullptr);
    g_signal_connect(App.fs_cb,      "toggled",          G_CALLBACK(on_check_toggled), nullptr);
    g_signal_connect(App.monitor_cb, "toggled",          G_CALLBACK(on_check_toggled), nullptr);
    g_signal_connect(App.name_entry, "changed",          G_CALLBACK(on_entry_changed), nullptr);
    return paned;
}

static GtkWidget* build_workspaces_tab() {
    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(outer, 14); gtk_widget_set_margin_end(outer, 14);
    gtk_widget_set_margin_top(outer, 10);   gtk_widget_set_margin_bottom(outer, 10);

    GtkWidget* hint = gtk_label_new(
        "Rename any workspace — writes hyprland.lua default_name and syncs the waybar bars. "
        "Keep the leading nerd-font glyph if you want an icon.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_opacity(hint, 0.6);
    gtk_widget_set_margin_bottom(hint, 10);
    gtk_box_append(GTK_BOX(outer), hint);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
    gtk_box_append(GTK_BOX(outer), scroll);

    App.ws_rows.clear();
    int row = 0;
    for (const auto& w : App.workspaces) {
        GtkWidget* id = gtk_label_new((std::to_string(w.id)).c_str());
        gtk_label_set_xalign(GTK_LABEL(id), 1.0f);
        gtk_widget_add_css_class(id, "ws-id");
        gtk_widget_set_size_request(id, 24, -1);
        gtk_grid_attach(GTK_GRID(grid), id, 0, row, 1, 1);

        GtkWidget* ent = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(ent), w.name.c_str());
        gtk_widget_set_hexpand(ent, TRUE);
        gtk_widget_set_size_request(ent, 320, -1);
        gtk_grid_attach(GTK_GRID(grid), ent, 1, row, 1, 1);

        GtkWidget* badge = gtk_label_new(w.is_crt ? w.monitor.c_str() : "");
        gtk_label_set_xalign(GTK_LABEL(badge), 0.0f);
        gtk_widget_add_css_class(badge, "crt-badge");
        gtk_grid_attach(GTK_GRID(grid), badge, 2, row, 1, 1);

        App.ws_rows.push_back({ w.id, w.name, ent });
        row++;
    }

    GtkWidget* apply = gtk_button_new_with_label("Apply Workspace Names");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_widget_set_margin_top(apply, 12);
    gtk_widget_set_halign(apply, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(outer), apply);

    App.rename_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(App.rename_status), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(App.rename_status), TRUE);
    gtk_widget_set_opacity(App.rename_status, 0.8);
    gtk_widget_set_margin_top(App.rename_status, 6);
    gtk_box_append(GTK_BOX(outer), App.rename_status);

    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_names), nullptr);
    return outer;
}

static void build_ui(GtkApplication* app) {
    GtkWidget* win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "HyprRule");
    gtk_window_set_default_size(GTK_WINDOW(win), 860, 600);

    GtkWidget* header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);
    GtkWidget* refresh = gtk_button_new_with_label("⟳  Refresh");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), refresh);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), nullptr);

    GtkWidget* nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_rules_tab(),      gtk_label_new("Window Rules"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_workspaces_tab(), gtk_label_new("Workspace Names"));
    gtk_window_set_child(GTK_WINDOW(win), nb);

    on_refresh(nullptr, nullptr);
    gtk_window_present(GTK_WINDOW(win));
}

// Read the CRT monitor name and the two category workspaces from snap.json (the
// same file the snap daemon uses). Missing file or keys keep the defaults.
static void load_snap_settings() {
    std::string snap = read_file(App.snap_path);
    if (snap.empty()) return;
    std::string mon = json_str(snap, "monitor");
    if (!mon.empty()) App.crt_monitor = mon;
    auto cat_ws = [&](const char* cat, int fallback) {
        size_t p = snap.find(std::string("\"") + cat + "\"");
        if (p == std::string::npos) return fallback;
        int ws = json_int(snap.substr(p), "ws");
        return ws > 0 ? ws : fallback;
    };
    App.emu_ws   = cat_ws("Emulator", App.emu_ws);
    App.media_ws = cat_ws("Media",    App.media_ws);
}

// ─────────────────────────────────────────────────────────────────────────────
// Activation
// ─────────────────────────────────────────────────────────────────────────────

static void on_activate(GtkApplication* app, gpointer) {
    // Prefer the theme's dark variant so widgets (entries, dropdowns, buttons)
    // go dark cohesively; the pywal CSS below is the backstop for the base colors.
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, build_css().c_str());
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    const std::string conf = g_get_user_config_dir();   // $XDG_CONFIG_HOME or ~/.config
    App.config_path = conf + "/hypr/hyprland.lua";
    App.waybar_path = conf + "/waybar/config.jsonc";
    App.snap_path   = conf + "/hyprule/snap.json";
    load_snap_settings();
    App.workspaces  = parse_workspaces(read_file(App.config_path));

    build_ui(app);
}

int main(int argc, char* argv[]) {
    GtkApplication* app = gtk_application_new("io.github.ciphernovaa.hyprule", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
