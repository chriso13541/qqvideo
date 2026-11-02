/* file_browser.cpp -- see file_browser.h.
 *
 * Layout (a modal over the player, square and bordered like the control
 * bar):
 *
 *   [<][>][^][R]  [ /home/chris/Videos                          ]
 *   +-----------+------------------------------------------------+
 *   | Home      | Name                      Size    Modified     |
 *   | Desktop   | [] Shows                          2026-10-30   |
 *   | Videos    | >  movie.mkv               1.4 GB 2026-10-29   |
 *   | Downloads |                                                |
 *   | /  , C:\  |                                                |
 *   | Recent    |                                                |
 *   +-----------+------------------------------------------------+
 *   File: [ movie.mkv                ] [Video files v] [Open][Cancel]
 *
 * Keys: Up/Down/PgUp/PgDn/Home/End move, Enter opens, Backspace goes up a
 * folder, Alt+Left/Right go back/forward, Esc cancels, typing a letter
 * jumps to the next name starting with it.
 */
#include "file_browser.h"

#include "imgui.h"
#include "lumen_icons.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace fs = std::filesystem;

/* ---- UTF-8 <-> fs::path --------------------------------------------------
 * On Windows fs::path is UTF-16 inside; these keep everything we hand
 * to ImGui and the rest of qqvideo in UTF-8. On Linux they're no-ops. */
static fs::path u8_to_path(const std::string &s) {
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

static std::string path_to_u8(const fs::path &p) {
#if defined(_WIN32)
    const std::wstring &w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
#else
    return p.native();
#endif
}

/* ---- file types -------------------------------------------------------- */
static const char *const MEDIA_EXTS[] = {
    "mp4", "m4v", "mov", "m4a", "3gp", "mkv", "mka", "webm", "avi", "ts", "m2ts",
    "ogg", "ogv", "opus", "flac", "mp3", "aac", "h264", "264", NULL
};
static const char *const SUB_EXTS[] = { "srt", "ass", "ssa", "vtt", NULL };

static bool ext_in(const char *path, const char *const *list) {
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    const char *sep = strrchr(path, '/');
#if defined(_WIN32)
    const char *bs = strrchr(path, '\\');
    if (bs && (!sep || bs > sep)) sep = bs;
#endif
    if (!dot || (sep && dot < sep)) return false;
    dot++;
    for (int i = 0; list[i]; i++) {
        const char *a = dot, *b = list[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return true;
    }
    return false;
}
bool lumen_fb_is_media_ext(const char *path)    { return ext_in(path, MEDIA_EXTS); }
bool lumen_fb_is_subtitle_ext(const char *path) { return ext_in(path, SUB_EXTS); }

enum { FILTER_MEDIA = 0, FILTER_SUBS = 1, FILTER_ALL = 2 };
static const char *const FILTER_NAMES[] = { "Video and audio files", "Subtitle files", "All files" };

/* ---- state ------------------------------------------------------------- */
struct Entry {
    std::string name;      /* UTF-8, for display and sorting */
    bool        dir;
    bool        hidden;
    uintmax_t   size;      /* bytes, files only */
    time_t      mtime;     /* 0 = unknown */
    int         type;      /* 0 other, 1 media, 2 subtitle */
};

struct Place { std::string label, path; const char *icon; };

struct lumen_file_browser {
    std::string config_file;       /* "" = don't persist */
    bool        open        = false;
    bool        just_opened = false;
    std::string title;
    int         filter      = FILTER_MEDIA;
    bool        show_hidden = false;

    std::string cwd;               /* UTF-8 */
    std::vector<Entry> entries;    /* everything in cwd */
    std::vector<int>   view;       /* indices into entries: filtered + sorted */
    bool        view_dirty = true;
    std::string error;             /* "Permission denied" etc. for cwd */
    int         selected   = -1;   /* index into view */
    bool        scroll_to_sel = false;

    int         sort_col = 0;      /* 0 name, 1 size, 2 modified */
    bool        sort_desc = false;

    std::vector<std::string> back, fwd;
    std::vector<std::string> recent;    /* most recent first */
    std::string last_dir;

    char path_buf[1024] = "";      /* the editable location bar */
    char name_buf[1024] = "";      /* the "File:" box */
    bool path_editing   = false;
};

/* ---- persistence: file_browser.ini ------------------------------------- */
static void load_state(lumen_file_browser *fb) {
    if (fb->config_file.empty()) return;
    FILE *f = fopen(fb->config_file.c_str(), "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (!strncmp(line, "last_dir = ", 11)) fb->last_dir = line + 11;
        else if (!strncmp(line, "recent = ", 9) && fb->recent.size() < 8) fb->recent.push_back(line + 9);
        else if (!strncmp(line, "show_hidden = ", 14)) fb->show_hidden = atoi(line + 14) != 0;
    }
    fclose(f);
}

static void save_state(lumen_file_browser *fb) {
    if (fb->config_file.empty()) return;
    std::error_code ec;
    fs::create_directories(u8_to_path(fb->config_file).parent_path(), ec);
    FILE *f = fopen(fb->config_file.c_str(), "w");
    if (!f) return;
    fprintf(f, "# qqvideo file browser -- last folder and recent folders\n");
    fprintf(f, "last_dir = %s\n", fb->last_dir.c_str());
    fprintf(f, "show_hidden = %d\n", fb->show_hidden ? 1 : 0);
    for (auto &r : fb->recent) fprintf(f, "recent = %s\n", r.c_str());
    fclose(f);
}

static void remember_recent(lumen_file_browser *fb, const std::string &dir) {
    fb->recent.erase(std::remove(fb->recent.begin(), fb->recent.end(), dir), fb->recent.end());
    fb->recent.insert(fb->recent.begin(), dir);
    if (fb->recent.size() > 8) fb->recent.resize(8);
}

/* ---- places ------------------------------------------------------------ */
static std::string home_dir() {
#if defined(_WIN32)
    const char *h = getenv("USERPROFILE");
    if (h && *h) return h;          /* getenv is ANSI; fine for typical profiles */
    wchar_t w[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", w, MAX_PATH);
    if (n && n < MAX_PATH) return path_to_u8(fs::path(w));
    return "C:\\";
#else
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/";
#endif
}

static bool is_dir(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(u8_to_path(p), ec);
}

/* XDG user dirs ("Videos" may be "Vidéos" or anything else); falls back
 * to ~/<english name>. */
static std::string xdg_user_dir(const char *key, const char *fallback) {
    std::string home = home_dir();
#if !defined(_WIN32)
    const char *cfg = getenv("XDG_CONFIG_HOME");
    std::string file = (cfg && *cfg) ? std::string(cfg) + "/user-dirs.dirs" : home + "/.config/user-dirs.dirs";
    if (FILE *f = fopen(file.c_str(), "r")) {
        char line[1024];
        std::string want = std::string("XDG_") + key + "_DIR=\"";
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, want.c_str(), want.size())) continue;
            std::string v = line + want.size();
            size_t q = v.find('"');
            if (q != std::string::npos) v.resize(q);
            if (!v.compare(0, 5, "$HOME")) v = home + v.substr(5);
            fclose(f);
            return v;
        }
        fclose(f);
    }
#else
    (void)key;
#endif
    return path_to_u8(u8_to_path(home) / fallback);
}

static std::vector<Place> build_places() {
    std::vector<Place> p;
    std::string home = home_dir();
    p.push_back({ "Home", home, ICON_HOME });
    struct { const char *key, *fallback, *label, *icon; } user_dirs[] = {
        { "DESKTOP",  "Desktop",   "Desktop",   ICON_COMPUTER },
        { "VIDEOS",   "Videos",    "Videos",    ICON_FILM },
        { "DOWNLOAD", "Downloads", "Downloads", ICON_DOWNLOAD },
    };
    for (auto &u : user_dirs) {
        std::string d = xdg_user_dir(u.key, u.fallback);
        if (d != home && is_dir(d)) p.push_back({ u.label, d, u.icon });
    }
#if defined(_WIN32)
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        char root[4] = { (char)('A' + i), ':', '\\', '\0' };
        p.push_back({ std::string(root, 2), root, ICON_DRIVE });
    }
#else
    p.push_back({ "/", "/", ICON_DRIVE });
    /* Removable drives and other mounts: the folders inside /run/media/$USER,
     * /media/$USER, /media and /mnt -- whatever exists. */
    const char *user = getenv("USER");
    std::vector<std::string> roots = { "/mnt", "/media" };
    if (user && *user) {
        roots.push_back(std::string("/run/media/") + user);
        roots.push_back(std::string("/media/") + user);
    }
    for (auto &r : roots) {
        std::error_code ec;
        for (fs::directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (!it->is_directory(ec2)) continue;
            std::string full = path_to_u8(it->path());
            if (user && full == std::string("/media/") + user) continue;   /* listed via its children */
            fs::path pp = it->path();
            if (fs::is_empty(pp, ec2)) continue;   /* skip empty mount points */
            p.push_back({ path_to_u8(pp.filename()), full, ICON_DRIVE });
        }
    }
#endif
    return p;
}

/* ---- listing ----------------------------------------------------------- */
static time_t to_time_t(fs::file_time_type ft) {
    /* C++17 has no clock_cast; shift by the two clocks' "now". Accurate to
     * well under a second, which is all a "Modified" column needs. */
    using namespace std::chrono;
    auto sys = time_point_cast<system_clock::duration>(ft - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sys);
}

static void load_dir(lumen_file_browser *fb, const std::string &dir) {
    fb->cwd = dir;
    fb->entries.clear();
    fb->error.clear();
    fb->selected = -1;
    fb->view_dirty = true;
    snprintf(fb->path_buf, sizeof(fb->path_buf), "%s", dir.c_str());

    std::error_code ec;
    fs::directory_iterator it(u8_to_path(dir), fs::directory_options::skip_permission_denied, ec), end;
    if (ec) { fb->error = ec.message(); return; }
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        Entry e;
        e.name = path_to_u8(it->path().filename());
        std::error_code e2;
        e.dir = it->is_directory(e2);
        e.hidden = !e.name.empty() && e.name[0] == '.';
#if defined(_WIN32)
        DWORD attr = GetFileAttributesW(it->path().c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) e.hidden = true;
#endif
        e.size = (!e.dir && it->is_regular_file(e2)) ? it->file_size(e2) : 0;
        if (e2) e.size = 0;
        std::error_code e3;
        auto ft = it->last_write_time(e3);
        e.mtime = e3 ? 0 : to_time_t(ft);
        e.type = e.dir ? 0 : lumen_fb_is_media_ext(e.name.c_str()) ? 1
               : lumen_fb_is_subtitle_ext(e.name.c_str()) ? 2 : 0;
        fb->entries.push_back(std::move(e));
    }
}

static bool ci_less(const std::string &a, const std::string &b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

static void rebuild_view(lumen_file_browser *fb) {
    std::string keep = (fb->selected >= 0 && fb->selected < (int)fb->view.size())
                       ? fb->entries[fb->view[fb->selected]].name : std::string();
    fb->view.clear();
    for (int i = 0; i < (int)fb->entries.size(); i++) {
        const Entry &e = fb->entries[i];
        if (e.hidden && !fb->show_hidden) continue;
        if (!e.dir) {
            if (fb->filter == FILTER_MEDIA && e.type != 1) continue;
            if (fb->filter == FILTER_SUBS  && e.type != 2) continue;
        }
        fb->view.push_back(i);
    }
    const auto &E = fb->entries;
    int col = fb->sort_col; bool desc = fb->sort_desc;
    std::stable_sort(fb->view.begin(), fb->view.end(), [&](int a, int b) {
        const Entry &x = E[a], &y = E[b];
        if (x.dir != y.dir) return x.dir;                      /* folders first, always */
        bool lt, gt;
        if (col == 1)      { lt = x.size < y.size;   gt = y.size < x.size; }
        else if (col == 2) { lt = x.mtime < y.mtime; gt = y.mtime < x.mtime; }
        else               { lt = ci_less(x.name, y.name); gt = ci_less(y.name, x.name); }
        if (!lt && !gt) return ci_less(x.name, y.name);        /* ties by name */
        return desc ? gt : lt;
    });
    fb->selected = -1;
    if (!keep.empty())
        for (int i = 0; i < (int)fb->view.size(); i++)
            if (E[fb->view[i]].name == keep) { fb->selected = i; break; }
    fb->view_dirty = false;
}

static std::string join(const std::string &dir, const std::string &name) {
    return path_to_u8(u8_to_path(dir) / u8_to_path(name));
}

static void navigate(lumen_file_browser *fb, const std::string &dir, bool record = true) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(u8_to_path(dir), ec);
    std::string d = ec ? dir : path_to_u8(p);
    if (!is_dir(d)) return;
    if (record && !fb->cwd.empty() && d != fb->cwd) { fb->back.push_back(fb->cwd); fb->fwd.clear(); }
    load_dir(fb, d);
    fb->name_buf[0] = '\0';
}

static std::string parent_of(const std::string &dir) {
    fs::path p = u8_to_path(dir);
    fs::path par = p.parent_path();
    if (par.empty() || par == p) return dir;
    return path_to_u8(par);
}

/* ---- public API -------------------------------------------------------- */
lumen_file_browser *lumen_fb_create(const char *config_dir) {
    lumen_file_browser *fb = new lumen_file_browser();
    if (config_dir && *config_dir) fb->config_file = join(config_dir, "file_browser.ini");
    load_state(fb);
    return fb;
}

void lumen_fb_destroy(lumen_file_browser *fb) { delete fb; }

bool lumen_fb_is_open(const lumen_file_browser *fb) { return fb && fb->open; }

void lumen_fb_open(lumen_file_browser *fb, const char *title, lumen_fb_kind kind, const char *start_dir) {
    if (!fb) return;
    fb->title = title ? title : "Open";
    fb->filter = kind == LUMEN_FB_SUBTITLES ? FILTER_SUBS : FILTER_MEDIA;
    fb->open = true;
    fb->just_opened = true;
    fb->back.clear(); fb->fwd.clear();
    fb->name_buf[0] = '\0';
    std::string start = (start_dir && *start_dir && is_dir(start_dir)) ? std::string(start_dir)
                      : is_dir(fb->last_dir) ? fb->last_dir : home_dir();
    fb->cwd.clear();
    navigate(fb, start, false);
}

static void finish(lumen_file_browser *fb) {
    fb->last_dir = fb->cwd;
    fb->open = false;
    save_state(fb);
    ImGui::CloseCurrentPopup();
}

/* Open whatever the File box / selection names: a folder navigates, a
 * file is the answer. Returns true when a file was picked. */
static bool activate(lumen_file_browser *fb, std::string target, char *out, size_t out_size) {
    if (target.empty()) return false;
    fs::path tp = u8_to_path(target);
    if (tp.is_relative()) target = join(fb->cwd, target);
    if (is_dir(target)) { navigate(fb, target); return false; }
    std::error_code ec;
    if (!fs::exists(u8_to_path(target), ec)) {
        fb->error = "Not found: " + target;
        return false;
    }
    if (target.size() >= out_size) {       /* would be cut short into a wrong path */
        fb->error = "Path too long for qqvideo (" + std::to_string(target.size()) + " bytes, max " +
                    std::to_string(out_size - 1) + ")";
        return false;
    }
    snprintf(out, out_size, "%s", target.c_str());
    remember_recent(fb, fb->cwd);
    finish(fb);
    return true;
}

static void fmt_size(uintmax_t b, char *out, size_t n) {
    if (b < 1024) snprintf(out, n, "%u B", (unsigned)b);
    else if (b < 1024u * 1024u) snprintf(out, n, "%.0f KB", b / 1024.0);
    else if (b < 1024ull * 1024u * 1024u) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(out, n, "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
}

static bool square_button(const char *label, const char *tip, bool enabled = true) {
    if (!enabled) ImGui::BeginDisabled();
    bool r = ImGui::Button(label, ImVec2(ImGui::GetFrameHeight() + 6.0f, 0));
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    if (!enabled) ImGui::EndDisabled();
    return r && enabled;
}

lumen_fb_result lumen_fb_draw(lumen_file_browser *fb, char *out, size_t out_size) {
    if (!fb || !fb->open) return LUMEN_FB_NONE;
    lumen_fb_result result = LUMEN_FB_NONE;

    ImGuiIO &io = ImGui::GetIO();
    std::string popup_id = fb->title + "###lumen_file_browser";
    if (fb->just_opened) {
        ImGui::OpenPopup(popup_id.c_str());
        /* Fill most of the window, within sensible bounds. */
        float w = std::min(760.0f, io.DisplaySize.x - 24.0f);
        float h = std::min(500.0f, io.DisplaySize.y - 24.0f);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 260), ImVec2(io.DisplaySize.x, io.DisplaySize.y));

    /* Square, bordered, like the control bar and an old common dialog. */
    /* Square corners; the frames themselves are the retro bevels drawn by
     * retro_style.cpp (raised dialog, sunken panes and text boxes). */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    /* Fully opaque: the light theme's popup background is slightly
     * see-through, which lets the moving video bleed into the dialog. */
    ImVec4 face = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg); face.w = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_PopupBg, face);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 1));   /* white panes, like a list view */

    bool keep_open = true;
    if (!ImGui::BeginPopupModal(popup_id.c_str(), &keep_open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(6);
        ImGui::PopStyleColor(2);
        if (fb->open && !fb->just_opened) {   /* closed via the title-bar X */
            fb->last_dir = fb->cwd; fb->open = false; save_state(fb);
            return LUMEN_FB_CANCELLED;
        }
        fb->just_opened = false;
        return LUMEN_FB_NONE;
    }
    bool first_frame = fb->just_opened;
    fb->just_opened = false;
    if (fb->view_dirty) rebuild_view(fb);

    /* ---- toolbar: back, forward, up, refresh, location ---------------- */
    if (square_button(ICON_ARROW_LEFT "##back", "Back (Alt+Left)", !fb->back.empty()) ||
        (!fb->back.empty() && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) {
        fb->fwd.push_back(fb->cwd);
        std::string d = fb->back.back(); fb->back.pop_back();
        navigate(fb, d, false);
    }
    ImGui::SameLine(0, 2);
    if (square_button(ICON_ARROW_RIGHT "##fwd", "Forward (Alt+Right)", !fb->fwd.empty()) ||
        (!fb->fwd.empty() && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow))) {
        fb->back.push_back(fb->cwd);
        std::string d = fb->fwd.back(); fb->fwd.pop_back();
        navigate(fb, d, false);
    }
    ImGui::SameLine(0, 2);
    std::string up = parent_of(fb->cwd);
    if (square_button(ICON_ARROW_UP "##up", "Up one folder (Backspace)", up != fb->cwd))
        navigate(fb, up);
    ImGui::SameLine(0, 2);
    if (square_button(ICON_REFRESH "##refresh", "Refresh")) navigate(fb, fb->cwd, false);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##location", fb->path_buf, sizeof(fb->path_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        std::string target = fb->path_buf;
        if (!target.empty() && target[0] == '~') target = home_dir() + target.substr(1);
        if (activate(fb, target, out, out_size)) result = LUMEN_FB_PICKED;
        else if (!is_dir(target)) snprintf(fb->path_buf, sizeof(fb->path_buf), "%s", fb->cwd.c_str());
    }
    fb->path_editing = ImGui::IsItemActive();

    /* ---- body: places | listing -------------------------------------- */
    float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.0f + 4.0f;
    float body_h = ImGui::GetContentRegionAvail().y - footer_h;
    if (body_h < 60) body_h = 60;

    static std::vector<Place> places;               /* drives/mounts: rebuilt on each open */
    if (first_frame || places.empty()) places = build_places();

    ImGui::BeginChild("##places", ImVec2(150, body_h), ImGuiChildFlags_Borders);
    for (auto &pl : places) {
        char lbl[600];
        snprintf(lbl, sizeof(lbl), "%s  %s##pl_%s", pl.icon, pl.label.c_str(), pl.path.c_str());
        if (ImGui::Selectable(lbl, pl.path == fb->cwd)) navigate(fb, pl.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pl.path.c_str());
    }
    bool any_recent = false;
    for (auto &r : fb->recent) {
        if (!is_dir(r)) continue;
        if (!any_recent) { ImGui::Spacing(); ImGui::TextDisabled(ICON_HISTORY "  Recent"); any_recent = true; }
        std::string nm = path_to_u8(u8_to_path(r).filename());
        if (nm.empty()) nm = r;
        char lbl[600];
        snprintf(lbl, sizeof(lbl), "%s  %s##rc_%s", ICON_FOLDER, nm.c_str(), r.c_str());
        if (ImGui::Selectable(lbl, r == fb->cwd)) navigate(fb, r);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##listing", ImVec2(0, body_h), ImGuiChildFlags_Borders);
    bool picked_dir = false;
    std::string open_target;
    if (!fb->error.empty()) ImGui::TextColored(ImVec4(0.75f, 0.1f, 0.1f, 1), "%s", fb->error.c_str());
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##files", 3, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0, 0);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 76, 1);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_PreferSortDescending, 120, 2);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsDirty && ss->SpecsCount > 0) {
                fb->sort_col  = (int)ss->Specs[0].ColumnUserID;
                fb->sort_desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                rebuild_view(fb);
                ss->SpecsDirty = false;
            }
        }

        if (fb->view.empty() && fb->error.empty()) {   /* say why it's empty, inside the list */
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled(fb->filter == FILTER_ALL || fb->entries.empty()
                                ? "(empty folder)" : "(nothing here matches the file type)");
        }
        ImGuiListClipper clip;
        clip.Begin((int)fb->view.size());
        if (fb->scroll_to_sel && fb->selected >= 0) clip.IncludeItemByIndex(fb->selected);
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const Entry &e = fb->entries[fb->view[row]];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                bool sel = row == fb->selected;
                if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns |
                                                    ImGuiSelectableFlags_AllowDoubleClick)) {
                    fb->selected = row;
                    if (!e.dir) snprintf(fb->name_buf, sizeof(fb->name_buf), "%s", e.name.c_str());
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        open_target = join(fb->cwd, e.name);
                        picked_dir = e.dir;
                    }
                }
                if (sel && fb->scroll_to_sel) { ImGui::SetScrollHereY(0.5f); fb->scroll_to_sel = false; }
                ImGui::SameLine(0, 0);
                ImVec4 icol = e.dir      ? ImVec4(0.80f, 0.60f, 0.10f, 1)   /* manila folder */
                            : e.type == 1 ? ImVec4(0.20f, 0.42f, 0.80f, 1)
                            : e.type == 2 ? ImVec4(0.25f, 0.55f, 0.25f, 1)
                            :               ImVec4(0.45f, 0.45f, 0.45f, 1);
                if (e.hidden) icol.w = 0.5f;
                ImGui::TextColored(icol, "%s", e.dir ? ICON_FOLDER : e.type == 1 ? ICON_FILE_VIDEO
                                                   : e.type == 2 ? ICON_FILE_TEXT : ICON_FILE);
                ImGui::SameLine();
                if (e.hidden) ImGui::TextDisabled("%s", e.name.c_str());
                else          ImGui::TextUnformatted(e.name.c_str());

                ImGui::TableNextColumn();
                if (!e.dir) {
                    char sz[32]; fmt_size(e.size, sz, sizeof(sz));
                    float w = ImGui::CalcTextSize(sz).x;   /* right-align sizes */
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
                    ImGui::TextUnformatted(sz);
                }
                ImGui::TableNextColumn();
                if (e.mtime) {
                    char ts[32];
                    struct tm tmv;
#if defined(_WIN32)
                    localtime_s(&tmv, &e.mtime);
#else
                    localtime_r(&e.mtime, &tmv);
#endif
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tmv);
                    ImGui::TextDisabled("%s", ts);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    /* ---- keyboard (when no text box is being typed in) ---------------- */
    bool typing = ImGui::IsAnyItemActive() && (fb->path_editing || ImGui::GetIO().WantTextInput);
    if (!typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.KeyAlt) {
        int n = (int)fb->view.size(), s = fb->selected, ns = s;
        int page = (int)(body_h / ImGui::GetTextLineHeightWithSpacing()) - 2;
        if (page < 1) page = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ns = s < 0 ? 0 : s + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   ns = s < 0 ? 0 : s - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))  ns = s + page;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))    ns = s - page;
        if (ImGui::IsKeyPressed(ImGuiKey_Home))      ns = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End))       ns = n - 1;
        /* Type a letter: jump to the next name starting with it. */
        for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
            ImWchar c = io.InputQueueCharacters[i];
            if (c < 32 || c >= 127 || n == 0) continue;
            for (int k = 1; k <= n; k++) {
                int idx = ((s < 0 ? -1 : s) + k) % n;
                const std::string &nm = fb->entries[fb->view[idx]].name;
                if (!nm.empty() && tolower((unsigned char)nm[0]) == tolower(c)) { ns = idx; break; }
            }
        }
        if (n > 0 && ns != s) {
            ns = ns < 0 ? 0 : (ns >= n ? n - 1 : ns);
            fb->selected = ns;
            fb->scroll_to_sel = true;
            const Entry &e = fb->entries[fb->view[ns]];
            if (!e.dir) snprintf(fb->name_buf, sizeof(fb->name_buf), "%s", e.name.c_str());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && up != fb->cwd) navigate(fb, up);
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
            fb->selected >= 0 && fb->selected < n) {
            const Entry &e = fb->entries[fb->view[fb->selected]];
            open_target = join(fb->cwd, e.name);
            picked_dir = e.dir;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !fb->path_editing) {
        fb->last_dir = fb->cwd; fb->open = false; save_state(fb);
        ImGui::CloseCurrentPopup();
        result = LUMEN_FB_CANCELLED;
    }
    if (!open_target.empty() && result == LUMEN_FB_NONE) {
        if (picked_dir) navigate(fb, open_target);
        else if (activate(fb, open_target, out, out_size)) result = LUMEN_FB_PICKED;
    }

    /* ---- footer: File box, filter, hidden, Open / Cancel --------------- */
    if (fb->open) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File:");
        ImGui::SameLine();
        float btn_w = 80.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn_w - ImGui::GetStyle().ItemSpacing.x);
        bool enter = ImGui::InputText("##name", fb->name_buf, sizeof(fb->name_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemActive()) fb->path_editing = true;
        ImGui::SameLine();
        bool can_open = fb->name_buf[0] != '\0' ||
                        (fb->selected >= 0 && fb->selected < (int)fb->view.size());
        if (!can_open) ImGui::BeginDisabled();
        bool open_click = ImGui::Button("Open", ImVec2(btn_w, 0));
        if (!can_open) ImGui::EndDisabled();
        if (enter || open_click) {
            std::string target = fb->name_buf;
            if (target.empty() && fb->selected >= 0)
                target = fb->entries[fb->view[fb->selected]].name;
            if (!target.empty() && target[0] == '~') target = home_dir() + target.substr(1);
            if (activate(fb, target, out, out_size)) result = LUMEN_FB_PICKED;
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Type:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("##filter", &fb->filter, FILTER_NAMES, 3)) fb->view_dirty = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Show hidden", &fb->show_hidden)) fb->view_dirty = true;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btn_w);
        if (ImGui::Button("Cancel", ImVec2(btn_w, 0))) {
            fb->last_dir = fb->cwd; fb->open = false; save_state(fb);
            ImGui::CloseCurrentPopup();
            result = LUMEN_FB_CANCELLED;
        }
    }

    if (!keep_open && fb->open) {           /* title-bar X */
        fb->last_dir = fb->cwd; fb->open = false; save_state(fb);
        result = LUMEN_FB_CANCELLED;
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(2);
    return result;
}
