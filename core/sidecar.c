#include "sidecar.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
  #include <windows.h>
  #if defined(_MSC_VER)
    #define strcasecmp  _stricmp
    #define strncasecmp _strnicmp
  #endif
#else
  #include <strings.h>
  #include <dirent.h>
  #include <sys/stat.h>
#endif

/* ---- languages -------------------------------------------------------- */

static const struct { const char *name, *c2, *c3a, *c3b; } LANGS[] = {
    {"English","en","eng",NULL}, {"Spanish","es","spa",NULL}, {"French","fr","fre","fra"},
    {"German","de","ger","deu"}, {"Italian","it","ita",NULL}, {"Portuguese","pt","por",NULL},
    {"Japanese","ja","jpn",NULL}, {"Chinese","zh","chi","zho"}, {"Korean","ko","kor",NULL},
    {"Russian","ru","rus",NULL}, {"Dutch","nl","dut","nld"}, {"Swedish","sv","swe",NULL},
    {"Norwegian","no","nor",NULL}, {"Danish","da","dan",NULL}, {"Finnish","fi","fin",NULL},
    {"Polish","pl","pol",NULL}, {"Turkish","tr","tur",NULL}, {"Arabic","ar","ara",NULL},
    {"Hebrew","he","heb",NULL}, {"Hindi","hi","hin",NULL}, {"Greek","el","gre","ell"},
    {"Czech","cs","cze","ces"}, {"Hungarian","hu","hun",NULL}, {"Romanian","ro","rum","ron"},
    {"Thai","th","tha",NULL}, {"Vietnamese","vi","vie",NULL}, {"Indonesian","id","ind",NULL},
    {"Ukrainian","uk","ukr",NULL},
};

const char *lumen_language_name(const char *code) {
    if (!code || !*code) return NULL;
    for (size_t i = 0; i < sizeof(LANGS) / sizeof(LANGS[0]); i++) {
        if (!strcasecmp(code, LANGS[i].name) || !strcasecmp(code, LANGS[i].c3a) ||
            (LANGS[i].c3b && !strcasecmp(code, LANGS[i].c3b)))
            return LANGS[i].name;
        /* 2-letter codes only as exact tokens: "hi" is Hindi as a language
         * code but also "hearing impaired" -- the caller checks flags first. */
        if (!strcasecmp(code, LANGS[i].c2)) return LANGS[i].name;
    }
    return NULL;
}

/* ---- helpers ---------------------------------------------------------- */

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d : "";
}
static int is_sub_ext(const char *e) {
    return !strcasecmp(e, ".srt") || !strcasecmp(e, ".ass") || !strcasecmp(e, ".ssa") || !strcasecmp(e, ".vtt");
}
static int is_video_ext(const char *e) {
    static const char *v[] = {".mp4",".m4v",".mov",".mkv",".webm",".avi",".ts",".m2ts",".ogv",".3gp"};
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) if (!strcasecmp(e, v[i])) return 1;
    return 0;
}
/* "Movie.en.srt" starts with stem "Movie" followed by a separator or the extension */
static int name_matches(const char *name, const char *stem) {
    size_t n = strlen(stem);
    if (n == 0 || strncasecmp(name, stem, n) != 0) return 0;
    char c = name[n];
    return c == '.' || c == '_' || c == '-' || c == ' ';
}

typedef void (*entry_fn)(const char *name, int is_dir, void *ud);

static void list_dir(const char *dir, entry_fn fn, void *ud) {
#if defined(_WIN32)
    char pat[600];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        fn(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ud);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int is_dir = 0;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;                     /* d_type is DT_UNKNOWN on some filesystems */
        if (stat(full, &st) == 0) is_dir = S_ISDIR(st.st_mode);
        fn(e->d_name, is_dir, ud);
    }
    closedir(d);
#endif
}

typedef struct {
    const char *dir, *stem;
    int videos;
    struct cand { char path[768]; char name[256]; int strong; } c[64];
    int n;
    char subdirs[4][512];   /* Subs/ Subtitles/ and Subs/<stem>/ */
    int nsub;
} scan_t;

static void add_cand(scan_t *s, const char *dir, const char *name, int strong) {
    if (s->n >= 64) return;
    struct cand *c = &s->c[s->n++];
    snprintf(c->path, sizeof(c->path), "%s/%s", dir, name);
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->strong = strong;
}

static void top_entry(const char *name, int is_dir, void *ud) {
    scan_t *s = (scan_t *)ud;
    if (is_dir) {
        if ((!strcasecmp(name, "subs") || !strcasecmp(name, "subtitles") || !strcasecmp(name, "sub")) && s->nsub < 4)
            snprintf(s->subdirs[s->nsub++], 512, "%s/%s", s->dir, name);
        return;
    }
    const char *e = ext_of(name);
    if (is_video_ext(e)) s->videos++;
    else if (is_sub_ext(e)) add_cand(s, s->dir, name, name_matches(name, s->stem));
}

typedef struct { scan_t *s; const char *dir; int in_stem_dir; } sub_ud;

static void subdir_entry(const char *name, int is_dir, void *ud) {
    sub_ud *u = (sub_ud *)ud;
    scan_t *s = u->s;
    if (is_dir) {                            /* Subs/<movie name>/ */
        if (!u->in_stem_dir && !strcasecmp(name, s->stem) && s->nsub < 4)
            snprintf(s->subdirs[s->nsub++], 512, "%s/%s", u->dir, name);
        return;
    }
    if (is_sub_ext(ext_of(name)))
        add_cand(s, u->dir, name, u->in_stem_dir || name_matches(name, s->stem));
}

static int cand_cmp(const void *a, const void *b) {
    const struct cand *x = (const struct cand *)a, *y = (const struct cand *)b;
    if (x->strong != y->strong) return y->strong - x->strong;
    return strcasecmp(x->path, y->path);
}

/* ---- labels ----------------------------------------------------------- */

void lumen_sidecar_label(const char *file_name, const char *stem, char *out, size_t n) {
    char base[128];
    snprintf(base, sizeof(base), "%s", file_name);
    char *dot = strrchr(base, '.');
    char fmt[8] = "SUB";
    if (dot) {
        snprintf(fmt, sizeof(fmt), "%s", dot + 1);
        for (char *p = fmt; *p; p++) *p = (char)toupper((unsigned char)*p);
        *dot = '\0';
    }
    /* Only look at what follows the movie name: "Movie.2019.en" must not
     * read "2019" or words from the title as a language. */
    const char *rest = base;
    if (stem && name_matches(file_name, stem)) rest = base + strlen(stem);

    const char *lang = NULL;
    int sdh = 0, forced = 0;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", rest);
    for (char *tok = strtok(tmp, "._- []()"); tok; tok = strtok(NULL, "._- []()")) {
        if (!strcasecmp(tok, "forced")) { forced = 1; continue; }
        if (!strcasecmp(tok, "sdh") || !strcasecmp(tok, "cc") ||
            (!strcasecmp(tok, "hi") && lang)) { sdh = 1; continue; }   /* "en.hi" = hearing impaired */
        const char *l = lumen_language_name(tok);
        if (l && !lang) lang = l;
    }

    if (lang)
        snprintf(out, n, "%s%s%s [%s file]", lang, sdh ? " (SDH)" : "", forced ? " - forced" : "", fmt);
    else
        snprintf(out, n, "%s [%s file]", base, fmt);
}

/* ---- search ----------------------------------------------------------- */

int lumen_find_sidecar_subs(const char *media_path, lumen_sidecar_t *out, int max) {
    static scan_t s;                        /* ~40 KB: keep it off the stack */
    memset(&s, 0, sizeof(s));

    char dir[512], stem[256];
    const char *slash = strrchr(media_path, '/');
#if defined(_WIN32)
    const char *bs = strrchr(media_path, '\\');
    if (bs > slash) slash = bs;
#endif
    if (slash) {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - media_path), media_path);
        if (!dir[0]) strcpy(dir, "/");
    } else {
        strcpy(dir, ".");
    }
    snprintf(stem, sizeof(stem), "%s", slash ? slash + 1 : media_path);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    s.dir = dir;
    s.stem = stem;
    list_dir(dir, top_entry, &s);
    for (int i = 0; i < s.nsub; i++) {      /* nsub may grow while iterating (Subs/<stem>/) */
        char here[512];
        snprintf(here, sizeof(here), "%s", s.subdirs[i]);
        size_t dl = strlen(dir);
        int in_stem_dir = strncmp(here, dir, dl) == 0 && strchr(here + dl + 1, '/') != NULL;
        sub_ud u = { &s, here, in_stem_dir };
        list_dir(here, subdir_entry, &u);
    }

    int sole_video = s.videos <= 1;
    qsort(s.c, (size_t)s.n, sizeof(s.c[0]), cand_cmp);
    int k = 0;
    for (int i = 0; i < s.n && k < max; i++) {
        if (!s.c[i].strong && !sole_video) continue;
        snprintf(out[k].path, sizeof(out[k].path), "%s", s.c[i].path);
        snprintf(out[k].file, sizeof(out[k].file), "%s", s.c[i].name);
        lumen_sidecar_label(s.c[i].name, stem, out[k].label, sizeof(out[k].label));
        k++;
    }
    return k;
}
