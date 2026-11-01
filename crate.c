/* CRATE DIGGA 0.1 (JJ DA PLUG): an MPC OS VST2 plugin (instrument) that digs for free sounds on the internet.
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
    res_t res[NRES]; int nres, rpage, rsel, apage, hidden;
    char status[120]; double status_t;
    atomic_int busy;
    int16_t *pv; int pv_frames; int pv_play; unsigned pv_gen; int pv_valid, pv_kept;
    char pv_name[80], pv_lic[8], pv_creator[48], pv_url[400];
    cfile_t cr[200]; int ncr, cpage, csel;
    struct { int16_t *d; int frames; char name[40]; } pad[NPADS];
    int pad_sel; signed char padmap[128]; int learn;
    char note[100]; double note_t;
    int curl_state; char netinfo[160]; unsigned rev;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .padmu = PTHREAD_MUTEX_INITIALIZER, .netmu = PTHREAD_MUTEX_INITIALIZER, .rsel = -1, .csel = -1, .apage = 1, .shortonly = 1 };

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
                *(void **)&CURL.init = dlsym(CURL.h, "curl_easy_init");
                *(void **)&CURL.setopt = dlsym(CURL.h, "curl_easy_setopt");
                *(void **)&CURL.perform = dlsym(CURL.h, "curl_easy_perform");
                *(void **)&CURL.cleanup = dlsym(CURL.h, "curl_easy_cleanup");
                *(void **)&CURL.getinfo = dlsym(CURL.h, "curl_easy_getinfo");
                *(void **)&CURL.strerr = dlsym(CURL.h, "curl_easy_strerror");
                if (CURL.init && CURL.setopt && CURL.perform && CURL.cleanup && CURL.getinfo && CURL.strerr) G.curl_state = 1;
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

static int http_curl(const char *url, buf_t *b, long timeout, char *err, size_t ecap, long *code) {
    void *h = CURL.init();
    if (!h) { snprintf(err, ecap, "curl init failed"); return -1; }
    const char *ca = getenv("CRATE_CAINFO");
    CURL.setopt(h, 10002, url);                       /* URL */
    CURL.setopt(h, 20011, wcb);                       /* WRITEFUNCTION */
    CURL.setopt(h, 10001, b);                         /* WRITEDATA */
    CURL.setopt(h, 52, 1L);                           /* FOLLOWLOCATION */
    CURL.setopt(h, 68, 5L);                           /* MAXREDIRS */
    CURL.setopt(h, 13, timeout);                      /* TIMEOUT */
    CURL.setopt(h, 78, 12L);                          /* CONNECTTIMEOUT */
    CURL.setopt(h, 10018, "CrateDigga/0.1 (JJ DA PLUG; MPC)");   /* USERAGENT */
    CURL.setopt(h, 99, 1L);                           /* NOSIGNAL */
    CURL.setopt(h, 10102, "");                        /* ACCEPT_ENCODING: whatever curl supports */
    if (ca && *ca) CURL.setopt(h, 10065, ca);         /* CAINFO (tests only) */
    int rc = CURL.perform(h);
    long c = 0;
    CURL.getinfo(h, 0x200002, &c);                    /* RESPONSE_CODE */
    CURL.cleanup(h);
    *code = c;
    if (rc != 0) { snprintf(err, ecap, "%s", b->over ? "File too big" : CURL.strerr(rc)); return -1; }
    return 0;
}

static int http_spawn(const char *url, buf_t *b, long timeout, char *err, size_t ecap, long *code) {
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
    char *argv_c[] = { (char *)"curl", (char *)"-sSLf", (char *)"--max-time", tm, (char *)"--max-filesize", mx, (char *)"-A", (char *)"CrateDigga/0.1", (char *)"-o", tmp, (char *)url, NULL };
    char *argv_w[] = { (char *)"wget", (char *)"-q", (char *)"-T", tm, (char *)"-O", tmp, (char *)url, NULL };
    char *envp[] = { (char *)"PATH=/usr/bin:/bin:/usr/local/bin", NULL };   /* a clean environment: no LD_PRELOAD from the MPC */
    pid_t pid;
    if (posix_spawn(&pid, bin, NULL, NULL, is_curl ? argv_c : argv_w, envp)) { snprintf(err, ecap, "Cannot start %s", bin); return -1; }
    int st = 0;
    waitpid(pid, &st, 0);
    FILE *f = fopen(tmp, "rb");
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && f;
    if (ok) {
        uint8_t chunk[16384];
        size_t r;
        while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) if (wcb(chunk, 1, r, b) != r) { ok = 0; break; }
    }
    if (f) fclose(f);
    unlink(tmp);
    *code = ok ? 200 : 0;
    if (!ok) snprintf(err, ecap, b->over ? "File too big" : "Download failed");
    return ok ? 0 : -1;
}

/* GET url into a new buffer; only http:// and https:// */
static int http_get(const char *url, buf_t *b, size_t max, long timeout, char *err, size_t ecap, long *code) {
    memset(b, 0, sizeof *b);
    b->max = max;
    *code = 0;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) { snprintf(err, ecap, "Blocked address"); return -1; }
    int rc = curl_load() ? http_curl(url, b, timeout, err, ecap, code) : http_spawn(url, b, timeout, err, ecap, code);
    if (rc == 0 && *code >= 400) { snprintf(err, ecap, "Server answered %ld", *code); rc = -1; }
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

typedef struct { int apage; } sjob_t;

static void *job_search(void *arg) {
    int apage = ((sjob_t *)arg)->apage;
    free(arg);
    char q[40], qe[120], url[600], err[100] = "";
    int cat, shortonly;
    LOCK(); snprintf(q, sizeof q, "%s", G.q); cat = G.cat; shortonly = G.shortonly; UNLOCK();
    const char *api = getenv("CRATE_API");
    urlenc(q, qe, sizeof qe);
    snprintf(url, sizeof url, "%s?q=%s&license=cc0,pdm&page_size=%d&page=%d&mature=false%s", api && *api ? api : "https://api.openverse.org/v1/audio/",
             qe, NRES, apage, cat == 1 ? "&category=sound_effect" : cat == 2 ? "&category=music" : "");
    status_set("Searching...");
    buf_t b;
    long code;
    if (http_get(url, &b, 4 << 20, 25, err, sizeof err, &code)) {
        char m[160];
        snprintf(m, sizeof m, "Search failed: %.100s", err);
        status_set(m);
        atomic_store(&G.busy, 0);
        return NULL;
    }
    res_t *tmp = calloc(NRES, sizeof *tmp);
    int n = 0, hidden = 0;
    const char *arr = obj_get((const char *)b.d, "results");
    if (tmp && arr && *(arr = skip_ws(arr)) == '[') {
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
    LOCK();
    if (tmp) { memcpy(G.res, tmp, sizeof(res_t) * NRES); }
    G.nres = n; G.rpage = 0; G.rsel = -1; G.apage = apage; G.hidden = hidden; G.rev++;
    UNLOCK();
    free(tmp);
    char m[120];
    if (n) snprintf(m, sizeof m, "%d sounds (page %d)%s", n, apage, hidden ? ", some hidden" : "");
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
    char err[100] = "", m[160];
    buf_t b;
    long code;
    const char *api = getenv("CRATE_API");
    status_set("Testing the connection...");
    int has = curl_load();
    int rc = http_get(api && *api ? api : "https://api.openverse.org/v1/audio/?q=test&license=cc0&page_size=1", &b, 1 << 20, 25, err, sizeof err, &code);
    LOCK();
    snprintf(G.netinfo, sizeof G.netinfo, "%s: %s", has ? "libcurl" : "curl/wget program", rc ? err : "connection OK");
    UNLOCK();
    if (rc) snprintf(m, sizeof m, "Test failed: %.100s", err); else { snprintf(m, sizeof m, "Connection OK (%zu bytes)", b.n); free(b.d); }
    status_set(m);
    atomic_store(&G.busy, 0);
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

static void cfg_save(void) {
    char dir[300], path[330], tmp[340];
    signed char map[128];
    LOCK(); snprintf(dir, sizeof dir, "%s/pads", G.base); memcpy(map, G.padmap, sizeof map); int have = G.base[0]; UNLOCK();
    if (!have) return;
    mkdir(dir, 0755);
    char names[NPADS][40];
    pthread_mutex_lock(&G.padmu);
    for (int i = 0; i < NPADS; i++) snprintf(names[i], sizeof names[i], "%s", G.pad[i].d ? G.pad[i].name : "");
    pthread_mutex_unlock(&G.padmu);
    snprintf(path, sizeof path, "%s/pads.cfg", dir);
    snprintf(tmp, sizeof tmp, "%s/pads.tmp", dir);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < NPADS; i++) if (names[i][0]) fprintf(f, "name %d %s\n", i, names[i]);
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
            if (sscanf(line, "name %d %59[^\n]", &i, nm) == 2 && i >= 0 && i < NPADS) snprintf(names[i], sizeof names[i], "%s", nm);
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
    struct { int on; long pos; float gain; } voice[NPADS];
    unsigned seen_gen; long pv_pos;
    double born;
} inst_t;

static void *create(const char *dir) {
    (void)dir;
    inst_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->born = mono();
    LOCK();
    if (!G.base[0]) make_dirs();
    G.refs++;
    int boot = !G.boot;
    if (boot) { G.boot = 1; map_default(); }
    UNLOCK();
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
        UNLOCK();
        if (learned) { note_set(msg); cfg_save_async(); }
        pad_trigger(s, p, m[2]);
    }
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
    int v = atoi(val);
    int fire = atof(val) > 0.5;
    if (!strcmp(key, "cat")) { LOCK(); G.cat = v < 0 ? 0 : v > 2 ? 2 : v; G.rev++; UNLOCK(); return; }
    if (!strcmp(key, "shortonly")) { LOCK(); G.shortonly = v ? 1 : 0; G.rev++; UNLOCK(); return; }
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
                                         "pad_clear", "learn_pad", "learn_base", "net_test" };
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++) if (!strcmp(key, fixed[i])) return 1;
    return (key[0] == 'k' && key[1] == 'c' && isdigit((unsigned char)key[2])) || key_idx(key, "pq", NQUICK) || key_idx(key, "rt", ROWS) ||
           key_idx(key, "ct", ROWS) || key_idx(key, "pd", NPADS);
}

static int get_param(void *inst, const char *key, char *b, int n) {
    (void)inst;
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
            disp_text(ti, sizeof ti, G.res[i].title, 40);
            snprintf(t, sizeof t, "%s%s   %ds  %s", i == G.rsel ? "> " : "   ", ti, G.res[i].dur, G.res[i].license);
        } else if (k == 1 && G.nres == 0) snprintf(t, sizeof t, "Type a word on the SEARCH tab or tap a preset");
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
        } else if (k == 1 && G.ncr == 0) snprintf(t, sizeof t, "Your crate is empty: KEEP sounds on the RESULTS tab");
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
    if (!strcmp(key, "dir_txt")) { LOCK(); disp_text(t, sizeof t, G.base, 60); UNLOCK(); return put(b, n, t); }
    if (!strcmp(key, "display_rev")) {
        LOCK();
        unsigned fm = 0;
        for (int i = 0; i < 128; i++) fm = fm * 3u + (unsigned)(G.padmap[i] + 1);
        unsigned h = G.rev * 2654435761u + (unsigned)G.pad_sel * 31u + (unsigned)G.learn * 7u + fm + (unsigned)atomic_load(&G.busy) * 101u + (unsigned)G.cat * 13u + (unsigned)G.shortonly * 17u + (unsigned)G.pv_valid * 19u + (unsigned)G.pv_kept * 23u;
        UNLOCK();
        pthread_mutex_lock(&G.padmu);
        for (int i = 0; i < NPADS; i++) h = h * 31u + (G.pad[i].d ? (unsigned)G.pad[i].frames : 0u);
        pthread_mutex_unlock(&G.padmu);
        double now = mono();
        LOCK(); h += (G.note[0] && now - G.note_t < 10.0) ? 1u : 0u; h += (G.status[0] && now - G.status_t < 120.0) ? 2u : 0u; UNLOCK();
        return snprintf(b, (size_t)n, "%08x", h);
    }
    return 0;
}

/* the preview and the pads, mixed in 16 bit; a busy lock skips one block rather than waiting on the audio thread */
static void render(void *inst, int16_t *out, int frames) {
    inst_t *s = inst;
    memset(out, 0, (size_t)frames * 4);
    if (pthread_mutex_trylock(&G.padmu) != 0) return;
    if (s->seen_gen != G.pv_gen) { s->seen_gen = G.pv_gen; s->pv_pos = 0; }
    if (G.pv && G.pv_play && s->pv_pos < G.pv_frames) {
        long pos = s->pv_pos;
        for (int i = 0; i < frames && pos < G.pv_frames; i++, pos++) {
            out[2 * i] = (int16_t)(G.pv[2 * pos] * 0.8f);
            out[2 * i + 1] = (int16_t)(G.pv[2 * pos + 1] * 0.8f);
        }
        s->pv_pos = pos;
    }
    for (int p = 0; p < NPADS; p++) {
        int vel = atomic_exchange(&s->trig[p], 0);
        if (vel && G.pad[p].d) { s->voice[p].on = 1; s->voice[p].pos = 0; s->voice[p].gain = (float)vel / 127.0f; }
        if (!s->voice[p].on || !G.pad[p].d) continue;
        const int16_t *d = G.pad[p].d;
        long pos = s->voice[p].pos, total = G.pad[p].frames;
        float g = s->voice[p].gain * 0.9f;
        for (int i = 0; i < frames && pos < total; i++, pos++) {
            int a = out[2 * i] + (int)(d[2 * pos] * g), c = out[2 * i + 1] + (int)(d[2 * pos + 1] * g);
            out[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
            out[2 * i + 1] = (int16_t)(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
        }
        s->voice[p].pos = pos;
        if (pos >= total) s->voice[p].on = 0;
    }
    pthread_mutex_unlock(&G.padmu);
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
