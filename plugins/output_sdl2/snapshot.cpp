/* snapshot.cpp -- see snapshot.h */
#include "snapshot.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>      /* SHGetKnownFolderPath */
#include <io.h>          /* _open_osfhandle */
#include <fcntl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO          /* we open the file ourselves (Unicode, no overwrite) */
#if defined(__GNUC__)
#pragma GCC diagnostic push              /* third-party code: its -Wextra noise isn't ours to fix */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../third_party/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

/* ---- UTF-8 <-> path (std::filesystem uses wide strings on Windows) ---- */
static fs::path u8_to_path(const std::string &s) {
#if defined(_WIN32)
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return fs::path(w);
#else
    return fs::path(s);
#endif
}

static std::string path_to_u8(const fs::path &p) {
#if defined(_WIN32)
    const std::wstring &w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
#else
    return p.string();
#endif
}

/* ---- where snapshots and clips go -------------------------------------- */
/* The user's Pictures or Videos folder. Windows: the shell's known folder
 * (follows OneDrive/redirection). Linux: XDG user-dirs ("Bilder",
 * "Vidéos"...), else ~/Pictures / ~/Videos. */
static fs::path user_media_dir(bool videos) {
#if defined(_WIN32)
    /* FOLDERID_Pictures / FOLDERID_Videos, spelled out so we needn't link libuuid */
    static const GUID k_pictures = { 0x33E28130, 0x4E1E, 0x4676, { 0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB } };
    static const GUID k_videos   = { 0x18989B1D, 0x99B5, 0x455B, { 0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC } };
    PWSTR w = NULL;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(videos ? k_videos : k_pictures, KF_FLAG_CREATE, NULL, &w)) && w) p = fs::path(w);
    if (w) CoTaskMemFree(w);
    if (!p.empty()) return p;
    wchar_t prof[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    return (n && n < MAX_PATH) ? fs::path(prof) / (videos ? L"Videos" : L"Pictures") : fs::path(L".");
#else
    const char *h = getenv("HOME");
    std::string home = (h && *h) ? h : "/tmp";
    const char *fallback = videos ? "Videos" : "Pictures";
    const char *cfg = getenv("XDG_CONFIG_HOME");
    std::string file = (cfg && *cfg) ? std::string(cfg) + "/user-dirs.dirs" : home + "/.config/user-dirs.dirs";
    if (FILE *f = fopen(file.c_str(), "r")) {
        char line[1024];
        const std::string want = videos ? "XDG_VIDEOS_DIR=\"" : "XDG_PICTURES_DIR=\"";
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, want.c_str(), want.size())) continue;
            std::string v = line + want.size();
            size_t q = v.find('"');
            if (q != std::string::npos) v.resize(q);
            if (!v.compare(0, 5, "$HOME")) v = home + v.substr(5);
            fclose(f);
            /* "$HOME/" alone means "no such folder": use the default */
            if (!v.empty() && v != home && v != home + "/") return fs::path(v);
            return fs::path(home) / fallback;
        }
        fclose(f);
    }
    return fs::path(home) / fallback;
#endif
}

static fs::path pictures_dir(void) { return user_media_dir(false); }

int lumen_videos_dir(char *out, size_t out_size) {
    fs::path p = user_media_dir(true);
    std::error_code ec;
    fs::create_directories(p, ec);
    if (!fs::is_directory(p, ec)) return -1;
    snprintf(out, out_size, "%s", path_to_u8(p).c_str());
    return 0;
}

void lumen_media_stamp(const char *prefix, char *out, size_t out_size) {
    time_t t = time(NULL);
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char when[40];
    strftime(when, sizeof(when), "%Y-%m-%d_%H-%M-%S", &lt);
    snprintf(out, out_size, "%s-%s", prefix, when);
}

/* Create `p` for writing, failing if it already exists (never overwrite,
 * never follow a symlink planted there). NULL with errno set on failure. */
static FILE *create_new(const fs::path &p) {
#if defined(_WIN32)
    HANDLE hf = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        errno = GetLastError() == ERROR_FILE_EXISTS ? EEXIST : EACCES;
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)hf, _O_WRONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(hf); errno = EIO; return NULL; }
    FILE *f = _fdopen(fd, "wb");
    if (!f) _close(fd);
    return f;
#else
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) close(fd);
    return f;
#endif
}

/* ---- the save itself, on its own thread -------------------------------- */
struct snap_job {
    unsigned char *rgb;
    int w, h;
    fs::path dir;
    std::string stamp;       /* qqvideo-2026-11-04_00-22-15 */
};

static SDL_atomic_t g_running;       /* saves still in progress */
static SDL_mutex *g_lock;            /* guards the three below */
static int  g_done;                  /* a save finished, not yet polled */
static int  g_ok;
static char g_msg[1200];

static void finish(int ok, const std::string &msg) {
    SDL_LockMutex(g_lock);
    g_done = 1;
    g_ok = ok;
    snprintf(g_msg, sizeof(g_msg), "%s", msg.c_str());
    SDL_UnlockMutex(g_lock);
}

/* a short write sets the stream's error flag, checked after encoding */
static void write_cb(void *ctx, void *data, int size) {
    if (size > 0) fwrite(data, 1, (size_t)size, (FILE *)ctx);
}

static int SDLCALL save_thread(void *arg) {
    snap_job *job = (snap_job *)arg;
    std::error_code ec;
    fs::create_directories(job->dir, ec);          /* fine if it exists already */

    FILE *f = NULL;
    fs::path path;
    for (int n = 1; n < 100 && !f; n++) {
        std::string name = job->stamp + (n > 1 ? "-" + std::to_string(n) : "") + ".png";
        path = job->dir / u8_to_path(name);
        f = create_new(path);
        if (!f && errno != EEXIST) break;
    }
    if (!f) {
        finish(0, "couldn't create a file in " + path_to_u8(job->dir) + " (" + strerror(errno) + ")");
    } else {
        int ok = stbi_write_png_to_func(write_cb, f, job->w, job->h, 3, job->rgb, job->w * 3) != 0;
        ok = ok && !ferror(f);
        ok = (fclose(f) == 0) && ok;
        if (ok) {
            finish(1, path_to_u8(path));
        } else {
            fs::remove(path, ec);
            finish(0, "couldn't write " + path_to_u8(path) + " (disk full?)");
        }
    }
    free(job->rgb);
    delete job;
    SDL_AtomicAdd(&g_running, -1);
    return 0;
}

int lumen_snapshot_save(unsigned char *rgb, int w, int h, char *err, size_t err_size) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (!rgb || w <= 0 || h <= 0 || !g_lock) {
        free(rgb);
        snprintf(err, err_size, "nothing to save");
        return -1;
    }

    char stamp[64];
    lumen_media_stamp("qqvideo", stamp, sizeof(stamp));

    snap_job *job = new snap_job;
    job->rgb = rgb;
    job->w = w;
    job->h = h;
    job->dir = pictures_dir();
    job->stamp = stamp;
    SDL_AtomicAdd(&g_running, 1);
    SDL_Thread *th = SDL_CreateThread(save_thread, "qq-snapshot", job);
    if (!th) {
        SDL_AtomicAdd(&g_running, -1);
        snprintf(err, err_size, "couldn't start saving (%s)", SDL_GetError());
        free(job->rgb);
        delete job;
        return -1;
    }
    SDL_DetachThread(th);
    return 0;
}

int lumen_snapshot_poll(int *ok, char *msg, size_t msg_size) {
    if (!g_lock) return 0;
    SDL_LockMutex(g_lock);
    int done = g_done;
    if (done) {
        g_done = 0;
        *ok = g_ok;
        snprintf(msg, msg_size, "%s", g_msg);
    }
    SDL_UnlockMutex(g_lock);
    return done;
}

void lumen_snapshot_wait(int max_ms) {
    for (int waited = 0; SDL_AtomicGet(&g_running) > 0 && waited < max_ms; waited += 10)
        SDL_Delay(10);
}
