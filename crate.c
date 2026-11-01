/* CRATE DIGGA 1.4 (JJ DA PLUG): an MPC OS VST2 plugin (instrument) that digs for free sounds on the internet.
 * Made by JJ-LEMMING.
 *
 * It searches Openverse (https://openverse.org) for audio that is CC0 or Public Domain only: nothing else is ever requested
 * or shown. Tap a result to hear it, KEEP it in your crate (saved as WAV with a small text file naming source and
 * license), or put it on one of 16 pads that play from the plugin (also by the MPC's real pads, learned once).
 *
 * Safety: the plugin only makes outgoing HTTPS requests to the search service and to the file address a result names (http:// or
 * https:// only, 15 MB at most). It listens on no port. Files are written only to its own folder. All network and disk work
 * is on worker threads; render() only mixes audio. HTTPS uses the MPC's own libcurl (loaded when needed) and its
 * certificate store; the certificates are checked. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"
int crate_ogg_decode(const unsigned char *mem, int len, int *channels, int *rate, short **out);

typedef struct {
    void *(*create)(const char *data_dir);
    void (*destroy)(void *inst);
    void (*midi)(void *inst, const uint8_t *msg, int len);
    void (*set_param)(void *inst, const char *key, const char *val);
    int (*get_param)(void *inst, const char *key, char *buf, int buf_len);
    void (*render)(void *inst, int16_t *out_lr, int frames);
    void (*process)(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames);
    void (*song_pos)(void *inst, double ppq, double bpm, int playing, int frames);
} mpc_engine_t;

#define ROWS 7
#define NPADS 16
#define NRES 21
#define PAD_MAX_FRAMES (44100 * 180)
#define MAX_DL (15 * 1024 * 1024)
extern char **environ;

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
static void disp_text(char *dst, size_t cap, const char *src, int max_chars) {
    size_t o = 0;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 3 < cap; chars++) {
        if (chars >= max_chars) { if (o + 4 < cap) { dst[o++] = '.'; dst[o++] = '.'; } break; }
        if (*p < 0x80) { dst[o++] = (*p < 32 || *p == 127) ? ' ' : (char)*p; p++; }
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80 && (((p[0] & 0x1F) << 6) | (p[1] & 0x3F)) <= 0x17F) { dst[o++] = (char)p[0]; dst[o++] = (char)p[1]; p += 2; }
        else { dst[o++] = '?'; p++; while ((*p & 0xC0) == 0x80) p++; }
    }
    dst[o] = 0;
}


/* ---------- state ---------- */

typedef struct { char title[80], creator[48], license[8], url[400], ext[8]; int dur; } res_t;
typedef struct { char name[100]; long long size; time_t mt; } cfile_t;

static struct {
    pthread_mutex_t mu, padmu, netmu;
    int refs, boot;
    char base[200];
    char q[32]; int cat, shortonly;
    res_t res[NRES]; int nres, rpage, rsel, apage, hidden, src;
    char status[120]; double status_t;
    atomic_int busy;
    int16_t *pv; int pv_frames; int pv_play; unsigned pv_gen; int pv_valid, pv_kept;
    char pv_name[80], pv_lic[8], pv_creator[48], pv_url[400];
    cfile_t cr[200]; int ncr, cpage, csel;
    struct { int16_t *d; int frames; char name[40]; } pad[NPADS];
    int pad_sel; signed char padmap[128]; int learn;
    char note[100]; double note_t;
    int curl_state; char netinfo[160]; unsigned rev;
    struct { unsigned char att, dec, rel, grp, mode; } pc[NPADS];   /* per pad: attack, decay, release (steps), mute group 0-8, mode 0 one-shot / 1 hold */
    int last_note, last_pad, acc_state;   /* acc_state: 0 no account, 1 waiting for the e-mail link, 2 ready */
    atomic_int verifying;
    char email[64], cid[80], csec[80], tok[700]; double tok_exp;     /* the Openverse account */
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .padmu = PTHREAD_MUTEX_INITIALIZER, .netmu = PTHREAD_MUTEX_INITIALIZER, .rsel = -1, .csel = -1, .apage = 1, .shortonly = 1, .last_note = -1, .last_pad = -1 };

/* the turntable: pitch fader, range, key lock, scratch slider, platter picture (frame number of the filmstrip) */
#define NPLAT 48
typedef struct { atomic_int pitch, prange, klock, jogv, jogref, jogstart, joglast, plat; atomic_llong jogt; } deck_t;
static const int RANGE_PCT[3] = { 8, 16, 50 };
static double deck_speed(deck_t *dk) { return 1.0 + (atomic_load(&dk->pitch) - 500) / 500.0 * RANGE_PCT[atomic_load(&dk->prange) % 3] / 100.0; }
static void deck_init(deck_t *dk) { atomic_store(&dk->pitch, 500); atomic_store(&dk->prange, 1); atomic_store(&dk->jogv, 500); atomic_store(&dk->jogref, 500); atomic_store(&dk->joglast, 500); }
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

#define LOCK() pthread_mutex_lock(&G.mu)
#define UNLOCK() pthread_mutex_unlock(&G.mu)

static void note_set(const char *t) { LOCK(); snprintf(G.note, sizeof G.note, "%s", t); G.note_t = mono(); G.rev++; UNLOCK(); }
static void status_set(const char *t) { LOCK(); snprintf(G.status, sizeof G.status, "%s", t); G.status_t = mono(); G.rev++; UNLOCK(); }

static void make_dirs(void) {
    const char *env = getenv("CRATE_DIR");
    struct stat sb;
    if (env && *env) snprintf(G.base, sizeof G.base, "%s", env);
    else if (!stat("/sdcard/MPC Documents", &sb)) snprintf(G.base, sizeof G.base, "/sdcard/MPC Documents/Crate Digga");
    else if (!stat("/sdcard/Force Documents", &sb)) snprintf(G.base, sizeof G.base, "/sdcard/Force Documents/Crate Digga");
    else snprintf(G.base, sizeof G.base, "/sdcard/Crate Digga");
    char p[300];
    mkdir(G.base, 0755);
    snprintf(p, sizeof p, "%s/crate", G.base); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/pads", G.base); mkdir(p, 0755);
}

/* ---------- HTTPS: the MPC's own libcurl, loaded when first needed; without it the system's curl or wget program ---------- */

typedef struct { uint8_t *d; size_t n, cap, max; int over; } buf_t;

static struct {
    void *h;
    void *(*slist_add)(void *, const char *);
    void (*slist_free)(void *);
    void *(*init)(void);
    int (*setopt)(void *, int, ...);
    int (*perform)(void *);
    void (*cleanup)(void *);
    int (*getinfo)(void *, int, ...);
    const char *(*strerr)(int);
} CURL;

static int curl_load(void) {
    pthread_mutex_lock(&G.netmu);
    if (!G.curl_state) {
        G.curl_state = -1;
        if (!getenv("CRATE_NOLIBCURL")) {
            static const char *const names[] = { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", NULL };
            for (int i = 0; names[i] && !CURL.h; i++) CURL.h = dlopen(names[i], RTLD_NOW);
            if (CURL.h) {
                *(void **)&CURL.slist_add = dlsym(CURL.h, "curl_slist_append");
                *(void **)&CURL.slist_free = dlsym(CURL.h, "curl_slist_free_all");
                *(void **)&CURL.init = dlsym(CURL.h, "curl_easy_init");
                *(void **)&CURL.setopt = dlsym(CURL.h, "curl_easy_setopt");
                *(void **)&CURL.perform = dlsym(CURL.h, "curl_easy_perform");
                *(void **)&CURL.cleanup = dlsym(CURL.h, "curl_easy_cleanup");
                *(void **)&CURL.getinfo = dlsym(CURL.h, "curl_easy_getinfo");
                *(void **)&CURL.strerr = dlsym(CURL.h, "curl_easy_strerror");
                if (CURL.slist_add && CURL.slist_free && CURL.init && CURL.setopt && CURL.perform && CURL.cleanup && CURL.getinfo && CURL.strerr) G.curl_state = 1;
            }
        }
    }
    int ok = G.curl_state > 0;
    pthread_mutex_unlock(&G.netmu);
    return ok;
}

static size_t wcb(void *p, size_t sz, size_t nm, void *u) {
    buf_t *b = u;
    size_t n = sz * nm;
    if (b->n + n > b->max) { b->over = 1; return 0; }
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        while (nc < b->n + n + 1) nc *= 2;
        uint8_t *t = realloc(b->d, nc);
        if (!t) return 0;
        b->d = t; b->cap = nc;
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
    b->d[b->n] = 0;
    return n;
}

typedef struct { const char *post, *bearer; } req_t;

static int http_curl(const req_t *rq, const char *url, buf_t *b, long timeout, char *err, size_t ecap, long *code) {
    void *h = CURL.init();
    if (!h) { snprintf(err, ecap, "curl init failed"); return -1; }
    const char *ca = getenv("CRATE_CAINFO");
    CURL.setopt(h, 10002, url);                       /* URL */
    CURL.setopt(h, 20011, wcb);                       /* WRITEFUNCTION */
    CURL.setopt(h, 10001, b);                         /* WRITEDATA */
    CURL.setopt(h, 52, 1L);                           /* FOLLOWLOCATION */
    CURL.setopt(h, 68, 5L);                           /* MAXREDIRS */
    CURL.setopt(h, 13, timeout);                      /* TIMEOUT */
    CURL.setopt(h, 78, 7L);                          /* CONNECTTIMEOUT */
    CURL.setopt(h, 10018, "CrateDigga/0.1 (JJ DA PLUG; MPC)");   /* USERAGENT */
    CURL.setopt(h, 99, 1L);                           /* NOSIGNAL */
    CURL.setopt(h, 10102, "");                        /* ACCEPT_ENCODING: whatever curl supports */
    if (ca && *ca) CURL.setopt(h, 10065, ca);         /* CAINFO (tests only) */
    void *hdr = NULL;
    if (rq && rq->bearer && *rq->bearer) {
        char ah[760];
        snprintf(ah, sizeof ah, "Authorization: Bearer %s", rq->bearer);
        hdr = CURL.slist_add(hdr, ah);
        CURL.setopt(h, 10023, hdr);                   /* HTTPHEADER */
    }
    if (rq && rq->post) CURL.setopt(h, 10015, rq->post);   /* POSTFIELDS (form encoded) */
    int rc = CURL.perform(h);
    long c = 0;
    CURL.getinfo(h, 0x200002, &c);                    /* RESPONSE_CODE */
    CURL.cleanup(h);
    if (hdr) CURL.slist_free(hdr);
    *code = c;
    if (rc != 0) { snprintf(err, ecap, "%s", b->over ? "File too big" : CURL.strerr(rc)); return -1; }
    return 0;
}

static int http_spawn(const req_t *rq, const char *url, buf_t *b, long timeout, char *err, size_t ecap, long *code) {
    char tmp[80], tm[16], mx[24];
    snprintf(tmp, sizeof tmp, "/tmp/crate_%d_%ld.dl", (int)getpid(), (long)time(NULL));
    snprintf(tm, sizeof tm, "%ld", timeout);
    snprintf(mx, sizeof mx, "%zu", b->max);
    static const char *const curls[] = { "/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", NULL };
    static const char *const wgets[] = { "/usr/bin/wget", "/bin/wget", "/usr/local/bin/wget", NULL };
    const char *bin = NULL;
    int is_curl = 0;
    for (int i = 0; curls[i] && !bin; i++) if (!access(curls[i], X_OK)) { bin = curls[i]; is_curl = 1; }
    for (int i = 0; wgets[i] && !bin; i++) if (!access(wgets[i], X_OK)) bin = wgets[i];
    if (!bin) { snprintf(err, ecap, "No curl library or program on this MPC"); return -1; }
    char ah[760] = "";
    if (rq && rq->bearer && *rq->bearer) snprintf(ah, sizeof ah, "Authorization: Bearer %s", rq->bearer);
    char *argv_c[16];
    int ac = 0;
    argv_c[ac++] = (char *)"curl"; argv_c[ac++] = (char *)"-sSL"; argv_c[ac++] = (char *)"--max-time"; argv_c[ac++] = tm;
    argv_c[ac++] = (char *)"--max-filesize"; argv_c[ac++] = mx; argv_c[ac++] = (char *)"-A"; argv_c[ac++] = (char *)"CrateDigga/0.1";
    if (ah[0]) { argv_c[ac++] = (char *)"-H"; argv_c[ac++] = ah; }
    if (rq && rq->post) { argv_c[ac++] = (char *)"-d"; argv_c[ac++] = (char *)rq->post; }
    argv_c[ac++] = (char *)"-w"; argv_c[ac++] = (char *)"%{http_code}"; argv_c[ac++] = (char *)"-o"; argv_c[ac++] = tmp; argv_c[ac++] = (char *)url; argv_c[ac] = NULL;
    char *argv_w[] = { (char *)"wget", (char *)"-q", (char *)"-T", tm, (char *)"-O", tmp, (char *)url, NULL };
    char *envp[] = { (char *)"PATH=/usr/bin:/bin:/usr/local/bin", NULL };   /* a clean environment: no LD_PRELOAD from the MPC */
    pid_t pid;
    char outf[90];
    snprintf(outf, sizeof outf, "%s.code", tmp);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, outf, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* curl prints the HTTP status to stdout */
    if (posix_spawn(&pid, bin, &fa, NULL, is_curl ? argv_c : argv_w, envp)) { posix_spawn_file_actions_destroy(&fa); snprintf(err, ecap, "Cannot start %s", bin); return -1; }
    posix_spawn_file_actions_destroy(&fa);
    int st = 0;
    waitpid(pid, &st, 0);
    long hc = 0;
    { FILE *cf = fopen(outf, "r"); if (cf) { if (fscanf(cf, "%ld", &hc) != 1) hc = 0; fclose(cf); } unlink(outf); }
    FILE *f = fopen(tmp, "rb");
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && f;
    if (is_curl && hc) { *code = hc; if (hc >= 400) ok = ok; }
    if (ok) {
        uint8_t chunk[16384];
        size_t r;
        while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) if (wcb(chunk, 1, r, b) != r) { ok = 0; break; }
    }
    if (f) fclose(f);
    unlink(tmp);
    if (!is_curl || !hc) *code = ok ? 200 : 0;
    if (!ok) snprintf(err, ecap, b->over ? "File too big" : "Download failed");
    return ok ? 0 : -1;
}

/* GET (or POST a form) into a new buffer; only http:// and https://. On an error status the answer body stays in b for the caller to read. */
static int http_req(const req_t *rq, const char *url, buf_t *b, size_t max, long timeout, char *err, size_t ecap, long *code) {
    memset(b, 0, sizeof *b);
    b->max = max;
    *code = 0;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) { snprintf(err, ecap, "Blocked address"); return -1; }
    int rc = curl_load() ? http_curl(rq, url, b, timeout, err, ecap, code) : http_spawn(rq, url, b, timeout, err, ecap, code);
    if (rc == 0 && *code >= 400) { snprintf(err, ecap, "Server answered %ld", *code); rc = -1; }
    if (rc && !b->d) { b->n = 0; }
    return rc;
}
static int http_get(const char *url, buf_t *b, size_t max, long timeout, char *err, size_t ecap, long *code) {
    int rc = http_req(NULL, url, b, max, timeout, err, ecap, code);
    if (rc) { free(b->d); b->d = NULL; b->n = 0; }
    return rc;
}

/* ---------- a small JSON reader for the search answer ---------- */

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
static const char *skip_val(const char *p) {   /* p at the start of a value; returns the byte after it, NULL on error */
    p = skip_ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        p++;
        for (;;) {
            p = skip_ws(p);
            if (*p == close) return p + 1;
            if (!*p) return NULL;
            if (*p == ',') { p++; continue; }
            if (open == '{') {   /* key: value */
                p = skip_val(p);
                if (!p) return NULL;
                p = skip_ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = skip_val(p);
            if (!p) return NULL;
        }
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r') p++;
    return p;
}
/* the value of key in the object that starts at obj (a '{') */
static const char *obj_get(const char *obj, const char *key) {
    const char *p = skip_ws(obj);
    if (*p != '{') return NULL;
    size_t kl = strlen(key);
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}' || !*p) return NULL;
        if (*p == ',') { p++; continue; }
        if (*p != '"') return NULL;
        const char *ks = p + 1, *ke = skip_val(p);
        if (!ke) return NULL;
        p = skip_ws(ke);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        if ((size_t)(ke - 1 - ks) == kl && !strncmp(ks, key, kl)) return p;
        p = skip_val(p);
        if (!p) return NULL;
    }
}
static void val_str(const char *v, char *out, size_t cap) {
    out[0] = 0;
    if (!v || *v != '"') return;
    size_t o = 0;
    for (v++; *v && *v != '"' && o + 4 < cap; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            if (*v == 'n' || *v == 't' || *v == 'r') out[o++] = ' ';
            else if (*v == 'u') { out[o++] = '?'; for (int k = 0; k < 4 && v[1]; k++) v++; }
            else out[o++] = *v;
        } else out[o++] = *v;
    }
    out[o] = 0;
}
static double val_num(const char *v) { return v && (*v == '-' || isdigit((unsigned char)*v)) ? atof(v) : 0; }

/* ---------- decoders (WAV, MP3, OGG) ---------- */

/* ---------- sample pads: 16 pads in the plugin play WAV files from the inbox or from anywhere on the MPC ---------- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* WAV (PCM 8/16/24/32 bit or float) to 44.1 kHz stereo 16 bit; returns 0 and a malloc'd buffer, or a negative error */
static int wav_decode(const uint8_t *f, size_t n, int16_t **out, int *frames, const char **why) {
    *out = NULL;
    *why = "Not a WAV file";
    if (n < 44 || memcmp(f, "RIFF", 4) || memcmp(f + 8, "WAVE", 4)) return -1;
    size_t p = 12;
    int fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    while (p + 8 <= n) {
        uint32_t cl = rd32(f + p + 4);
        const uint8_t *c = f + p + 8;
        size_t avail = n - (p + 8);
        if (!memcmp(f + p, "fmt ", 4) && cl >= 16 && avail >= 16) {
            fmt = rd16(c); ch = rd16(c + 2); rate = rd32(c + 4); bits = rd16(c + 14);
            if (fmt == 0xFFFE && cl >= 26 && avail >= 26) fmt = rd16(c + 24);
        } else if (!memcmp(f + p, "data", 4)) {
            data = c;
            dlen = cl > avail ? avail : cl;
            break;
        }
        p += 8 + (size_t)cl + (cl & 1);
    }
    if (!data || !ch || ch > 8 || rate < 2000 || rate > 400000) return -1;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) { *why = "WAV format not supported"; return -1; }
    size_t bps = (size_t)bits / 8, fr = dlen / (bps * (size_t)ch);
    if (!fr) { *why = "Empty WAV"; return -1; }
    double ratio = 44100.0 / rate;
    size_t of = (size_t)((double)fr * ratio);
    if (of > PAD_MAX_FRAMES) { *why = "Longer than 3 minutes"; return -1; }
    if (of < 1) of = 1;
    int16_t *o = malloc(of * 2 * sizeof(int16_t));
    if (!o) { *why = "Out of memory"; return -1; }
    for (size_t i = 0; i < of; i++) {
        double src = (double)i / ratio;
        size_t i0 = (size_t)src, i1 = i0 + 1 < fr ? i0 + 1 : i0;
        double fx = src - (double)i0;
        for (int k = 0; k < 2; k++) {
            int c = k < ch ? k : 0;
            double v[2];
            for (int j = 0; j < 2; j++) {
                const uint8_t *s = data + ((j ? i1 : i0) * (size_t)ch + (size_t)c) * bps;
                double x;
                if (fmt == 3) { float fv; uint32_t u = rd32(s); memcpy(&fv, &u, 4); x = fv; }
                else if (bits == 8) x = ((int)s[0] - 128) / 128.0;
                else if (bits == 16) x = (int16_t)rd16(s) / 32768.0;
                else if (bits == 24) x = ((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24)) / 2147483648.0;
                else x = (int32_t)rd32(s) / 2147483648.0;
                v[j] = x;
            }
            double y = v[0] + (v[1] - v[0]) * fx;
            if (y > 1) y = 1;
            if (y < -1) y = -1;
            o[2 * i + (size_t)k] = (int16_t)(y * 32767.0);
        }
    }
    *out = o;
    *frames = (int)of;
    return 0;
}

static int wav_write(const char *path, const int16_t *d, int frames, int excl) {
    int fd = open(path, O_WRONLY | O_CREAT | (excl ? O_EXCL : O_TRUNC) | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    uint32_t dl = (uint32_t)frames * 4u, v;
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data";
    v = dl + 36; memcpy(h + 4, &v, 4);
    memcpy(h + 40, &dl, 4);
    int ok = write(fd, h, 44) == 44;
    const uint8_t *p = (const uint8_t *)d;
    size_t left = dl;
    while (ok && left) { ssize_t r = write(fd, p, left > 65536 ? 65536 : left); if (r <= 0) ok = 0; else { p += r; left -= (size_t)r; } }
    if (close(fd)) ok = 0;
    if (!ok) unlink(path);
    return ok ? 0 : -1;
}

/* MP3 to 44.1 kHz stereo 16 bit */
static int mp3_decode(const uint8_t *f, size_t n, int16_t **out, int *frames, const char **why) {
    *out = NULL;
    mp3dec_t *dec = malloc(sizeof *dec);
    short *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(short));
    int16_t *tmp = NULL;
    size_t nf = 0, cap = 0, pos = 0;
    int hz = 0, bad = 0;
    *why = "Not an MP3 file";
    if (!dec || !pcm) { free(dec); free(pcm); *why = "Out of memory"; return -1; }
    mp3dec_init(dec);
    while (pos < n) {
        mp3dec_frame_info_t fi;
        int sm = mp3dec_decode_frame(dec, f + pos, (int)(n - pos), pcm, &fi);
        if (!fi.frame_bytes) break;
        pos += (size_t)fi.frame_bytes;
        if (sm <= 0 || fi.channels < 1 || fi.hz < 8000) continue;
        if (!hz) hz = fi.hz;
        if (fi.hz != hz) continue;
        if (nf + (size_t)sm > (size_t)hz * 180) { *why = "Longer than 3 minutes"; bad = 1; break; }
        if (nf + (size_t)sm > cap) {
            cap = cap ? cap * 2 : 1u << 17;
            while (cap < nf + (size_t)sm) cap *= 2;
            int16_t *t2 = realloc(tmp, cap * 2 * sizeof(int16_t));
            if (!t2) { *why = "Out of memory"; bad = 1; break; }
            tmp = t2;
        }
        for (int i = 0; i < sm; i++) {
            tmp[2 * (nf + (size_t)i)] = pcm[i * fi.channels];
            tmp[2 * (nf + (size_t)i) + 1] = pcm[i * fi.channels + (fi.channels > 1 ? 1 : 0)];
        }
        nf += (size_t)sm;
    }
    free(dec); free(pcm);
    if (bad || !nf) { free(tmp); return -1; }
    double ratio = 44100.0 / hz;
    size_t of = (size_t)((double)nf * ratio);
    if (of < 1) of = 1;
    int16_t *o = malloc(of * 2 * sizeof(int16_t));
    if (!o) { free(tmp); *why = "Out of memory"; return -1; }
    for (size_t i = 0; i < of; i++) {
        double src = (double)i / ratio;
        size_t i0 = (size_t)src, i1 = i0 + 1 < nf ? i0 + 1 : i0;
        double fx = src - (double)i0;
        for (int k = 0; k < 2; k++) o[2 * i + (size_t)k] = (int16_t)(tmp[2 * i0 + (size_t)k] + (tmp[2 * i1 + (size_t)k] - tmp[2 * i0 + (size_t)k]) * fx);
    }
    free(tmp);
    *out = o;
    *frames = (int)of;
    return 0;
}



static int ogg_decode(const uint8_t *f, size_t n, int16_t **out, int *frames, const char **why) {
    int ch = 0, hz = 0;
    short *pcm = NULL;
    *out = NULL;
    *why = "Not a readable OGG file";
    int nf = crate_ogg_decode(f, (int)n, &ch, &hz, &pcm);
    if (nf <= 0 || ch < 1 || hz < 8000) { free(pcm); return -1; }
    if ((long long)nf > (long long)hz * 180) { free(pcm); *why = "Longer than 3 minutes"; return -1; }
    double ratio = 44100.0 / hz;
    size_t of = (size_t)((double)nf * ratio);
    if (of < 1) of = 1;
    int16_t *o = malloc(of * 2 * sizeof(int16_t));
    if (!o) { free(pcm); *why = "Out of memory"; return -1; }
    for (size_t i = 0; i < of; i++) {
        double src = (double)i / ratio;
        size_t i0 = (size_t)src, i1 = i0 + 1 < (size_t)nf ? i0 + 1 : i0;
        double fx = src - (double)i0;
        for (int k = 0; k < 2; k++) {
            int c = k < ch ? k : 0;
            double a = pcm[i0 * (size_t)ch + (size_t)c], b2 = pcm[i1 * (size_t)ch + (size_t)c];
            o[2 * i + (size_t)k] = (int16_t)(a + (b2 - a) * fx);
        }
    }
    free(pcm);
    *out = o;
    *frames = (int)of;
    return 0;
}

static int decode_any(const uint8_t *f, size_t n, int16_t **out, int *frames, const char **why) {
    if (n > 12 && !memcmp(f, "RIFF", 4)) return wav_decode(f, n, out, frames, why);
    if (n > 4 && !memcmp(f, "OggS", 4)) return ogg_decode(f, n, out, frames, why);
    return mp3_decode(f, n, out, frames, why);
}

/* ---------- preview: one decoded sound plays on the plugin's output ---------- */

static void pv_install(int16_t *d, int frames, const char *name, const char *lic, const char *creator, const char *url, int kept) {
    pthread_mutex_lock(&G.padmu);
    int16_t *old = G.pv;
    G.pv = d; G.pv_frames = frames; G.pv_play = 1; G.pv_gen++;
    pthread_mutex_unlock(&G.padmu);
    free(old);
    LOCK();
    snprintf(G.pv_name, sizeof G.pv_name, "%s", name);
    snprintf(G.pv_lic, sizeof G.pv_lic, "%s", lic);
    snprintf(G.pv_creator, sizeof G.pv_creator, "%s", creator);
    snprintf(G.pv_url, sizeof G.pv_url, "%s", url);
    G.pv_valid = 1; G.pv_kept = kept; G.rev++;
    UNLOCK();
}

/* ---------- jobs on worker threads ---------- */

static void spawn_job(void *(*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    if (pthread_create(&t, &at, fn, arg)) { free(arg); atomic_store(&G.busy, 0); status_set("Cannot start a worker"); }
    pthread_attr_destroy(&at);
}

static void urlenc(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_' || *in == '.') out[o++] = *in;
        else if (*in == ' ') out[o++] = '+';
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", (unsigned char)*in);
    }
    out[o] = 0;
}

static int lic_ok(const char *l) { return !strcasecmp(l, "cc0") || !strcasecmp(l, "pdm"); }
static int ext_ok(const char *e) { return !e[0] || !strcasecmp(e, "mp3") || !strcasecmp(e, "ogg") || !strcasecmp(e, "oga") || !strcasecmp(e, "wav"); }

static const char *api_root(void) { const char *e = getenv("CRATE_ROOT"); return e && *e ? e : "https://api.openverse.org/v1/"; }

static void urlenc(const char *in, char *out, size_t cap);

/* the free Openverse account: client id and secret live in openverse.txt (two lines) next to the crate */
static void account_load(void) {
    char p[300];
    LOCK(); snprintf(p, sizeof p, "%s/openverse.txt", G.base); UNLOCK();
    FILE *f = fopen(p, "r");
    if (!f) return;
    char id[80] = "", sec[80] = "";
    if (fgets(id, sizeof id, f)) id[strcspn(id, "\r\n")] = 0;
    if (fgets(sec, sizeof sec, f)) sec[strcspn(sec, "\r\n")] = 0;
    fclose(f);
    LOCK(); snprintf(G.cid, sizeof G.cid, "%s", id); snprintf(G.csec, sizeof G.csec, "%s", sec); UNLOCK();
}

/* a bearer token for the account, or "" (anonymous). Fetched when missing or about to expire. */
static void bearer_get(char *out, size_t cap) {
    out[0] = 0;
    char id[80], sec[80];
    LOCK();
    snprintf(id, sizeof id, "%s", G.cid); snprintf(sec, sizeof sec, "%s", G.csec);
    if (G.tok[0] && mono() < G.tok_exp - 60) { snprintf(out, cap, "%s", G.tok); UNLOCK(); return; }
    UNLOCK();
    if (!id[0] || !sec[0]) return;
    char url[300], post[300], ide[200], sece[200], err[100];
    urlenc(id, ide, sizeof ide); urlenc(sec, sece, sizeof sece);
    snprintf(url, sizeof url, "%sauth_tokens/token/", api_root());
    snprintf(post, sizeof post, "grant_type=client_credentials&client_id=%s&client_secret=%s", ide, sece);
    req_t rq = { post, NULL };
    buf_t b;
    long code;
    if (http_req(&rq, url, &b, 1 << 16, 20, err, sizeof err, &code) == 0 && b.d) {
        char tk[700];
        val_str(obj_get((const char *)b.d, "access_token"), tk, sizeof tk);
        double ex = val_num(obj_get((const char *)b.d, "expires_in"));
        if (tk[0]) {
            LOCK(); snprintf(G.tok, sizeof G.tok, "%s", tk); G.tok_exp = mono() + (ex > 0 ? ex : 3000); UNLOCK();
            snprintf(out, cap, "%s", tk);
        }
    }
    free(b.d);
}


/* Wikimedia Commons: free audio, no account and no key at all. Used when Openverse refuses or finds nothing. */
static const char *wiki_root(void) { const char *e = getenv("CRATE_WIKI"); return e && *e ? e : "https://commons.wikimedia.org/w/api.php"; }
static void strip_tags(char *s) {
    char *o = s; int in = 0;
    for (char *p = s; *p; p++) { if (*p == '<') in = 1; else if (*p == '>') in = 0; else if (!in) *o++ = *p; }
    *o = 0;
    char *a = s; while (*a == ' ') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
}
static int wiki_lic(const char *l, char *out, size_t cap) {   /* only CC0 and public domain pass */
    char t[64]; size_t i = 0;
    for (; l[i] && i < sizeof t - 1; i++) t[i] = (char)tolower((unsigned char)l[i]);
    t[i] = 0;
    if (!strncmp(t, "cc0", 3) || !strncmp(t, "cc-zero", 7)) { snprintf(out, cap, "CC0"); return 1; }
    if (strstr(t, "public domain") || !strcmp(t, "pd") || !strncmp(t, "pd-", 3) || !strncmp(t, "pd ", 3)) { snprintf(out, cap, "PDM"); return 1; }
    return 0;
}
/* returns the number of sounds put into tmp, or -1 (err filled) */
static int wiki_search(const char *qe, int apage, int shortonly, res_t *tmp, int *hidden, char *err, size_t ecap) {
    char url[900];
    snprintf(url, sizeof url, "%s?action=query&format=json&formatversion=2&generator=search&gsrnamespace=6&gsrsearch=%s%%20filetype%%3Aaudio&gsrlimit=50&gsroffset=%d"
             "&prop=imageinfo&iiprop=url%%7Cmime%%7Csize%%7Cextmetadata&iiextmetadatafilter=LicenseShortName%%7CArtist",
             wiki_root(), qe, (apage - 1) * 50);
    req_t rq = { NULL, NULL };
    buf_t b = {0};
    long code = 0;
    if (http_req(&rq, url, &b, 4 << 20, 25, err, ecap, &code)) { free(b.d); return -1; }
    int n = 0;
    const char *q = b.d ? obj_get((const char *)b.d, "query") : NULL;
    const char *arr = q ? obj_get(q, "pages") : NULL;
    if (arr && *(arr = skip_ws(arr)) == '[') {
        const char *p = arr + 1;
        while (n < NRES) {
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p != '{') break;
            const char *end = skip_val(p);
            if (!end) break;
            const char *ii = obj_get(p, "imageinfo");
            if (ii && *(ii = skip_ws(ii)) == '[') ii = skip_ws(ii + 1);
            if (ii && *ii == '{') {
                char u[400], lic[64], lc[8], ti[80], ar[200], ext[8] = "", mime[40];
                val_str(obj_get(ii, "url"), u, sizeof u);
                val_str(obj_get(ii, "mime"), mime, sizeof mime);
                const char *em = obj_get(ii, "extmetadata");
                const char *lv = em ? obj_get(em, "LicenseShortName") : NULL;
                const char *av = em ? obj_get(em, "Artist") : NULL;
                val_str(lv ? obj_get(lv, "value") : NULL, lic, sizeof lic);
                val_str(av ? obj_get(av, "value") : NULL, ar, sizeof ar);
                strip_tags(ar);
                const char *dot = strrchr(u, '.');
                if (dot && strlen(dot) < 7) snprintf(ext, sizeof ext, "%s", dot + 1);
                for (char *c = ext; *c; c++) *c = (char)tolower((unsigned char)*c);
                double dur = val_num(obj_get(ii, "duration"));
                if (wiki_lic(lic, lc, sizeof lc) && (!strcmp(ext, "ogg") || !strcmp(ext, "oga") || !strcmp(ext, "wav") || !strcmp(ext, "mp3")) && !strncmp(u, "https://", 8)) {
                    if (shortonly && dur > 60) (*hidden)++;
                    else {
                        res_t *r = &tmp[n];
                        val_str(obj_get(p, "title"), ti, sizeof ti);
                        char *t = ti; if (!strncmp(t, "File:", 5)) t += 5;
                        snprintf(r->title, sizeof r->title, "%s", t);
                        char *d2 = strrchr(r->title, '.'); if (d2 && strlen(d2) < 7) *d2 = 0;
                        for (char *c = r->title; *c; c++) if (*c == '_') *c = ' ';
                        snprintf(r->creator, sizeof r->creator, "%s", ar[0] ? ar : "unknown");
                        snprintf(r->license, sizeof r->license, "%s", lc);
                        snprintf(r->url, sizeof r->url, "%s", u);
                        snprintf(r->ext, sizeof r->ext, "%s", ext);
                        r->dur = (int)(dur + 0.5);
                        n++;
                    }
                } else if (lic[0]) (*hidden)++;
            }
            p = end;
        }
    }
    free(b.d);
    return n;
}

typedef struct { int apage; } sjob_t;

static void *job_search(void *arg) {
    int apage = ((sjob_t *)arg)->apage;
    free(arg);
    char q[40], qe[120], url[600], err[100] = "";
    int cat, shortonly;
    LOCK(); snprintf(q, sizeof q, "%s", G.q); cat = G.cat; shortonly = G.shortonly; UNLOCK();
    urlenc(q, qe, sizeof qe);
    snprintf(url, sizeof url, "%saudio/?q=%s&license=cc0,pdm&page_size=%d&page=%d&mature=false%s", api_root(),
             qe, NRES, apage, cat == 1 ? "&category=sound_effect" : cat == 2 ? "&category=music" : "");
    status_set("Searching...");
    char bearer[720];
    bearer_get(bearer, sizeof bearer);
    req_t rq = { NULL, bearer };
    buf_t b = {0};
    long code = 0;
    int use_wiki = 0;
    LOCK(); if (apage > 1 && G.src == 1) use_wiki = 1; UNLOCK();
    res_t *tmp = calloc(NRES, sizeof *tmp);
    int n = 0, hidden = 0;
    if (!tmp) { status_set("Out of memory"); atomic_store(&G.busy, 0); return NULL; }
    long ovcode = 0;
    int ovbad = !use_wiki && http_req(&rq, url, &b, 4 << 20, 25, err, sizeof err, &code);
    if (ovbad && code == 401 && bearer[0]) {   /* a key that is not valid (yet): forget the token and ask without it */
        free(b.d); b.d = NULL;
        LOCK(); G.tok[0] = 0; UNLOCK();
        req_t rq2 = { NULL, NULL };
        ovbad = http_req(&rq2, url, &b, 4 << 20, 25, err, sizeof err, &code);
    }
    ovcode = code;
    if (ovbad) {
        free(b.d); b.d = NULL;
        if (apage == 1) use_wiki = 1;
        else { char m[160]; snprintf(m, sizeof m, "Search failed: %.100s", err); status_set(m); free(tmp); atomic_store(&G.busy, 0); return NULL; }
    }
    const char *arr = (!use_wiki && b.d) ? obj_get((const char *)b.d, "results") : NULL;
    if (arr && *(arr = skip_ws(arr)) == '[') {
        const char *p = arr + 1;
        while (n < NRES) {
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p != '{') break;
            const char *end = skip_val(p);
            if (!end) break;
            res_t *r = &tmp[n];
            char url2[400], lic[16], ext[16];
            val_str(obj_get(p, "url"), url2, sizeof url2);
            val_str(obj_get(p, "license"), lic, sizeof lic);
            val_str(obj_get(p, "filetype"), ext, sizeof ext);
            double ms = val_num(obj_get(p, "duration"));
            if (lic_ok(lic) && ext_ok(ext) && (!strncmp(url2, "https://", 8) || !strncmp(url2, "http://", 7))) {
                if (shortonly && ms > 60000) hidden++;
                else {
                    val_str(obj_get(p, "title"), r->title, sizeof r->title);
                    val_str(obj_get(p, "creator"), r->creator, sizeof r->creator);
                    snprintf(r->license, sizeof r->license, "%s", !strcasecmp(lic, "cc0") ? "CC0" : "PDM");
                    snprintf(r->url, sizeof r->url, "%s", url2);
                    snprintf(r->ext, sizeof r->ext, "%s", ext);
                    r->dur = (int)(ms / 1000.0 + 0.5);
                    if (!r->title[0]) snprintf(r->title, sizeof r->title, "(no title)");
                    n++;
                }
            } else if (!lic_ok(lic) && lic[0]) hidden++;
            p = end;
        }
    }
    free(b.d);
    if (!use_wiki && n == 0 && apage == 1) use_wiki = 1;
    if (use_wiki) {
        n = 0; hidden = 0; memset(tmp, 0, sizeof(res_t) * NRES);
        n = wiki_search(qe, apage, shortonly, tmp, &hidden, err, sizeof err);
        if (n < 0) {
            char m[160];
            snprintf(m, sizeof m, "No luck. Openverse %ld, Wikimedia: %.60s", ovcode, err[0] ? err : "no connection");
            status_set(m); free(tmp); atomic_store(&G.busy, 0); return NULL;
        }
    }
    LOCK();
    G.src = use_wiki ? 1 : 0;
    memcpy(G.res, tmp, sizeof(res_t) * NRES);
    G.nres = n; G.rpage = 0; G.rsel = -1; G.apage = apage; G.hidden = hidden; G.rev++;
    UNLOCK();
    free(tmp);
    char m[120];
    if (n) snprintf(m, sizeof m, "%d sounds (page %d)%s%s", n, apage, hidden ? ", some hidden" : "", use_wiki ? "  [Wikimedia]" : "");
    else snprintf(m, sizeof m, apage > 1 ? "No more results" : "Nothing found");
    status_set(m);
    atomic_store(&G.busy, 0);
    return NULL;
}

typedef struct { char url[400], title[80], creator[48], lic[8]; } pjob_t;

static void *job_preview(void *arg) {
    pjob_t *j = arg;
    char err[100] = "";
    buf_t b;
    long code;
    status_set("Loading sound...");
    if (http_get(j->url, &b, MAX_DL, 60, err, sizeof err, &code)) {
        char m[160];
        snprintf(m, sizeof m, "Load failed: %.100s", err);
        status_set(m);
    } else {
        int16_t *d = NULL;
        int fr = 0;
        const char *why = "";
        if (decode_any(b.d, b.n, &d, &fr, &why) == 0) {
            pv_install(d, fr, j->title, j->lic, j->creator, j->url, 0);
            status_set("Playing");
        } else {
            char m[160];
            snprintf(m, sizeof m, "Cannot play: %s", why);
            status_set(m);
        }
        free(b.d);
    }
    free(j);
    atomic_store(&G.busy, 0);
    return NULL;
}

/* ---------- the crate: kept sounds as WAV + a text file with title, creator, license and source ---------- */

static int cmp_cr(const void *a, const void *b) {
    const cfile_t *x = a, *y = b;
    if (x->mt != y->mt) return x->mt < y->mt ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

static void crate_scan(void) {
    char dir[300];
    LOCK(); snprintf(dir, sizeof dir, "%s/crate", G.base); UNLOCK();
    static __thread cfile_t tmp[200];
    int n = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < 200) {
            size_t l = strlen(e->d_name);
            if (e->d_name[0] == '.' || l < 5 || l >= sizeof tmp[0].name || strcasecmp(e->d_name + l - 4, ".wav")) continue;
            char p[420];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
            snprintf(tmp[n].name, sizeof tmp[n].name, "%s", e->d_name);
            tmp[n].size = st.st_size; tmp[n].mt = st.st_mtime;
            n++;
        }
        closedir(d);
    }
    qsort(tmp, (size_t)n, sizeof tmp[0], cmp_cr);
    LOCK();
    memcpy(G.cr, tmp, sizeof(cfile_t) * (size_t)n);
    G.ncr = n;
    if (G.csel >= n) G.csel = -1;
    G.rev++;
    UNLOCK();
}

static void safe_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 1 < cap && o < 50; in++) out[o++] = (isalnum((unsigned char)*in) || *in == '-' || *in == ' ') ? *in : '_';
    while (o && (out[o - 1] == ' ' || out[o - 1] == '_')) o--;
    out[o] = 0;
    if (!o) snprintf(out, cap, "sound");
}

static void *job_keep(void *arg) {
    (void)arg;
    char name[80], lic[8], creator[48], url[400], dir[300];
    pthread_mutex_lock(&G.padmu);
    int fr = G.pv_frames;
    int16_t *copy = G.pv && fr > 0 ? malloc((size_t)fr * 4) : NULL;
    if (copy) memcpy(copy, G.pv, (size_t)fr * 4);
    pthread_mutex_unlock(&G.padmu);
    LOCK();
    snprintf(name, sizeof name, "%s", G.pv_name); snprintf(lic, sizeof lic, "%s", G.pv_lic);
    snprintf(creator, sizeof creator, "%s", G.pv_creator); snprintf(url, sizeof url, "%s", G.pv_url);
    snprintf(dir, sizeof dir, "%s/crate", G.base);
    UNLOCK();
    if (!copy) { note_set("Nothing to keep: play a sound first"); atomic_store(&G.busy, 0); return NULL; }
    char sn[80], path[420];
    safe_name(name, sn, sizeof sn);
    int ok = 0;
    for (int k = 0; k < 100 && !ok; k++) {
        if (k) snprintf(path, sizeof path, "%s/%s-%d.wav", dir, sn, k); else snprintf(path, sizeof path, "%s/%s.wav", dir, sn);
        ok = wav_write(path, copy, fr, 1) == 0;
        if (!ok && errno != EEXIST) break;
    }
    free(copy);
    if (ok) {
        char tp[430];
        snprintf(tp, sizeof tp, "%.*s.txt", (int)strlen(path) - 4, path);
        FILE *f = fopen(tp, "w");
        if (f) { fprintf(f, "Title: %s\nCreator: %s\nLicense: %s\nSource: %s\nFound with: Openverse (openverse.org)\n", name, creator, !strcmp(lic, "CC0") ? "CC0 1.0 (public domain dedication)" : "Public Domain Mark", url); fclose(f); }
        LOCK(); G.pv_kept = 1; UNLOCK();
        note_set("Kept in your CRATE");
        crate_scan();
    } else note_set("Could not save the sound");
    atomic_store(&G.busy, 0);
    return NULL;
}

typedef struct { char path[420], name[80]; } cjob_t;

static void *job_cplay(void *arg) {
    cjob_t *j = arg;
    FILE *f = fopen(j->path, "rb");
    int ok = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 44 && n < (60L << 20)) {
            uint8_t *buf = malloc((size_t)n);
            int16_t *d = NULL;
            int fr = 0;
            const char *why;
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n && wav_decode(buf, (size_t)n, &d, &fr, &why) == 0) {
                char tp[430], lic[8] = "", cr[48] = "", ln[200];
                snprintf(tp, sizeof tp, "%.*s.txt", (int)strlen(j->path) - 4, j->path);
                FILE *t = fopen(tp, "r");
                if (t) {
                    while (fgets(ln, sizeof ln, t)) {
                        if (!strncmp(ln, "License: CC0", 12)) snprintf(lic, sizeof lic, "CC0");
                        else if (!strncmp(ln, "License: Public", 15)) snprintf(lic, sizeof lic, "PDM");
                        else if (!strncmp(ln, "Creator: ", 9)) { snprintf(cr, sizeof cr, "%s", ln + 9); cr[strcspn(cr, "\n")] = 0; }
                    }
                    fclose(t);
                }
                pv_install(d, fr, j->name, lic, cr, "", 1);
                ok = 1;
            }
            free(buf);
        }
        fclose(f);
    }
    if (!ok) note_set("Cannot read that file");
    free(j);
    atomic_store(&G.busy, 0);
    return NULL;
}

static void *job_test(void *arg) {
    (void)arg;
    char err[100] = "", m[160], url[300], bearer[720];
    buf_t b;
    long code;
    status_set("Testing the connection...");
    int has = curl_load();
    bearer_get(bearer, sizeof bearer);
    snprintf(url, sizeof url, "%srate_limit/", api_root());
    req_t rq = { NULL, bearer };
    int rc = http_req(&rq, url, &b, 1 << 16, 25, err, sizeof err, &code);
    char info[160];
    if (rc && code == 429) snprintf(info, sizeof info, "%s: online, but Openverse says too many requests", has ? "libcurl" : "curl/wget");
    else if (rc && code == 401) snprintf(info, sizeof info, "%s: online, account not accepted", has ? "libcurl" : "curl/wget");
    else if (rc) snprintf(info, sizeof info, "%s: %s", has ? "libcurl" : "curl/wget", err);
    else {
        char today[24] = "?";
        const char *v = b.d ? obj_get((const char *)b.d, "requests_today") : NULL;
        if (v) snprintf(today, sizeof today, "%d", (int)val_num(v));
        const char *vf = b.d ? obj_get((const char *)b.d, "verified") : NULL;
        snprintf(info, sizeof info, "%s: online. %s%s%s", has ? "libcurl" : "curl/wget", bearer[0] ? "Account " : "No account needed. ",
                 bearer[0] ? (vf && !strncmp(vf, "true", 4) ? "verified" : "NOT verified yet") : "", bearer[0] ? "" : "");
        (void)today;
    }
    free(b.d);
    LOCK(); snprintf(G.netinfo, sizeof G.netinfo, "%s", info); UNLOCK();
    snprintf(m, sizeof m, "%s", rc ? "Test: see INFO tab" : "Connection OK");
    status_set(m);
    atomic_store(&G.busy, 0);
    return NULL;
}

/* after REGISTER (and at every start with an account that is not confirmed yet): ask Openverse every 6 seconds whether the e-mail link was tapped,
 * so there is nothing to press: the status turns to "Account ready" by itself */
static void *job_verify(void *arg) {
    (void)arg;
    if (atomic_exchange(&G.verifying, 1)) return NULL;
    for (int tries = 0; tries < 400; tries++) {
        char bearer[720], url[300], err[100];
        buf_t b; long code = 0;
        LOCK(); G.tok[0] = 0; UNLOCK();
        bearer_get(bearer, sizeof bearer);
        int ok = 0;
        if (bearer[0]) {
            snprintf(url, sizeof url, "%srate_limit/", api_root());
            req_t rq = { NULL, bearer };
            if (http_req(&rq, url, &b, 1 << 16, 10, err, sizeof err, &code) == 0) {
                const char *vf = b.d ? obj_get((const char *)b.d, "verified") : NULL;
                ok = vf && !strncmp(vf, "true", 4);
                free(b.d);
            }
        }
        LOCK(); G.acc_state = ok ? 2 : 1; G.rev++; UNLOCK();
        if (ok) { status_set("Account ready: searching is open now"); break; }
        { char wm[120]; snprintf(wm, sizeof wm, bearer[0] ? "Waiting for your e-mail link (check %d, code %ld)" : "Waiting for your e-mail link (check %d, no key yet)", tries + 1, code); status_set(wm); }
        for (int k = 0; k < 6; k++) { sleep(1); LOCK(); int gone = !G.cid[0]; UNLOCK(); if (gone) { atomic_store(&G.verifying, 0); return NULL; } }
    }
    atomic_store(&G.verifying, 0);
    return NULL;
}

/* the one-time free account: name, purpose and the typed e-mail address go to Openverse; the id and secret that come back are kept in openverse.txt */
static void *job_register(void *arg) {
    (void)arg;
    char email[64], ee[200], nm[40], url[300], post[600], err[100], m[200];
    LOCK(); snprintf(email, sizeof email, "%s", G.email); UNLOCK();
    uint32_t rnd = (uint32_t)time(NULL) ^ (uint32_t)(mono() * 1000.0) ^ (uint32_t)(uintptr_t)&rnd;
    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) { if (fread(&rnd, 1, sizeof rnd, ur) != sizeof rnd) rnd ^= 0x9e3779b9u; fclose(ur); }
    snprintf(nm, sizeof nm, "crate-digga-%08x", rnd);
    urlenc(email, ee, sizeof ee);
    snprintf(url, sizeof url, "%sauth_tokens/register/", api_root());
    snprintf(post, sizeof post, "name=%s&description=Personal+tool+on+an+MPC+to+find+CC0+and+public+domain+sounds&email=%s", nm, ee);
    req_t rq = { post, NULL };
    buf_t b;
    long code;
    status_set("Registering...");
    int rc = http_req(&rq, url, &b, 1 << 16, 30, err, sizeof err, &code);
    if (rc == 0 && b.d) {
        char id[80], sec[80];
        val_str(obj_get((const char *)b.d, "client_id"), id, sizeof id);
        val_str(obj_get((const char *)b.d, "client_secret"), sec, sizeof sec);
        if (id[0] && sec[0]) {
            char p[300];
            LOCK(); snprintf(p, sizeof p, "%s/openverse.txt", G.base); UNLOCK();
            int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
            int ok = fd >= 0;
            if (ok) { char line[200]; int n = snprintf(line, sizeof line, "%s\n%s\n", id, sec); ok = write(fd, line, (size_t)n) == n; close(fd); }
            if (ok) {
                LOCK(); snprintf(G.cid, sizeof G.cid, "%s", id); snprintf(G.csec, sizeof G.csec, "%s", sec); G.tok[0] = 0; UNLOCK();
                snprintf(m, sizeof m, "Done. Now tap the link in the e-mail from Openverse. This screen notices it by itself");
                LOCK(); G.acc_state = 1; UNLOCK();
            } else snprintf(m, sizeof m, "Registered, but the key could not be saved");
        } else snprintf(m, sizeof m, "Unexpected answer from Openverse");
    } else if (code == 429) snprintf(m, sizeof m, "Openverse says too many requests: try again in a while");
    else if (code == 400) snprintf(m, sizeof m, "Openverse refused the address: check the e-mail spelling");
    else snprintf(m, sizeof m, "Register failed: %.100s", err);
    free(b.d);
    status_set(m);
    atomic_store(&G.busy, 0);
    LOCK(); int started = G.acc_state == 1; UNLOCK();
    if (started) job_verify(NULL);
    return NULL;
}

static int start_search(int apage) {
    if (atomic_exchange(&G.busy, 1)) { note_set("Busy, wait a moment"); return 0; }
    sjob_t *j = malloc(sizeof *j);
    if (!j) { atomic_store(&G.busy, 0); return 0; }
    j->apage = apage < 1 ? 1 : apage;
    spawn_job(job_search, j);
    return 1;
}

/* ---------- the 16 pads: saved on the card (pads/padNN.wav + pads/pads.cfg), so they are back after a restart ---------- */

static const int ATT_MS[] = { 0, 2, 5, 10, 20, 50, 100, 200, 500 };
static const int DEC_MS[] = { 0, 50, 100, 200, 400, 800, 1500, 3000 };      /* 0 = off: the whole sample plays */
static const int REL_MS[] = { 5, 20, 50, 100, 200, 400, 800, 1500 };
#define N_ATT ((int)(sizeof ATT_MS / sizeof *ATT_MS))
#define N_DEC ((int)(sizeof DEC_MS / sizeof *DEC_MS))
#define N_REL ((int)(sizeof REL_MS / sizeof *REL_MS))
#define N_GRP 9

static void pc_default(void) { for (int i = 0; i < NPADS; i++) { G.pc[i].att = 0; G.pc[i].dec = 0; G.pc[i].rel = 1; G.pc[i].grp = 0; G.pc[i].mode = 0; } }

static void cfg_save(void) {
    char dir[300], path[330], tmp[340];
    signed char map[128];
    LOCK(); snprintf(dir, sizeof dir, "%s/pads", G.base); memcpy(map, G.padmap, sizeof map); int have = G.base[0]; UNLOCK();
    if (!have) return;
    mkdir(dir, 0755);
    char names[NPADS][40];
    unsigned char pcs[NPADS][5];
    pthread_mutex_lock(&G.padmu);
    for (int i = 0; i < NPADS; i++) { snprintf(names[i], sizeof names[i], "%s", G.pad[i].d ? G.pad[i].name : ""); pcs[i][0] = G.pc[i].att; pcs[i][1] = G.pc[i].dec; pcs[i][2] = G.pc[i].rel; pcs[i][3] = G.pc[i].grp; pcs[i][4] = G.pc[i].mode; }
    pthread_mutex_unlock(&G.padmu);
    snprintf(path, sizeof path, "%s/pads.cfg", dir);
    snprintf(tmp, sizeof tmp, "%s/pads.tmp", dir);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < NPADS; i++) { if (names[i][0]) fprintf(f, "name %d %s\n", i, names[i]); fprintf(f, "env %d %d %d %d %d %d\n", i, pcs[i][0], pcs[i][1], pcs[i][2], pcs[i][3], pcs[i][4]); }
    for (int n = 0; n < 128; n++) if (map[n] >= 0) fprintf(f, "map %d %d\n", n, map[n]);
    if (fclose(f) == 0) rename(tmp, path); else unlink(tmp);
}
static void *cfg_save_thread(void *a) { (void)a; cfg_save(); return NULL; }
static void cfg_save_async(void) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_create(&t, &at, cfg_save_thread, NULL);
    pthread_attr_destroy(&at);
}
static void map_default(void) {
    memset(G.padmap, -1, sizeof G.padmap);
    for (int k = 0; k < 16; k++) { G.padmap[k] = (signed char)k; G.padmap[36 + k] = (signed char)k; }
}

static void *boot_thread(void *a) {
    (void)a;
    char dir[300], line[200], names[NPADS][40], path[330];
    memset(names, 0, sizeof names);
    LOCK(); snprintf(dir, sizeof dir, "%s/pads", G.base); UNLOCK();
    snprintf(path, sizeof path, "%s/pads.cfg", dir);
    FILE *f = fopen(path, "r");
    signed char map[128];
    memset(map, -1, sizeof map);
    int have_map = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            int i, nn;
            char nm[60];
            if (sscanf(line, "name %d %39[^\n]", &i, nm) == 2 && i >= 0 && i < NPADS) snprintf(names[i], sizeof names[i], "%s", nm);
            else if (!strncmp(line, "env ", 4)) {
                int pi, a, d, rl, g, md;
                if (sscanf(line, "env %d %d %d %d %d %d", &pi, &a, &d, &rl, &g, &md) == 6 && pi >= 0 && pi < NPADS && a >= 0 && a < N_ATT && d >= 0 && d < N_DEC && rl >= 0 && rl < N_REL && g >= 0 && g < N_GRP) {
                    pthread_mutex_lock(&G.padmu);
                    G.pc[pi].att = (unsigned char)a; G.pc[pi].dec = (unsigned char)d; G.pc[pi].rel = (unsigned char)rl; G.pc[pi].grp = (unsigned char)g; G.pc[pi].mode = (unsigned char)(md ? 1 : 0);
                    pthread_mutex_unlock(&G.padmu);
                }
            }
            else if (sscanf(line, "map %d %d", &nn, &i) == 2 && nn >= 0 && nn < 128 && i >= 0 && i < NPADS) { map[nn] = (signed char)i; have_map = 1; }
        }
        fclose(f);
    }
    if (have_map) { LOCK(); memcpy(G.padmap, map, sizeof map); UNLOCK(); }
    for (int i = 0; i < NPADS; i++) {
        snprintf(path, sizeof path, "%s/pad%02d.wav", dir, i + 1);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd < 0) continue;
        if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 44 && st.st_size < (80LL << 20)) {
            uint8_t *buf = malloc((size_t)st.st_size);
            int16_t *d = NULL;
            int fr = 0;
            const char *why;
            if (buf && read(fd, buf, (size_t)st.st_size) == st.st_size && wav_decode(buf, (size_t)st.st_size, &d, &fr, &why) == 0) {
                pthread_mutex_lock(&G.padmu);
                free(G.pad[i].d);
                G.pad[i].d = d; G.pad[i].frames = fr;
                snprintf(G.pad[i].name, sizeof G.pad[i].name, "%s", names[i][0] ? names[i] : "sample");
                pthread_mutex_unlock(&G.padmu);
            }
            free(buf);
        }
        close(fd);
    }
    crate_scan();
    return NULL;
}

static void pad_assign(int p) {   /* the sound that was played last goes on pad p */
    char name[80], dir[300], pp[330];
    pthread_mutex_lock(&G.padmu);
    int fr = G.pv_frames;
    int16_t *copy = G.pv && fr > 0 ? malloc((size_t)fr * 4) : NULL;
    if (copy) memcpy(copy, G.pv, (size_t)fr * 4);
    pthread_mutex_unlock(&G.padmu);
    if (!copy) { note_set("Play a sound first, then press TO PAD"); return; }
    LOCK(); snprintf(name, sizeof name, "%s", G.pv_name); snprintf(dir, sizeof dir, "%s/pads", G.base); UNLOCK();
    snprintf(pp, sizeof pp, "%s/pad%02d.wav", dir, p + 1);
    mkdir(dir, 0755);
    wav_write(pp, copy, fr, 0);
    pthread_mutex_lock(&G.padmu);
    free(G.pad[p].d);
    G.pad[p].d = copy; G.pad[p].frames = fr;
    disp_text(G.pad[p].name, sizeof G.pad[p].name, name, 26);
    pthread_mutex_unlock(&G.padmu);
    cfg_save();
    char m[100];
    snprintf(m, sizeof m, "Pad %d loaded", p + 1);
    note_set(m);
}

/* ---------- plugin instance ---------- */

typedef struct {
    atomic_int trig[NPADS];
    atomic_int rel[NPADS];            /* a note-off (hold mode) asks for the release */
    atomic_int stop;                  /* the transport stopped */
    struct { int on; long pos; float gain; long age, rage; } voice[NPADS];   /* rage < 0: not releasing */
    deck_t dk;
    int plat_rep;
    unsigned seen_gen; double pv_posf; long pv_fade;
    double ang, anchor; int scr, hopc; struct { double src; int age; } gr[5]; int ngr;
    int was_playing;
    double born;
} inst_t;

static void *create(const char *dir) {
    (void)dir;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->born = mono();
    deck_init(&s->dk);
    LOCK();
    if (!G.base[0]) make_dirs();
    G.refs++;
    int boot = !G.boot;
    if (boot) { G.boot = 1; map_default(); pc_default(); }
    UNLOCK();
    if (boot) { account_load(); LOCK(); int has = G.cid[0]; if (has) G.acc_state = 1; UNLOCK(); if (has) spawn_job(job_verify, NULL); }
    if (boot) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_create(&t, &at, boot_thread, NULL);
        pthread_attr_destroy(&at);
    }
    return s;
}

static void destroy(void *i) {
    LOCK();
    int last = --G.refs <= 0;
    if (last) G.boot = 0;
    UNLOCK();
    free(i);
    if (last) {
        pthread_mutex_lock(&G.padmu);
        for (int k = 0; k < NPADS; k++) { free(G.pad[k].d); G.pad[k].d = NULL; G.pad[k].frames = 0; }
        free(G.pv); G.pv = NULL; G.pv_frames = 0; G.pv_play = 0;
        pthread_mutex_unlock(&G.padmu);
    }
}

static void pad_trigger(inst_t *s, int p, int vel) {
    if (p < 0 || p >= NPADS) return;
    atomic_store(&s->trig[p], vel < 1 ? 1 : vel > 127 ? 127 : vel);
}

static void midi(void *i, const uint8_t *m, int n) {
    inst_t *s = i;
    if (n >= 3 && (m[0] & 0xF0) == 0x80) {   /* note off */
        LOCK(); int p = G.padmap[m[1] & 127]; UNLOCK();
        if (p >= 0) atomic_store(&s->rel[p], 1);
        return;
    }
    if (n >= 3 && (m[0] & 0xF0) == 0x90 && m[2] == 0) {
        LOCK(); int p = G.padmap[m[1] & 127]; UNLOCK();
        if (p >= 0) atomic_store(&s->rel[p], 1);
        return;
    }
    if (n >= 3 && (m[0] & 0xF0) == 0x90 && m[2] > 0) {
        int note = m[1] & 127, p, learned = 0;
        char msg[100] = "";
        LOCK();
        if (G.learn == 1) {
            for (int k = 0; k < 128; k++) if (G.padmap[k] == G.pad_sel) G.padmap[k] = -1;
            G.padmap[note] = (signed char)G.pad_sel;
            snprintf(msg, sizeof msg, "Pad %d now plays when MIDI note %d arrives", G.pad_sel + 1, note);
            G.learn = 0; learned = 1;
        } else if (G.learn == 2) {
            memset(G.padmap, -1, sizeof G.padmap);
            for (int k = 0; k < 16 && note + k < 128; k++) G.padmap[note + k] = (signed char)k;
            snprintf(msg, sizeof msg, "Learned: the pad you hit is pad 1 (note %d), the others follow", note);
            G.learn = 0; learned = 1;
        }
        p = G.padmap[note];
        G.last_note = note; G.last_pad = p; G.rev++;
        UNLOCK();
        if (learned) { note_set(msg); cfg_save_async(); }
        atomic_store(&s->rel[p < 0 ? 0 : p], 0);
        pad_trigger(s, p, m[2]);
    }
}

/* the host's transport: when it stops, every sound fades out, like the other sounds in the project */
static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    (void)ppq; (void)bpm; (void)frames;
    inst_t *s = inst;
    if (s->was_playing && !playing) atomic_store(&s->stop, 1);
    s->was_playing = playing ? 1 : 0;
}

static int tap_repeat(const char *key) {   /* the MPC sends a tap twice now and then */
    static char last[24];
    static double t;
    double now = mono();
    if (!strcmp(last, key) && now - t < 0.25) return 1;
    snprintf(last, sizeof last, "%s", key);
    t = now;
    return 0;
}

static int key_idx(const char *key, const char *prefix, int max) {
    size_t pl = strlen(prefix);
    if (strncmp(key, prefix, pl) || !isdigit((unsigned char)key[pl])) return 0;
    char *end;
    long v = strtol(key + pl, &end, 10);
    return *end == 0 && v >= 1 && v <= max ? (int)v : 0;
}

static const char KEYS[] = "qwertyuiopasdfghjklzxcvbnm";
static const char *const QUICK[] = { "kick", "snare", "hihat", "clap", "bass", "808", "loop", "fx" };
#define NQUICK 8

static void q_add(char c) { LOCK(); size_t l = strlen(G.q); if (l < sizeof G.q - 2) { G.q[l] = c; G.q[l + 1] = 0; G.rev++; } UNLOCK(); }

static void play_result(int idx) {   /* idx is an index into G.res; the tap also selects it */
    pjob_t *j = NULL;
    LOCK();
    if (idx >= 0 && idx < G.nres) {
        G.rsel = idx;
        G.rev++;
        j = malloc(sizeof *j);
        if (j) {
            snprintf(j->url, sizeof j->url, "%s", G.res[idx].url); snprintf(j->title, sizeof j->title, "%s", G.res[idx].title);
            snprintf(j->creator, sizeof j->creator, "%s", G.res[idx].creator); snprintf(j->lic, sizeof j->lic, "%s", G.res[idx].license);
        }
    }
    UNLOCK();
    if (!j) return;
    if (atomic_exchange(&G.busy, 1)) { free(j); note_set("Busy, wait a moment"); return; }
    spawn_job(job_preview, j);
}

static void set_param(void *inst, const char *key, const char *val) {
    inst_t *s = inst;
    deck_t *dk = &s->dk;
    int v = atoi(val);
    int fire = atof(val) > 0.5;
    if (!strcmp(key, "cat")) { LOCK(); G.cat = v < 0 ? 0 : v > 2 ? 2 : v; G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "shortonly")) { LOCK(); G.shortonly = v ? 1 : 0; G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "pitch")) {
        int v = (int)(atof(val) * 1000.0 + 0.5); if (v < 0) v = 0; if (v > 1000) v = 1000;
        if (v > 485 && v < 515) v = 500;   /* detent in the middle: exactly 0 % */
        atomic_store(&dk->pitch, v); LOCK(); G.rev++; UNLOCK(); return;
    }
    if (!strcmp(key, "prange")) { atomic_store(&dk->prange, atoi(val) % 3); LOCK(); G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "klock")) { atomic_store(&dk->klock, atoi(val) ? 1 : 0); LOCK(); G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "jog")) {
        int v = (int)(atof(val) * 1000.0 + 0.5); if (v < 0) v = 0; if (v > 1000) v = 1000;
        long long t = now_ms();
        if (t - atomic_load(&dk->jogt) > 250) { atomic_store(&dk->jogref, atomic_load(&dk->joglast)); atomic_store(&dk->jogstart, 1); }
        atomic_store(&dk->jogv, v); atomic_store(&dk->joglast, v); atomic_store(&dk->jogt, t);
        return;
    }
    if (!fire) return;
    int k;
    if ((k = key_idx(key, "pd", NPADS))) {
        LOCK(); G.pad_sel = k - 1; G.rev++; UNLOCK();
        pad_trigger(s, k - 1, 110);
        return;
    }
    if (tap_repeat(key)) return;
    if (key[0] == 'k' && key[1] == 'c' && isdigit((unsigned char)key[2])) { int n = atoi(key + 2); if (n >= 0 && n < 26) q_add(KEYS[n]); return; }
    if (!strcmp(key, "k_space")) { q_add(' '); return; }
    if (!strcmp(key, "k_back")) { LOCK(); size_t l = strlen(G.q); if (l) G.q[l - 1] = 0; G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "k_clear")) { LOCK(); G.q[0] = 0; G.rev++; UNLOCK(); return; }
    if ((k = key_idx(key, "pq", NQUICK))) { LOCK(); snprintf(G.q, sizeof G.q, "%s", QUICK[k - 1]); G.rev++; UNLOCK(); start_search(1); return; }
    if (!strcmp(key, "do_search")) {
        LOCK(); int e = !G.q[0]; UNLOCK();
        if (e) { note_set("Type a word first, or tap a preset"); return; }
        start_search(1);
        return;
    }
    if (key[0] == 'e' && key[1] == 'c' && isdigit((unsigned char)key[2])) {   /* a key of the e-mail keyboard on the ACCOUNT tab: 0-25 letters, 26-35 digits, 36 @, 37 ., 38 _, 39 - */
        static const char EK[] = "qwertyuiopasdfghjklzxcvbnm0123456789@._-";
        int n = atoi(key + 2);
        if (n >= 0 && n < 40) { LOCK(); size_t l = strlen(G.email); if (l < sizeof G.email - 2) { G.email[l] = EK[n]; G.email[l + 1] = 0; G.rev++; } UNLOCK(); }
        return;
    }
    if (!strcmp(key, "e_back")) { LOCK(); size_t l = strlen(G.email); if (l) G.email[l - 1] = 0; G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "e_clear")) { LOCK(); G.email[0] = 0; G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "deck_cue")) {
        pthread_mutex_lock(&G.padmu); G.pv_play = 0; G.pv_gen++; pthread_mutex_unlock(&G.padmu); return;
    }
    if (!strcmp(key, "register")) {
        LOCK(); int ok = strchr(G.email, '@') && strchr(strchr(G.email, '@'), '.') && strlen(G.email) > 5; int has = G.cid[0]; UNLOCK();
        if (!ok) { note_set("Type your e-mail address first"); return; }
        if (has) { note_set("An account is already saved: press TEST"); return; }
        if (atomic_exchange(&G.busy, 1)) { note_set("Busy, wait a moment"); return; }
        spawn_job(job_register, NULL);
        return;
    }
    if (!strcmp(key, "acc_forget")) {
        char p[300];
        LOCK(); snprintf(p, sizeof p, "%s/openverse.txt", G.base); G.cid[0] = 0; G.csec[0] = 0; G.tok[0] = 0; G.acc_state = 0; G.rev++; UNLOCK();
        unlink(p);
        note_set("Account key removed from this MPC");
        return;
    }
    {   /* the envelope of the selected pad */
        int which = 0, dir = 0;
        if (!strcmp(key, "att_m")) { which = 1; dir = -1; } else if (!strcmp(key, "att_p")) { which = 1; dir = 1; }
        else if (!strcmp(key, "dec_m")) { which = 2; dir = -1; } else if (!strcmp(key, "dec_p")) { which = 2; dir = 1; }
        else if (!strcmp(key, "rel_m")) { which = 3; dir = -1; } else if (!strcmp(key, "rel_p")) { which = 3; dir = 1; }
        else if (!strcmp(key, "grp_m")) { which = 4; dir = -1; } else if (!strcmp(key, "grp_p")) { which = 4; dir = 1; }
        else if (!strcmp(key, "mode_t")) { which = 5; dir = 1; }
        if (which) {
            LOCK(); int p = G.pad_sel; UNLOCK();
            pthread_mutex_lock(&G.padmu);
            unsigned char *f = which == 1 ? &G.pc[p].att : which == 2 ? &G.pc[p].dec : which == 3 ? &G.pc[p].rel : which == 4 ? &G.pc[p].grp : &G.pc[p].mode;
            int mx = which == 1 ? N_ATT : which == 2 ? N_DEC : which == 3 ? N_REL : which == 4 ? N_GRP : 2;
            int nv = *f + dir;
            if (which == 5) nv = (*f + 1) % 2;
            if (nv >= 0 && nv < mx) *f = (unsigned char)nv;
            pthread_mutex_unlock(&G.padmu);
            LOCK(); G.rev++; UNLOCK();
            cfg_save_async();
            return;
        }
    }
    if ((k = key_idx(key, "rt", ROWS))) { LOCK(); int i = G.rpage * ROWS + k - 1; UNLOCK(); play_result(i); return; }
    if (!strcmp(key, "r_prev")) {
        LOCK(); int p = G.rpage, ap = G.apage; if (p > 0) { G.rpage--; G.rev++; } UNLOCK();
        if (p == 0 && ap > 1) start_search(ap - 1);
        return;
    }
    if (!strcmp(key, "r_next")) {
        LOCK(); int more = (G.rpage + 1) * ROWS < G.nres, ap = G.apage, nres = G.nres; if (more) { G.rpage++; G.rev++; } UNLOCK();
        if (!more && nres > 0) start_search(ap + 1);
        return;
    }
    if (!strcmp(key, "pv_play")) {
        pthread_mutex_lock(&G.padmu);
        if (G.pv) { if (G.pv_play) G.pv_play = 0; else { G.pv_play = 1; G.pv_gen++; } }
        pthread_mutex_unlock(&G.padmu);
        return;
    }
    if (!strcmp(key, "keep")) {
        LOCK(); int valid = G.pv_valid, kept = G.pv_kept; UNLOCK();
        if (!valid) { note_set("Play a sound first, then press KEEP"); return; }
        if (kept) { note_set("Already in your CRATE"); return; }
        if (atomic_exchange(&G.busy, 1)) { note_set("Busy, wait a moment"); return; }
        spawn_job(job_keep, NULL);
        return;
    }
    if (!strcmp(key, "to_pad")) { LOCK(); int p = G.pad_sel; UNLOCK(); pad_assign(p); return; }
    if ((k = key_idx(key, "ct", ROWS))) {
        cjob_t *j = NULL;
        LOCK();
        int i = G.cpage * ROWS + k - 1;
        if (i < G.ncr) {
            G.csel = i; G.rev++;
            j = malloc(sizeof *j);
            if (j) { snprintf(j->path, sizeof j->path, "%s/crate/%s", G.base, G.cr[i].name); snprintf(j->name, sizeof j->name, "%.*s", (int)strlen(G.cr[i].name) - 4, G.cr[i].name); }
        }
        UNLOCK();
        if (j) { if (atomic_exchange(&G.busy, 1)) { free(j); note_set("Busy, wait a moment"); } else spawn_job(job_cplay, j); }
        return;
    }
    if (!strcmp(key, "c_prev")) { LOCK(); if (G.cpage > 0) { G.cpage--; G.rev++; } UNLOCK(); return; }
    if (!strcmp(key, "c_next")) { LOCK(); if ((G.cpage + 1) * ROWS < G.ncr) { G.cpage++; G.rev++; } UNLOCK(); return; }
    if (!strcmp(key, "c_del")) {
        char p[420] = "";
        LOCK();
        if (G.csel >= 0 && G.csel < G.ncr) snprintf(p, sizeof p, "%s/crate/%s", G.base, G.cr[G.csel].name);
        UNLOCK();
        if (!p[0]) { note_set("Tap a sound in the crate first"); return; }
        unlink(p);
        size_t l = strlen(p);
        if (l > 4) { strcpy(p + l - 4, ".txt"); unlink(p); }
        LOCK(); G.csel = -1; UNLOCK();
        crate_scan();
        note_set("Removed from your crate");
        return;
    }
    if (!strcmp(key, "pad_clear")) {
        LOCK(); int p = G.pad_sel; char base[200]; snprintf(base, sizeof base, "%s", G.base); UNLOCK();
        pthread_mutex_lock(&G.padmu);
        int16_t *old = G.pad[p].d;
        G.pad[p].d = NULL; G.pad[p].frames = 0; G.pad[p].name[0] = 0;
        pthread_mutex_unlock(&G.padmu);
        free(old);
        char pp[300];
        snprintf(pp, sizeof pp, "%s/pads/pad%02d.wav", base, p + 1);
        unlink(pp);
        cfg_save();
        return;
    }
    if (!strcmp(key, "learn_pad") || !strcmp(key, "learn_base")) {
        int want = key[6] == 'p' ? 1 : 2;
        LOCK(); G.learn = G.learn == want ? 0 : want; int l = G.learn; G.rev++; UNLOCK();
        note_set(l == 1 ? "Now hit the physical pad that should play this pad" : l == 2 ? "Now hit the physical pad that is PAD 1 (bottom left)" : "Learn cancelled");
        return;
    }
    if (!strcmp(key, "net_test")) {
        if (atomic_exchange(&G.busy, 1)) { note_set("Busy, wait a moment"); return; }
        spawn_job(job_test, NULL);
        return;
    }
}

static int put(char *b, int n, const char *s) { int r = snprintf(b, (size_t)n, "%s", s[0] ? s : " "); return r > 0 ? r : 1; }

static int is_trigger(const char *key) {
    static const char *const fixed[] = { "k_space", "k_back", "k_clear", "do_search", "r_prev", "r_next", "pv_play", "keep", "to_pad", "c_prev", "c_next", "c_del",
                                         "pad_clear", "learn_pad", "learn_base", "net_test", "e_back", "e_clear", "register", "acc_forget",
                                         "deck_cue", "att_m", "att_p", "dec_m", "dec_p", "rel_m", "rel_p", "grp_m", "grp_p", "mode_t" };
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++) if (!strcmp(key, fixed[i])) return 1;
    return (key[0] == 'k' && key[1] == 'c' && isdigit((unsigned char)key[2])) || key_idx(key, "pq", NQUICK) || key_idx(key, "rt", ROWS) ||
           key_idx(key, "ct", ROWS) || key_idx(key, "pd", NPADS) ||
           (key[0] == 'e' && key[1] == 'c' && isdigit((unsigned char)key[2]));
}

static int get_param(void *inst, const char *key, char *b, int n) {
    deck_t *dk = &((inst_t *)inst)->dk;
    char t[300] = "";
    int k;
    if (n <= 1) return 0;
    if (!strcmp(key, "cat")) return snprintf(b, (size_t)n, "%d", G.cat);
    if (!strcmp(key, "shortonly")) return snprintf(b, (size_t)n, "%d", G.shortonly);
    if (is_trigger(key)) return snprintf(b, (size_t)n, "0");
    if (!strcmp(key, "q_txt")) { LOCK(); snprintf(t, sizeof t, "%s_", G.q); UNLOCK(); return put(b, n, t); }
    if (!strcmp(key, "hdr_txt")) {
        LOCK();
        if (atomic_load(&G.busy)) snprintf(t, sizeof t, "WORKING...");
        else snprintf(t, sizeof t, "CC0 + PUBLIC DOMAIN ONLY");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "status_txt")) {
        LOCK();
        if (G.status[0] && mono() - G.status_t < 120.0) snprintf(t, sizeof t, "%s", G.status);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "note_txt")) {
        LOCK();
        if (G.note[0] && mono() - G.note_t < 10.0) snprintf(t, sizeof t, "%s", G.note);
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "r", ROWS))) {
        LOCK();
        int i = G.rpage * ROWS + k - 1;
        if (i < G.nres) {
            char ti[100];
            disp_text(ti, sizeof ti, G.res[i].title, 26);
            snprintf(t, sizeof t, "%s%s   %ds  %s", i == G.rsel ? "> " : "   ", ti, G.res[i].dur, G.res[i].license);
        } else if (k == 1 && G.nres == 0) snprintf(t, sizeof t, "Type a word, then press SEARCH");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "rs_info")) {
        LOCK();
        if (G.rsel >= 0 && G.rsel < G.nres) {
            char cr[60];
            disp_text(cr, sizeof cr, G.res[G.rsel].creator, 30);
            snprintf(t, sizeof t, "%s  -  by %s", !strcmp(G.res[G.rsel].license, "CC0") ? "CC0 (free for any use)" : "Public Domain", cr[0] ? cr : "unknown");
        } else snprintf(t, sizeof t, "Tap a sound to hear it");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "r_page_txt")) {
        LOCK();
        if (G.nres) snprintf(t, sizeof t, "page %d  %d-%d", G.apage, G.rpage * ROWS + 1, (G.rpage + 1) * ROWS < G.nres ? (G.rpage + 1) * ROWS : G.nres);
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "c", ROWS))) {
        LOCK();
        int i = G.cpage * ROWS + k - 1;
        if (i < G.ncr) {
            char nm[100], sz[24];
            disp_text(nm, sizeof nm, G.cr[i].name, 44);
            snprintf(sz, sizeof sz, "%lld KB", (G.cr[i].size + 1023) / 1024);
            snprintf(t, sizeof t, "%s%s   %s", i == G.csel ? "> " : "   ", nm, sz);
        } else if (k == 1 && G.ncr == 0) snprintf(t, sizeof t, "Your crate is empty: KEEP sounds on the SEARCH tab");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "c_page_txt")) {
        LOCK();
        if (G.ncr) snprintf(t, sizeof t, "%d-%d of %d", G.cpage * ROWS + 1, (G.cpage + 1) * ROWS < G.ncr ? (G.cpage + 1) * ROWS : G.ncr, G.ncr);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "pv_txt")) {
        LOCK();
        if (G.pv_valid) { char nm[100]; disp_text(nm, sizeof nm, G.pv_name, 36); snprintf(t, sizeof t, "%s %s%s", G.pv_kept ? "[crate]" : "", nm, G.pv_lic[0] ? "" : ""); }
        else snprintf(t, sizeof t, "No sound played yet");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "to_pad_txt")) { LOCK(); snprintf(t, sizeof t, "TO PAD %d", G.pad_sel + 1); UNLOCK(); return put(b, n, t); }
    if ((k = key_idx(key, "pn", NPADS))) {
        LOCK(); int sel = G.pad_sel; UNLOCK();
        pthread_mutex_lock(&G.padmu);
        char pn[40];
        disp_text(pn, sizeof pn, G.pad[k - 1].d ? G.pad[k - 1].name : "-", 11);
        snprintf(t, sizeof t, "%d%s %s", k, k - 1 == sel ? " >" : "", pn);
        pthread_mutex_unlock(&G.padmu);
        return put(b, n, t);
    }
    if (!strcmp(key, "pad_info_txt")) {
        LOCK(); int p = G.pad_sel; UNLOCK();
        pthread_mutex_lock(&G.padmu);
        if (G.pad[p].d) snprintf(t, sizeof t, "Pad %d: %s  (%.1f s)", p + 1, G.pad[p].name, G.pad[p].frames / 44100.0);
        else snprintf(t, sizeof t, "Pad %d is empty", p + 1);
        pthread_mutex_unlock(&G.padmu);
        return put(b, n, t);
    }
    if (!strcmp(key, "learn_txt")) {
        LOCK();
        int ln = G.learn, ps = G.pad_sel, first = -1, cnt = 0;
        for (int i = 0; i < 128; i++) if (G.padmap[i] == ps) { if (first < 0) first = i; cnt++; }
        UNLOCK();
        if (ln == 1) snprintf(t, sizeof t, "WAITING: hit the pad for PAD %d", ps + 1);
        else if (ln == 2) snprintf(t, sizeof t, "WAITING: hit PAD 1 (bottom left)");
        else if (cnt) snprintf(t, sizeof t, "PAD %d = MIDI note %d%s", ps + 1, first, cnt > 1 ? " +" : "");
        else snprintf(t, sizeof t, "PAD %d has no MIDI note: use LEARN", ps + 1);
        return put(b, n, t);
    }
    if (!strcmp(key, "net_txt")) {
        LOCK();
        if (G.netinfo[0]) snprintf(t, sizeof t, "%s", G.netinfo);
        else snprintf(t, sizeof t, "Press TEST to check the internet connection");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "pitch")) return snprintf(b, (size_t)n, "%.4f", atomic_load(&dk->pitch) / 1000.0);
    if (!strcmp(key, "jog")) return snprintf(b, (size_t)n, "%.4f", atomic_load(&dk->jogv) / 1000.0);
    if (!strcmp(key, "prange")) return snprintf(b, (size_t)n, "%d", atomic_load(&dk->prange));
    if (!strcmp(key, "klock")) return snprintf(b, (size_t)n, "%d", atomic_load(&dk->klock));
    if (!strcmp(key, "plat")) return snprintf(b, (size_t)n, "%.4f", atomic_load(&dk->plat) / (double)(NPLAT - 1));
    if (!strcmp(key, "pitch_txt")) { double sp = (deck_speed(dk) - 1.0) * 100.0; snprintf(t, sizeof t, "%+.1f %%", sp); return put(b, n, t); }
    if (!strcmp(key, "email_txt")) { LOCK(); snprintf(t, sizeof t, "%s_", G.email); UNLOCK(); return put(b, n, t); }
    if (!strcmp(key, "acc_txt")) {
        LOCK();
        if (G.cid[0]) snprintf(t, sizeof t, G.acc_state == 2 ? "ACCOUNT READY" : "WAITING FOR YOUR E-MAIL LINK...");
        else snprintf(t, sizeof t, "No account needed: search works without");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "last_txt")) {
        LOCK();
        if (G.last_note < 0) snprintf(t, sizeof t, "Last MIDI note: none yet (hit a pad)");
        else if (G.last_pad >= 0) snprintf(t, sizeof t, "Last MIDI note: %d = pad %d", G.last_note, G.last_pad + 1);
        else snprintf(t, sizeof t, "Last MIDI note: %d (no pad: use LEARN)", G.last_note);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "att_txt") || !strcmp(key, "dec_txt") || !strcmp(key, "rel_txt") || !strcmp(key, "grp_txt") || !strcmp(key, "mode_txt")) {
        LOCK(); int p = G.pad_sel; UNLOCK();
        pthread_mutex_lock(&G.padmu);
        int a = G.pc[p].att, d = G.pc[p].dec, rl = G.pc[p].rel, g = G.pc[p].grp, md = G.pc[p].mode;
        pthread_mutex_unlock(&G.padmu);
        if (key[0] == 'a') snprintf(t, sizeof t, "%d ms", ATT_MS[a]);
        else if (key[0] == 'd') { if (DEC_MS[d]) snprintf(t, sizeof t, "%d ms", DEC_MS[d]); else snprintf(t, sizeof t, "OFF"); }
        else if (key[0] == 'r') snprintf(t, sizeof t, "%d ms", REL_MS[rl]);
        else if (key[0] == 'g') { if (g) snprintf(t, sizeof t, "GROUP %d", g); else snprintf(t, sizeof t, "NONE"); }
        else snprintf(t, sizeof t, "%s", md ? "HOLD" : "ONE SHOT");
        return put(b, n, t);
    }
    if (!strcmp(key, "dir_txt")) { LOCK(); disp_text(t, sizeof t, G.base, 60); UNLOCK(); return put(b, n, t); }
    if (!strcmp(key, "display_rev")) {
        LOCK();
        unsigned fm = 0;
        for (int i = 0; i < 128; i++) fm = fm * 3u + (unsigned)(G.padmap[i] + 1);
        unsigned h = G.rev * 2654435761u + (unsigned)G.pad_sel * 31u + (unsigned)G.learn * 7u + fm + (unsigned)atomic_load(&G.busy) * 101u + (unsigned)G.cat * 13u + (unsigned)G.shortonly * 17u + (unsigned)G.pv_valid * 19u + (unsigned)G.pv_kept * 23u + (unsigned)atomic_load(&dk->pitch) * 31u + (unsigned)atomic_load(&dk->prange) * 7u + (unsigned)atomic_load(&dk->klock) * 5u + (unsigned)G.pv_play * 3u;
        UNLOCK();
        pthread_mutex_lock(&G.padmu);
        for (int i = 0; i < NPADS; i++) h = h * 31u + (G.pad[i].d ? (unsigned)G.pad[i].frames : 0u) + G.pc[i].att * 3u + G.pc[i].dec * 5u + G.pc[i].rel * 7u + G.pc[i].grp * 11u + G.pc[i].mode * 13u;
        pthread_mutex_unlock(&G.padmu);
        double now = mono();
        LOCK(); h += (G.note[0] && now - G.note_t < 10.0) ? 1u : 0u; h += (G.status[0] && now - G.status_t < 120.0) ? 2u : 0u; UNLOCK();
        return snprintf(b, (size_t)n, "%08x", h);
    }
    return 0;
}

/* the preview and the pads, mixed in 16 bit; a busy lock skips one block rather than waiting on the audio thread.
 * Each pad voice has an envelope: attack ramp, optional decay to silence, and a release after a note-off (hold mode), a mute-group choke or STOP. */
static void render(void *inst, int16_t *out, int frames) {
    inst_t *s = inst;
    deck_t *dk = &s->dk;
    memset(out, 0, (size_t)frames * 4);
    if (pthread_mutex_trylock(&G.padmu) != 0) return;
    int stopreq = atomic_exchange(&s->stop, 0);
    if (stopreq) s->pv_fade = s->pv_fade ? s->pv_fade : 882;
    if (s->seen_gen != G.pv_gen) { s->seen_gen = G.pv_gen; s->pv_posf = 0; s->pv_fade = 0; s->ngr = 0; s->hopc = 0; }
    {
        /* the turntable: play / scratch the loaded sound; the platter picture follows the movement */
        const int16_t *d = G.pv; double total = G.pv_frames, pos = s->pv_posf, speed = deck_speed(dk), p0 = pos;
        int klock = atomic_load(&dk->klock), scr = (now_ms() - atomic_load(&dk->jogt)) < 250;
        atomic_store(&dk->jogstart, 0);
        if (scr && !s->scr) { s->anchor = pos; }
        s->scr = scr;
        int playing = d && G.pv_play && pos < total - 2;
        if (d && (playing || scr)) {
            double target = s->anchor + (atomic_load(&dk->jogv) - atomic_load(&dk->jogref)) / 1000.0 * 88200.0;
            if (target < 0) target = 0; if (target > total - 2) target = total - 2;
            double rate = scr ? (target - pos) / (3.0 * frames) : speed;
            if (rate > 8) rate = 8; if (rate < -8) rate = -8;
            if (klock && !scr) {   /* time stretch: grains read at normal pitch, the grain start moves with the tempo */
                enum { GL = 1024, HS = 256 };
                static float win[GL]; static int win_ok;
                if (!win_ok) { for (int k = 0; k < GL; k++) win[k] = 0.5f - 0.5f * cosf(6.2831853f * (float)k / GL); win_ok = 1; }
                for (int i = 0; i < frames; i++) {
                    if (s->hopc <= 0) { if (s->ngr < 5) { s->gr[s->ngr].src = pos; s->gr[s->ngr].age = 0; s->ngr++; } pos += HS * speed; s->hopc += HS; }
                    s->hopc--;
                    float l = 0, rr = 0;
                    for (int k = 0; k < s->ngr; k++) {
                        long ix = (long)s->gr[k].src + s->gr[k].age;
                        if (ix >= 0 && ix < (long)total) {
                            float w = win[s->gr[k].age];
                            l += w * d[2 * ix]; rr += w * d[2 * ix + 1];
                        }
                        s->gr[k].age++;
                    }
                    for (int k = 0; k < s->ngr; ) { if (s->gr[k].age >= GL) { s->gr[k] = s->gr[s->ngr - 1]; s->ngr--; } else k++; }
                    float g = 0.4f;
                    if (s->pv_fade) { g *= (float)s->pv_fade / 882.0f; if (--s->pv_fade == 0) { pos = total; } }
                    int a = (int)(l * g), c = (int)(rr * g);
                    out[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                    out[2 * i + 1] = (int16_t)(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
                    if (pos >= total - 2) break;
                }
            } else {
                s->ngr = 0; s->hopc = 0;
                for (int i = 0; i < frames; i++) {
                    if (pos < 0 || pos >= total - 1) { if (!scr) pos = total; break; }
                    long ix = (long)pos; float fr = (float)(pos - ix), g = 0.8f;
                    if (s->pv_fade) { g *= (float)s->pv_fade / 882.0f; if (--s->pv_fade == 0) { pos = total; break; } }
                    out[2 * i] = (int16_t)((d[2 * ix] * (1 - fr) + d[2 * ix + 2] * fr) * g);
                    out[2 * i + 1] = (int16_t)((d[2 * ix + 1] * (1 - fr) + d[2 * ix + 3] * fr) * g);
                    pos += rate;
                }
            }
            s->pv_posf = pos;
            /* the platter: 33 1/3 turns a minute at normal speed, follows the scratching, forwards and backwards */
            double moved = klock && !scr ? frames * speed : pos - p0;
            s->ang += moved / 44100.0 * (100.0 / 3.0 / 60.0);
        } else if (G.pv_play && d == NULL) { /* nothing loaded */ }
        double fa = s->ang - floor(s->ang);
        atomic_store(&dk->plat, (int)(fa * NPLAT) % NPLAT);
    }
    for (int p = 0; p < NPADS; p++) {
        int vel = atomic_exchange(&s->trig[p], 0);
        if (vel && G.pad[p].d) {
            int g = G.pc[p].grp;
            if (g) for (int q = 0; q < NPADS; q++) if (q != p && G.pc[q].grp == g && s->voice[q].on && s->voice[q].rage < 0) s->voice[q].rage = 0;   /* mute group: the others let go */
            s->voice[p].on = 1; s->voice[p].pos = 0; s->voice[p].age = 0; s->voice[p].rage = -1; s->voice[p].gain = (float)vel / 127.0f;
        }
        if (atomic_exchange(&s->rel[p], 0) && G.pc[p].mode == 1 && s->voice[p].on && s->voice[p].rage < 0) s->voice[p].rage = 0;
        if (stopreq && s->voice[p].on && s->voice[p].rage < 0) s->voice[p].rage = 0;
        if (!s->voice[p].on || !G.pad[p].d) continue;
        const int16_t *d = G.pad[p].d;
        long pos = s->voice[p].pos, total = G.pad[p].frames, age = s->voice[p].age, rage = s->voice[p].rage;
        long att = (long)ATT_MS[G.pc[p].att] * 44100 / 1000, dec = (long)DEC_MS[G.pc[p].dec] * 44100 / 1000, rel = (long)REL_MS[G.pc[p].rel] * 44100 / 1000;
        if (rel < 1) rel = 1;
        float gain = s->voice[p].gain * 0.9f;
        for (int i = 0; i < frames; i++, pos++, age++) {
            if (pos >= total) { s->voice[p].on = 0; break; }
            float e = 1.0f;
            if (att > 0 && age < att) e = (float)age / (float)att;
            if (dec > 0) { long da = age - att; if (da < 0) da = 0; if (da >= dec) { s->voice[p].on = 0; break; } e *= 1.0f - (float)da / (float)dec; }
            if (rage >= 0) { if (rage >= rel) { s->voice[p].on = 0; break; } e *= 1.0f - (float)rage / (float)rel; rage++; }
            float g2 = gain * e;
            int a = out[2 * i] + (int)(d[2 * pos] * g2), c = out[2 * i + 1] + (int)(d[2 * pos + 1] * g2);
            out[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
            out[2 * i + 1] = (int16_t)(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
        }
        s->voice[p].pos = pos; s->voice[p].age = age; s->voice[p].rage = rage;
    }
    pthread_mutex_unlock(&G.padmu);
}

/* the wrapper asks once per audio block which of our values moved by themselves and tells the host (about 40 times a second), so the platter picture turns smoothly */
int mpc_engine_changed(void *inst, char *key, int len);
int mpc_engine_changed(void *inst, char *key, int len) {
    inst_t *s = inst;
    if (!s) return 0;
    int f = atomic_load(&s->dk.plat);
    if (f == s->plat_rep) return 0;
    s->plat_rep = f;
    snprintf(key, (size_t)len, "plat");
    return 1;
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
