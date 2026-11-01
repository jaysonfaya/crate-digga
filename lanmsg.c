/* MPC Messenger 1.4 (JJ DA PLUG): an MPC OS VST2 plugin (instrument) with a chat room on the MPC's Wi-Fi/LAN address.
 * Made by JJ-LEMMING.
 *
 * HOST: switch SERVER on. The MPC shows its address, a 6-digit join code and a QR code. A phone or laptop scans the QR
 * code (or opens the address and types the code) and chats: messages and files go both ways.
 * JOIN: a second MPC finds hosts on the network, you pick one, tap the code on the on-screen keypad, and it chats and
 * exchanges files with the host in the same way (the host's room is the room).
 * Files from others land in <Documents>/Messenger/inbox. Files in <Documents>/Messenger/outbox can be sent from the MPC.
 * Short signal tones (online, sent, received) play on the plugin's audio output and in the browser.
 *
 * Safety: the server is OFF until switched on and never starts by itself when a project loads. Joining needs the code
 * (5 wrong tries lock joining for 60 s). Every API call needs the session cookie and an X-LanMsg header. Uploads go only
 * to inbox/ under a sanitised name (never overwriting); downloads serve only plain regular files listed in outbox/.
 * Plain HTTP: use it on a network you trust. All network and disk work stays on worker threads; render() only
 * synthesises the tones. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "page.h"
#include "qr.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

/* the engine contract of wrapper/engine.h, declared here so the file builds on its own */
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

#define PORT_DEFAULT 8765
#define BEACON_PORT 8766
#define MAX_MSGS 100
#define MAX_TOKENS 8
#define MAX_FILES 64
#define MAX_CONN 6
#define MAX_UPLOAD (200LL << 20)
#define HDR_MAX 8192
#define NQUICK 8
#define ROWS 7          /* rows shown in the chat and in every file list */
#define NPADS 16
#define TW 32
#define TH 20
#define NTILE (TW * TH)   /* the picture preview: 32 x 20 tiles */
#define TILE_FRAMES 216   /* 6 x 6 x 6 colours */
#define OLD_NTILE 240      /* the phone's own tiny preview: 20 x 12 letters */
#define MAX_BENT 256
#define PAD_MAX_FRAMES (44100 * 180)
#define NPEERS 4
#define ONLINE_SECS 8
#define NMETER 100      /* the QR code is shown by 10 x 10 meters of 3 x 3 modules */
#define METER_FRAMES 512

enum { V_CHAT, V_INBOX, V_OUTBOX, V_HOST, V_JOIN, V_INFO };

static const char *QUICK[NQUICK] = { "OK", "Ready", "On my way", "Thanks", "Check inbox", "Send file", "Call me", "Later" };

/* from: 0 someone else, 1 me, 2 notice. box: 0 no file, 'I' file is in my inbox, 'O' file is in my outbox, 'P' still arriving, 'F' failed */
typedef struct { unsigned id; int from; time_t ts; char who[20]; char text[257]; char file[96]; char lfile[96]; char box; } msg_t;
typedef struct { char name[96]; long long size; time_t mt; } fent_t;
typedef struct { char name[96]; int dir; long long size; } bent_t;
typedef struct { char ip[48]; int port; char name[20]; time_t seen; } peer_t;

static struct {
    pthread_mutex_t mu;
    int refs, on, listen_fd, th_run, port, beacon_fd;
    pthread_t th;
    char err[48], pin[8], devname[20], ip[48];
    time_t ip_t, beacon_t;
    struct { char tok[33]; char name[20]; time_t seen; } tk[MAX_TOKENS];
    int ntok, fails;
    atomic_int snd_req;
    time_t fail_t0, locked_until;
    msg_t msgs[MAX_MSGS];
    int nmsg;
    unsigned next_id;
    fent_t files[MAX_FILES], infiles[MAX_FILES];
    int nfiles, ninfiles, newin;
    int in_page, in_sel, out_page, out_sel;   /* in_sel / out_sel: index in infiles / files, -1 none */
    time_t scan_t, in_seen;
    char base[200], inbox[256], outbox[256];
    int active_fd[MAX_CONN];
    atomic_int conns;
    /* join (client) side */
    struct {
        int run, alive, state, fd, port, req_n;          /* state: 0 idle, 1 connecting, 2 joined, 3 failed */
        pthread_t th;
        char ip[48], pin[8], cookie[40], name[20], host[20], err[64], online[160];
        unsigned last;
        char req_text[4][64], req_file[96];
    } cl;
    char jcode[8];
    peer_t peers[NPEERS];
    int npeers, peer_sel, scan_run, scan_alive;
    pthread_t scan_th;
    unsigned char thumb[NTILE]; char thumb_who[20], thumb_file[96];
    unsigned epoch;
    unsigned thumb_rev;
    pthread_mutex_t padmu;
    struct { int16_t *d; int frames; char name[40]; } pad[NPADS];
    int pad_sel, pad_owner_set;
    signed char padmap[128];      /* MIDI note -> pad 0..15, -1 none */
    int learn, pads_boot;         /* learn: 0 off, 1 next note goes to the selected pad, 2 next note is pad 1 (the rest follow) */
    void *pad_owner;
    atomic_int trig[NPADS];
    char bpath[400];
    struct { unsigned char att, dec, rel, grp, mode, start, end, pitch, tempo, stretch; } pc[NPADS];   /* per pad: the sound shaping (steps, see PC_*) */
    int last_note, last_pad;
    unsigned rev_pad;
    bent_t bent[MAX_BENT];
    int nbent, bpage, bsel;
    char note[100];
    double note_t;
    uint8_t qr[QR_N][QR_N];
    char qr_text[80];
    int qr_ok;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .padmu = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1, .beacon_fd = -1, .in_sel = -1, .out_sel = -1, .peer_sel = -1, .last_note = -1, .last_pad = -1 };

#define LOCK() pthread_mutex_lock(&G.mu)
#define UNLOCK() pthread_mutex_unlock(&G.mu)

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }

/* ---------- helpers ---------- */

static int rand_bytes(void *b, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(b, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

/* a file name is safe to list, serve and create when it is plain ASCII with no path part */
static int name_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 80 || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ' || c == '(' || c == ')')) return 0;
    }
    return 1;
}

/* turn what the sender called a file into a name_ok() name */
static void clean_name(const char *in, char *out, size_t cap) {
    const char *b = in;
    for (const char *p = in; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    size_t n = 0;
    while (*b == '.' || *b == ' ') b++;
    for (; *b && n < 80 && n + 1 < cap; b++) {
        unsigned char c = (unsigned char)*b;
        out[n++] = (isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ' || c == '(' || c == ')') ? (char)c : '_';
    }
    out[n] = 0;
    if (!n) snprintf(out, cap, "file");
}

static void one_line(char *s) { for (; *s; s++) if ((unsigned char)*s < 32 || *s == 127) *s = ' '; }

/* text for the MPC's screen: ASCII and Latin letters pass, anything else becomes '?'; cut to max_chars characters */
static void disp_text(char *dst, size_t cap, const char *src, int max_chars) {
    size_t o = 0;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 3 < cap; chars++) {
        if (chars >= max_chars) { if (o + 4 < cap) { dst[o++] = '.'; dst[o++] = '.'; } break; }
        if (*p < 0x80) { dst[o++] = (*p < 32 || *p == 127) ? ' ' : (char)*p; p++; }
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80 && (((p[0] & 0x1F) << 6) | (p[1] & 0x3F)) <= 0x17F) { dst[o++] = (char)p[0]; dst[o++] = (char)p[1]; p += 2; }
        else {
            dst[o++] = '?';
            p++;
            while ((*p & 0xC0) == 0x80) p++;
        }
    }
    dst[o] = 0;
}

static void fmt_size(long long b, char *o, size_t n) {
    if (b < 1024) snprintf(o, n, "%lld B", b);
    else if (b < (1 << 20)) snprintf(o, n, "%.1f KB", b / 1024.0);
    else snprintf(o, n, "%.1f MB", b / 1048576.0);
}

static void add_msg_full(int from, const char *who, const char *text, const char *file, const char *lfile, char box) {
    LOCK();
    if (G.nmsg == MAX_MSGS) { memmove(G.msgs, G.msgs + 1, sizeof(msg_t) * (MAX_MSGS - 1)); G.nmsg--; }
    msg_t *m = &G.msgs[G.nmsg++];
    memset(m, 0, sizeof *m);
    m->id = ++G.next_id;
    m->from = from;
    m->ts = time(NULL);
    snprintf(m->who, sizeof m->who, "%s", who ? who : "");
    snprintf(m->text, sizeof m->text, "%s", text ? text : "");
    snprintf(m->file, sizeof m->file, "%s", file ? file : "");
    snprintf(m->lfile, sizeof m->lfile, "%s", lfile ? lfile : (file ? file : ""));
    m->box = file && file[0] ? box : 0;
    one_line(m->text);
    UNLOCK();
}
static void add_msg(int from, const char *who, const char *text, const char *file, char box) { add_msg_full(from, who, text, file, NULL, box); }

static void new_pin(void) {
    uint32_t r = 0;
    LOCK();
    if (rand_bytes(&r, sizeof r)) snprintf(G.pin, sizeof G.pin, "------");   /* no randomness: nobody can pair */
    else snprintf(G.pin, sizeof G.pin, "%06u", r % 1000000u);
    UNLOCK();
}

static void note_set(const char *t) { LOCK(); snprintf(G.note, sizeof G.note, "%s", t); G.note_t = mono(); UNLOCK(); }

static void make_dirs(void) {
    const char *env = getenv("LANMSG_DIR");
    struct stat sb;
    if (env && *env) snprintf(G.base, sizeof G.base, "%s", env);
    else if (!stat("/sdcard/MPC Documents", &sb)) snprintf(G.base, sizeof G.base, "/sdcard/MPC Documents/Messenger");
    else if (!stat("/sdcard/Force Documents", &sb)) snprintf(G.base, sizeof G.base, "/sdcard/Force Documents/Messenger");
    else snprintf(G.base, sizeof G.base, "/sdcard/Messenger");
    snprintf(G.inbox, sizeof G.inbox, "%s/inbox", G.base);
    snprintf(G.outbox, sizeof G.outbox, "%s/outbox", G.base);
    mkdir(G.base, 0755);
    mkdir(G.inbox, 0755);
    mkdir(G.outbox, 0755);
}

static int cmp_name(const void *a, const void *b) { return strcasecmp(((const fent_t *)a)->name, ((const fent_t *)b)->name); }
static int cmp_new(const void *a, const void *b) {
    const fent_t *x = a, *y = b;
    if (x->mt != y->mt) return x->mt < y->mt ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

/* rescan outbox and inbox; cached for 2 s unless forced. The inbox is listed newest first. */
static void scan_files(int force) {
    time_t now = time(NULL);
    LOCK();
    if (!G.base[0]) { UNLOCK(); return; }
    if (!force && G.scan_t && now - G.scan_t < 2) { UNLOCK(); return; }
    G.scan_t = now;
    char ob[256], ib[256];
    snprintf(ob, sizeof ob, "%s", G.outbox);
    snprintf(ib, sizeof ib, "%s", G.inbox);
    time_t seen = G.in_seen;
    UNLOCK();

    static __thread fent_t tmp[MAX_FILES], tin[MAX_FILES];
    int n = 0, ni = 0, newc = 0;
    DIR *d = opendir(ob);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < MAX_FILES) {
            if (!name_ok(e->d_name)) continue;
            char p[400];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", ob, e->d_name);
            if (lstat(p, &st) || !S_ISREG(st.st_mode)) continue;
            snprintf(tmp[n].name, sizeof tmp[n].name, "%s", e->d_name);
            tmp[n].size = st.st_size;
            tmp[n].mt = st.st_mtime;
            n++;
        }
        closedir(d);
    }
    qsort(tmp, (size_t)n, sizeof tmp[0], cmp_name);
    d = opendir(ib);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && ni < MAX_FILES) {
            if (e->d_name[0] == '.') continue;
            char p[400];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", ib, e->d_name);
            if (lstat(p, &st) || !S_ISREG(st.st_mode)) continue;
            snprintf(tin[ni].name, sizeof tin[ni].name, "%s", e->d_name);
            tin[ni].size = st.st_size;
            tin[ni].mt = st.st_mtime;
            if (st.st_mtime >= seen && seen) newc++;
            ni++;
        }
        closedir(d);
    }
    qsort(tin, (size_t)ni, sizeof tin[0], cmp_new);
    LOCK();
    memcpy(G.files, tmp, sizeof(fent_t) * (size_t)n);
    G.nfiles = n;
    memcpy(G.infiles, tin, sizeof(fent_t) * (size_t)ni);
    G.ninfiles = ni;
    G.newin = newc;
    if (G.out_sel >= n) G.out_sel = n ? n - 1 : -1;
    if (G.out_sel < 0 && n) G.out_sel = 0;
    if (G.in_sel >= ni) G.in_sel = -1;
    UNLOCK();
}

static void local_ip_raw(char *out, size_t n) {
    struct ifaddrs *ifa, *p;
    char best[48] = "", any[48] = "";
    if (!getifaddrs(&ifa)) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP)) continue;
            char ip[48];
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, sizeof ip);
            if (!any[0]) snprintf(any, sizeof any, "%s", ip);
            if (!strncmp(p->ifa_name, "wl", 2) && !best[0]) snprintf(best, sizeof best, "%s", ip);
        }
        freeifaddrs(ifa);
    }
    snprintf(out, n, "%s", best[0] ? best : (any[0] ? any : "no network"));
}

/* the cached local address (refreshed every 3 s) and the device name "MPC-<last number of the address>" */
static void ip_refresh(void) {
    time_t now = time(NULL);
    LOCK();
    if (G.ip[0] && now - G.ip_t < 3) { UNLOCK(); return; }
    UNLOCK();
    char ip[48];
    local_ip_raw(ip, sizeof ip);
    LOCK();
    G.ip_t = now;
    snprintf(G.ip, sizeof G.ip, "%s", ip);
    const char *dot = strrchr(ip, '.');
    const char *ov = getenv("LANMSG_NAME");
    if (ov && *ov) snprintf(G.devname, sizeof G.devname, "%.15s", ov);
    else if (dot) snprintf(G.devname, sizeof G.devname, "MPC-%.8s", dot + 1);
    else snprintf(G.devname, sizeof G.devname, "MPC");
    UNLOCK();
}

/* ---------- HTTP server ---------- */

static int send_all(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void respond(int fd, int code, const char *ctype, const char *extra, const char *body, size_t len) {
    static const struct { int c; const char *t; } T[] = { {200, "OK"}, {400, "Bad Request"}, {401, "Unauthorized"},
        {403, "Forbidden"}, {404, "Not Found"}, {413, "Payload Too Large"}, {429, "Too Many Requests"}, {500, "Server Error"} };
    const char *t = "OK";
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].c == code) t = T[i].t;
    char h[600];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'\r\n%s\r\n",
        code, t, ctype, len, extra ? extra : "");
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static void respond_str(int fd, int code, const char *s) { respond(fd, code, "text/plain; charset=utf-8", NULL, s, strlen(s)); }

/* case-insensitive header lookup in the header block */
static int get_header(const char *hdrs, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *p = strchr(hdrs, '\n'); p; p = strchr(p + 1, '\n')) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = 0;
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < cap) { out[n] = p[n]; n++; }
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

static int hexv(int c) { return isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }

static void url_decode(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        if (*in == '%' && hexv(in[1]) >= 0 && hexv(in[2]) >= 0) { out[n++] = (char)(hexv(in[1]) * 16 + hexv(in[2])); in += 2; }
        else out[n++] = *in;
    }
    out[n] = 0;
}

static void url_encode(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') out[n++] = (char)c;
        else n += (size_t)snprintf(out + n, 4, "%%%02X", c);
    }
    out[n] = 0;
}

static int query_get(const char *q, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1;
            size_t n = strcspn(v, "&");
            char raw[400];
            snprintf(raw, sizeof raw, "%.*s", (int)(n < sizeof raw - 1 ? n : sizeof raw - 1), v);
            url_decode(raw, out, cap);
            return 1;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return 0;
}

/* the session cookie must be a known token; refreshes its "seen" time and returns the guest's name */
static int auth_who(const char *hdrs, char *who, size_t cap) {
    char ck[300];
    if (!get_header(hdrs, "Cookie", ck, sizeof ck)) return 0;
    const char *p = strstr(ck, "mt=");
    if (!p) return 0;
    char tok[33];
    snprintf(tok, sizeof tok, "%.32s", p + 3);
    if (strlen(tok) != 32) return 0;
    int ok = 0;
    LOCK();
    for (int i = 0; i < G.ntok; i++) if (!strcmp(G.tk[i].tok, tok)) { ok = 1; G.tk[i].seen = time(NULL); if (who) snprintf(who, cap, "%s", G.tk[i].name); }
    UNLOCK();
    return ok;
}

static int pin_equal(const char *a, const char *b) {   /* constant time */
    if (strlen(a) != 6 || strlen(b) != 6) return 0;
    int d = 0;
    for (int i = 0; i < 6; i++) d |= a[i] ^ b[i];
    return d == 0;
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void do_pair(int fd, const char *body) {
    char pin[16] = "", raw[64] = "", name[20];
    query_get(body, "pin", pin, sizeof pin);
    query_get(body, "name", raw, sizeof raw);
    size_t n = 0;
    for (const char *c = raw; *c && n < 16; c++) {
        unsigned char u = (unsigned char)*c;
        if (isalnum(u) || u == ' ' || u == '_' || u == '-') name[n++] = (char)u;
    }
    name[n] = 0;
    if (!name[0]) snprintf(name, sizeof name, "Guest");
    time_t now = time(NULL);
    int code = 401;
    char cookie[120] = "";
    LOCK();
    if (now < G.locked_until) code = 429;
    else if (G.pin[0] != '-' && pin_equal(pin, G.pin)) {
        char rb[16], tok[33];
        if (rand_bytes(rb, sizeof rb) == 0) {
            for (int i = 0; i < 16; i++) snprintf(tok + i * 2, 3, "%02x", (unsigned char)rb[i]);
            if (G.ntok == MAX_TOKENS) { memmove(&G.tk[0], &G.tk[1], sizeof G.tk[0] * (MAX_TOKENS - 1)); G.ntok--; }
            snprintf(G.tk[G.ntok].tok, 33, "%s", tok);
            snprintf(G.tk[G.ntok].name, sizeof G.tk[G.ntok].name, "%s", name);
            G.tk[G.ntok].seen = now;
            G.ntok++;
            snprintf(cookie, sizeof cookie, "Set-Cookie: mt=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000\r\n", tok);
            G.fails = 0;
            code = 200;
        } else code = 500;
    } else {
        if (!G.fails || now - G.fail_t0 > 60) { G.fails = 0; G.fail_t0 = now; }
        if (++G.fails >= 5) { G.locked_until = now + 60; G.fails = 0; }
    }
    UNLOCK();
    if (code == 200) {
        char t[64];
        snprintf(t, sizeof t, "%s is online", name);
        add_msg(2, "", t, "", 0);
        atomic_store(&G.snd_req, 1);
        respond(fd, 200, "text/plain", cookie, "ok", 2);
    } else respond_str(fd, code, code == 429 ? "locked" : "no");
}

static void do_state(int fd, const char *query) {
    char sv[24] = "0";
    query_get(query, "since", sv, sizeof sv);
    unsigned since = (unsigned)strtoul(sv, NULL, 10);
    char *buf = NULL;
    size_t blen = 0;
    FILE *f = open_memstream(&buf, &blen);
    if (!f) { respond_str(fd, 500, "err"); return; }
    time_t now = time(NULL);
    LOCK();
    fprintf(f, "{\"ep\":%u,\"host\":", G.epoch);
    json_str(f, G.devname);
    fputs(",\"online\":[", f);
    int first = 1;
    for (int i = 0; i < G.ntok; i++) {
        if (now - G.tk[i].seen > ONLINE_SECS) continue;
        if (!first) fputc(',', f);
        first = 0;
        json_str(f, G.tk[i].name);
    }
    fputs("],\"msgs\":[", f);
    first = 1;
    for (int i = 0; i < G.nmsg; i++) {
        if (G.msgs[i].id <= since) continue;
        if (!first) fputc(',', f);
        first = 0;
        fprintf(f, "{\"id\":%u,\"from\":%d,\"ts\":%ld,\"who\":", G.msgs[i].id, G.msgs[i].from, (long)G.msgs[i].ts);
        json_str(f, G.msgs[i].from == 1 ? G.devname : G.msgs[i].who);
        fputs(",\"text\":", f);
        json_str(f, G.msgs[i].text);
        fputs(",\"file\":", f);
        json_str(f, G.msgs[i].file);
        fprintf(f, ",\"dl\":%d}", G.msgs[i].from == 1 && G.msgs[i].file[0] ? 1 : 0);
    }
    UNLOCK();
    fputs("]}", f);
    fclose(f);
    respond(fd, 200, "application/json", NULL, buf, blen);
    free(buf);
}

static const char *mime_of(const char *n) {
    const char *d = strrchr(n, '.');
    if (!d) return "application/octet-stream";
    static const struct { const char *e, *m; } T[] = { {".wav", "audio/wav"}, {".mp3", "audio/mpeg"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".bmp", "image/bmp"}, {".webp", "image/webp"}, {".m4a", "audio/mp4"}, {".ogg", "audio/ogg"}, {".aif", "audio/aiff"}, {".aiff", "audio/aiff"} };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++) if (!strcasecmp(d, T[i].e)) return T[i].m;
    return "application/octet-stream";
}

/* a picture that arrived is decoded here and shrunk to 32 x 20 tiles of 6 x 6 x 6 colours for the MPC's mosaic: the whole tile area is averaged,
 * the brightness range is stretched a little, and a light ordered dither keeps smooth areas smooth */
static int thumb_from_file(const char *path, const char *who, const char *name) {
    const char *d = strrchr(name, '.');
    if (!d || !(!strcasecmp(d, ".png") || !strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".gif") || !strcasecmp(d, ".bmp"))) return 0;
    int w = 0, h = 0, c = 0;
    if (!stbi_info(path, &w, &h, &c) || w < 1 || h < 1 || (long long)w * h > 40000000LL) return 0;
    unsigned char *px = stbi_load(path, &w, &h, &c, 3);
    if (!px) return 0;
    static float acc[NTILE][3];
    double cw = w, ch = w * (double)TH / TW;      /* the 32:20 window, centred, as large as the picture allows */
    if (ch > h) { ch = h; cw = h * (double)TW / TH; }
    double x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    for (int ty = 0; ty < TH; ty++) for (int tx = 0; tx < TW; tx++) {
        double sum[3] = { 0, 0, 0 };
        int cnt = 0, N = 10;
        for (int sy = 0; sy < N; sy++) for (int sx = 0; sx < N; sx++) {
            int X = (int)(x0 + (tx + (sx + 0.5) / N) * cw / TW), Y = (int)(y0 + (ty + (sy + 0.5) / N) * ch / TH);
            if (X < 0) X = 0;
            if (Y < 0) Y = 0;
            if (X >= w) X = w - 1;
            if (Y >= h) Y = h - 1;
            const unsigned char *p = px + ((size_t)Y * (size_t)w + (size_t)X) * 3;
            sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; cnt++;
        }
        for (int k = 0; k < 3; k++) acc[ty * TW + tx][k] = (float)(sum[k] / cnt);
    }
    stbi_image_free(px);
    float lo = 255, hi = 0;                       /* stretch: the darkest 2 % and brightest 2 % tiles of the luma become black and white */
    static float luma[NTILE];
    for (int i = 0; i < NTILE; i++) luma[i] = 0.3f * acc[i][0] + 0.59f * acc[i][1] + 0.11f * acc[i][2];
    { static float sorted[NTILE]; memcpy(sorted, luma, sizeof sorted);
      for (int i = 1; i < NTILE; i++) { float v = sorted[i]; int j = i - 1; while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; } sorted[j + 1] = v; }
      lo = sorted[NTILE * 2 / 100]; hi = sorted[NTILE * 98 / 100]; }
    float span = hi - lo < 40 ? 40 : hi - lo, mid = (hi + lo) / 2;
    if (span > 200) { span = 255; mid = 127.5f; }   /* a picture with a wide range is left as it is */
    unsigned char th[NTILE];
    for (int ty = 0; ty < TH; ty++) for (int tx = 0; tx < TW; tx++) {
        int i = ty * TW + tx, q[3];
        float want[3];
        static const float BAYER[4][4] = { { -7.5f, 0.5f, -5.5f, 2.5f }, { 4.5f, -3.5f, 6.5f, -1.5f }, { -4.5f, 3.5f, -6.5f, 1.5f }, { 7.5f, -0.5f, 5.5f, -2.5f } };
        float dith = BAYER[ty & 3][tx & 3] * 2.2f;   /* a light ordered dither: 6 levels per colour stay calm and still show smooth areas */
        for (int k = 0; k < 3; k++) { float v = (acc[i][k] - mid) * 255.0f / span + 127.5f + dith; want[k] = v < 0 ? 0 : v > 255 ? 255 : v; }
        for (int k = 0; k < 3; k++) { int v = (int)(want[k] / 51.0f + 0.5f); q[k] = v < 0 ? 0 : v > 5 ? 5 : v; }
        th[i] = (unsigned char)(q[0] * 36 + q[1] * 6 + q[2]);
    }
    LOCK();
    memcpy(G.thumb, th, NTILE);
    snprintf(G.thumb_who, sizeof G.thumb_who, "%s", who);
    snprintf(G.thumb_file, sizeof G.thumb_file, "%s", name);
    G.thumb_rev++;
    UNLOCK();
    return 1;
}

static void do_download(int fd, const char *rawname) {
    char name[120];
    url_decode(rawname, name, sizeof name);
    scan_files(0);
    int listed = 0;
    LOCK();
    for (int i = 0; i < G.nfiles; i++) if (!strcmp(G.files[i].name, name)) listed = 1;
    char path[400];
    snprintf(path, sizeof path, "%s/%s", G.outbox, name);
    UNLOCK();
    if (!name_ok(name) || !listed) { respond_str(fd, 404, "not found"); return; }
    int ffd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (ffd < 0 || fstat(ffd, &st) || !S_ISREG(st.st_mode)) { if (ffd >= 0) close(ffd); respond_str(fd, 404, "not found"); return; }
    char h[500];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n"
        "Content-Disposition: inline; filename=\"%s\"\r\nX-Content-Type-Options: nosniff\r\n\r\n", mime_of(name), (long long)st.st_size, name);
    if (send_all(fd, h, (size_t)n) == 0) {
        char *chunk = malloc(65536);
        if (chunk) {
            ssize_t r;
            while ((r = read(ffd, chunk, 65536)) > 0) if (send_all(fd, chunk, (size_t)r)) break;
            free(chunk);
        }
    }
    close(ffd);
}

/* create a new file in the inbox under a name_ok() name that does not exist yet; returns the fd and the final name */
static int inbox_create(const char *wanted, char *final, size_t cap, char *path, size_t pcap) {
    char name[100];
    clean_name(wanted, name, sizeof name);
    int ofd = -1;
    for (int k = 0; k < 100 && ofd < 0; k++) {
        if (k == 0) snprintf(final, cap, "%s", name);
        else {
            char *dot = strrchr(name, '.');
            if (dot && dot != name) snprintf(final, cap, "%.*s-%d%s", (int)(dot - name), name, k, dot);
            else snprintf(final, cap, "%s-%d", name, k);
        }
        LOCK();
        snprintf(path, pcap, "%s/%s", G.inbox, final);
        UNLOCK();
        ofd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (ofd < 0 && errno != EEXIST) break;
    }
    return ofd;
}

/* the first `have` body bytes are already in `body`; read the rest from the socket into the new inbox file */
static void do_upload(int fd, const char *who, const char *query, const char *body, size_t have, long long clen) {
    char raw[200], path[400], final[100];
    if (!query_get(query, "name", raw, sizeof raw)) snprintf(raw, sizeof raw, "file");
    if (clen <= 0 || clen > MAX_UPLOAD) { respond_str(fd, 413, "too large"); return; }
    int ofd = inbox_create(raw, final, sizeof final, path, sizeof path);
    if (ofd < 0) { respond_str(fd, 500, "cannot create file"); return; }
    long long got = 0;
    int bad = 0;
    if (have > (size_t)clen) have = (size_t)clen;
    if (have && write(ofd, body, have) != (ssize_t)have) bad = 1;
    got = (long long)have;
    char *chunk = malloc(65536);
    if (!chunk) bad = 1;
    while (!bad && got < clen) {
        size_t want = (clen - got) > 65536 ? 65536 : (size_t)(clen - got);
        ssize_t r = recv(fd, chunk, want, 0);
        if (r <= 0 || write(ofd, chunk, (size_t)r) != r) bad = 1;
        else got += r;
    }
    free(chunk);
    if (close(ofd)) bad = 1;
    if (bad) { unlink(path); respond_str(fd, 500, "upload failed"); return; }
    char sz[24], line[200];
    fmt_size(got, sz, sizeof sz);
    snprintf(line, sizeof line, "(%s)", sz);
    add_msg(0, who, line, final, 'I');
    atomic_store(&G.snd_req, 4);
    scan_files(1);
    respond_str(fd, 200, "ok");
    if (!thumb_from_file(path, who, final)) {   /* could not decode here (e.g. a HEIC photo): use the phone's own tiny preview, 240 letters, spread over the finer grid */
        char th[300] = "";
        if (query_get(query, "th", th, sizeof th) && strlen(th) == OLD_NTILE) {
            int ok = 1;
            for (int k = 0; k < OLD_NTILE; k++) if (!(isalnum((unsigned char)th[k]) || th[k] == '+' || th[k] == '/')) ok = 0;
            if (ok) {
                unsigned char nt[NTILE];
                for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) {
                    char ch = th[(y * 12 / TH) * 20 + (x * 20 / TW)];
                    int v = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '+' ? 62 : 63;
                    int r4 = v / 16, g4 = (v / 4) % 4, b4 = v % 4;
                    nt[y * TW + x] = (unsigned char)((r4 * 5 / 3) * 36 + (g4 * 5 / 3) * 6 + (b4 * 5 / 3));
                }
                LOCK(); memcpy(G.thumb, nt, NTILE); snprintf(G.thumb_who, sizeof G.thumb_who, "%s", who); snprintf(G.thumb_file, sizeof G.thumb_file, "%s", final); G.thumb_rev++; UNLOCK();
            }
        }
    }
}

static void handle(int fd) {
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char buf[HDR_MAX + 1];
    size_t len = 0;
    char *hend = NULL;
    while (!hend && len < HDR_MAX) {
        ssize_t r = recv(fd, buf + len, HDR_MAX - len, 0);
        if (r <= 0) return;
        len += (size_t)r;
        buf[len] = 0;
        hend = strstr(buf, "\r\n\r\n");
    }
    if (!hend) { respond_str(fd, 400, "headers too large"); return; }
    *hend = 0;
    char *body = hend + 4;
    size_t have = len - (size_t)(body - buf);

    char method[8], target[1000];
    if (sscanf(buf, "%7s %999s", method, target) != 2) { respond_str(fd, 400, "bad request"); return; }
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    const char *q = query ? query : "";
    int post = !strcmp(method, "POST"), get = !strcmp(method, "GET");
    if (!post && !get) { respond_str(fd, 400, "bad method"); return; }

    char tmp[64];
    long long clen = 0;
    if (get_header(buf, "Content-Length", tmp, sizeof tmp)) clen = strtoll(tmp, NULL, 10);
    if (post) {
        if (!get_header(buf, "X-LanMsg", tmp, sizeof tmp)) { respond_str(fd, 403, "forbidden"); return; }
        if (get_header(buf, "Expect", tmp, sizeof tmp) && !strncasecmp(tmp, "100-continue", 12))
            send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    if (get && !strcmp(target, "/")) {
        respond(fd, 200, "text/html; charset=utf-8", NULL, PAGE_HTML, sizeof PAGE_HTML - 1);
        return;
    }
    if (post && !strcmp(target, "/api/pair")) {
        char pb[64] = "";
        if (clen > 0 && clen < 64) {
            size_t got = have > (size_t)clen ? (size_t)clen : have;
            memcpy(pb, body, got);
            while (got < (size_t)clen) {
                ssize_t r = recv(fd, pb + got, (size_t)clen - got, 0);
                if (r <= 0) break;
                got += (size_t)r;
            }
            pb[got] = 0;
        }
        do_pair(fd, pb);
        return;
    }
    char who[20] = "";
    if (!auth_who(buf, who, sizeof who)) { respond_str(fd, 401, "join first"); return; }

    if (get && !strcmp(target, "/api/state")) { do_state(fd, q); return; }
    if (get && !strncmp(target, "/dl/", 4)) { do_download(fd, target + 4); return; }
    if (post && !strcmp(target, "/api/msg")) {
        if (clen <= 0 || clen > 1024) { respond_str(fd, 413, "message too long"); return; }
        char mb[1100];
        size_t got = have > (size_t)clen ? (size_t)clen : have;
        memcpy(mb, body, got);
        while (got < (size_t)clen) {
            ssize_t r = recv(fd, mb + got, (size_t)clen - got, 0);
            if (r <= 0) break;
            got += (size_t)r;
        }
        mb[got] = 0;
        if (got) { add_msg(0, who, mb, "", 0); atomic_store(&G.snd_req, 3); }
        respond_str(fd, 200, "ok");
        return;
    }
    if (post && !strcmp(target, "/api/upload")) { do_upload(fd, who, q, body, have, clen); return; }
    respond_str(fd, 404, "not found");
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle(fd);
    LOCK();
    for (int i = 0; i < MAX_CONN; i++) if (G.active_fd[i] == fd) G.active_fd[i] = -1;
    UNLOCK();
    close(fd);
    atomic_fetch_sub(&G.conns, 1);
    return NULL;
}

static int beacon_port(void) { const char *bp = getenv("LANMSG_BEACON_PORT"); return bp && atoi(bp) > 0 ? atoi(bp) : BEACON_PORT; }

/* tell other MPCs on the network that this room exists (UDP broadcast, every 2 s while the server is on) */
static void beacon_send(void) {
    if (G.beacon_fd < 0) return;
    time_t now = time(NULL);
    if (now - G.beacon_t < 2) return;
    G.beacon_t = now;
    char msg[80];
    LOCK();
    int n = snprintf(msg, sizeof msg, "LMSG1|%d|%s", G.port, G.devname);
    UNLOCK();
    const char *to = getenv("LANMSG_BEACON_TO");
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)beacon_port()) };
    if (to && *to) {   /* tests: send to one address instead of broadcasting */
        if (inet_pton(AF_INET, to, &a.sin_addr) == 1) sendto(G.beacon_fd, msg, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
        return;
    }
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(G.beacon_fd, msg, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
    struct ifaddrs *ifa, *p;
    if (!getifaddrs(&ifa)) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_broadaddr || p->ifa_broadaddr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_BROADCAST)) continue;
            struct sockaddr_in b = *(struct sockaddr_in *)p->ifa_broadaddr;
            b.sin_port = htons((uint16_t)beacon_port());
            sendto(G.beacon_fd, msg, (size_t)n, 0, (struct sockaddr *)&b, sizeof b);
        }
        freeifaddrs(ifa);
    }
}

static void *accept_loop(void *arg) {
    (void)arg;
    while (G.th_run) {
        beacon_send();
        struct pollfd p = { G.listen_fd, POLLIN, 0 };
        if (poll(&p, 1, 300) <= 0) continue;
        int fd = accept(G.listen_fd, NULL, NULL);
        if (fd < 0) continue;
        if (atomic_load(&G.conns) >= MAX_CONN) { respond_str(fd, 429, "busy"); close(fd); continue; }
        atomic_fetch_add(&G.conns, 1);
        LOCK();
        for (int i = 0; i < MAX_CONN; i++) if (G.active_fd[i] < 0) { G.active_fd[i] = fd; break; }
        UNLOCK();
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)fd)) {
            LOCK();
            for (int i = 0; i < MAX_CONN; i++) if (G.active_fd[i] == fd) G.active_fd[i] = -1;
            UNLOCK();
            close(fd);
            atomic_fetch_sub(&G.conns, 1);
        }
        pthread_attr_destroy(&at);
    }
    return NULL;
}

static void srv_start(void) {
    if (G.on || G.cl.state) return;   /* one role at a time: a joined MPC does not host */
    const char *pe = getenv("LANMSG_PORT");
    G.port = pe && atoi(pe) > 0 ? atoi(pe) : PORT_DEFAULT;
    G.err[0] = 0;
    make_dirs();
    ip_refresh();
    for (int i = 0; i < MAX_CONN; i++) G.active_fd[i] = -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { snprintf(G.err, sizeof G.err, "No socket"); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)G.port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 8)) {
        snprintf(G.err, sizeof G.err, errno == EADDRINUSE ? "Port %d busy" : "Cannot listen on %d", G.port);
        close(fd);
        return;
    }
    G.listen_fd = fd;
    G.beacon_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (G.beacon_fd >= 0) setsockopt(G.beacon_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    G.beacon_t = 0;
    G.th_run = 1;
    if (pthread_create(&G.th, NULL, accept_loop, NULL)) {
        G.th_run = 0; close(fd); G.listen_fd = -1;
        if (G.beacon_fd >= 0) { close(G.beacon_fd); G.beacon_fd = -1; }
        snprintf(G.err, sizeof G.err, "No thread");
        return;
    }
    G.on = 1;
    new_pin();
    scan_files(1);
}

static void srv_stop(void) {
    if (!G.on) return;
    G.th_run = 0;
    pthread_join(G.th, NULL);
    close(G.listen_fd);
    G.listen_fd = -1;
    if (G.beacon_fd >= 0) { close(G.beacon_fd); G.beacon_fd = -1; }
    LOCK();
    for (int i = 0; i < MAX_CONN; i++) if (G.active_fd[i] >= 0) shutdown(G.active_fd[i], SHUT_RDWR);
    G.ntok = 0;
    UNLOCK();
    for (int i = 0; i < 30 && atomic_load(&G.conns) > 0; i++) usleep(100000);   /* let running requests finish before the library can go */
    G.on = 0;
}

/* ---------- finding hosts (UDP listener, only while the JOIN page is open) ---------- */

static void *scan_loop(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)beacon_port()), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); fd = -1; }
    }
    while (G.scan_run) {
        time_t now = time(NULL);
        if (fd >= 0) {
            struct pollfd p = { fd, POLLIN, 0 };
            if (poll(&p, 1, 300) > 0) {
                char b[100];
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                ssize_t r = recvfrom(fd, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
                if (r > 6) {
                    b[r] = 0;
                    int port = 0;
                    char name[20] = "";
                    if (!strncmp(b, "LMSG1|", 6) && sscanf(b + 6, "%d|%19[A-Za-z0-9 _-]", &port, name) >= 1 && port > 0 && port < 65536) {
                        char ip[48];
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                        LOCK();
                        int own = !strcmp(ip, G.ip) && port == G.port;   /* not ourselves */
                        if (!own) {
                            int k = -1;
                            for (int i = 0; i < G.npeers; i++) if (!strcmp(G.peers[i].ip, ip) && G.peers[i].port == port) k = i;
                            if (k < 0 && G.npeers < NPEERS) k = G.npeers++;
                            if (k >= 0) {
                                snprintf(G.peers[k].ip, sizeof G.peers[k].ip, "%s", ip);
                                G.peers[k].port = port;
                                snprintf(G.peers[k].name, sizeof G.peers[k].name, "%s", name[0] ? name : "MPC");
                                G.peers[k].seen = now;
                            }
                        }
                        UNLOCK();
                    }
                }
            }
        } else usleep(300000);
        LOCK();   /* forget hosts that went quiet */
        for (int i = 0; i < G.npeers;) {
            if (now - G.peers[i].seen > 10 && G.cl.state != 2) {
                if (G.peer_sel == i) G.peer_sel = -1; else if (G.peer_sel > i) G.peer_sel--;
                memmove(&G.peers[i], &G.peers[i + 1], sizeof G.peers[0] * (size_t)(G.npeers - i - 1));
                G.npeers--;
            } else i++;
        }
        UNLOCK();
    }
    if (fd >= 0) close(fd);
    return NULL;
}

static void scan_start(void) {
    if (G.scan_alive) return;
    G.scan_run = 1;
    if (pthread_create(&G.scan_th, NULL, scan_loop, NULL)) { G.scan_run = 0; return; }
    G.scan_alive = 1;
    const char *tp = getenv("LANMSG_PEER");   /* tests: a fixed host "ip:port" */
    if (tp && *tp) {
        char ip[48];
        int port = 0;
        if (sscanf(tp, "%47[0-9.]:%d", ip, &port) == 2) {
            LOCK();
            if (G.npeers < NPEERS) {
                snprintf(G.peers[G.npeers].ip, sizeof G.peers[0].ip, "%s", ip);
                G.peers[G.npeers].port = port;
                snprintf(G.peers[G.npeers].name, sizeof G.peers[0].name, "TestHost");
                G.peers[G.npeers].seen = time(NULL) + 100000;
                G.npeers++;
            }
            UNLOCK();
        }
    }
}

static void scan_stop(void) {
    if (!G.scan_alive) return;
    G.scan_run = 0;
    pthread_join(G.scan_th, NULL);
    G.scan_alive = 0;
}

/* ---------- joining another MPC (HTTP client) ---------- */

static int cl_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r && errno != EINPROGRESS) { close(fd); return -1; }
    if (r) {
        int ok = 0;
        for (int i = 0; i < 30 && G.cl.run && !ok; i++) {   /* 3 s in slices, so LEAVE reacts quickly */
            struct pollfd p = { fd, POLLOUT, 0 };
            if (poll(&p, 1, 100) > 0) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) break;
                ok = 1;
            }
        }
        if (!ok) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    LOCK();
    G.cl.fd = fd;
    UNLOCK();
    return fd;
}

/* One request. Sends `body` or streams `upfd` (uplen bytes); the answer goes to *rbuf (malloc'd, 0-terminated, <= 256 KB)
 * or, with outfd >= 0 and status 200, straight into that file (<= MAX_UPLOAD). Returns the HTTP status or -1. */
static int http_req(const char *method, const char *path, const void *body, size_t blen, int upfd, long long uplen,
                    char **rbuf, int outfd, long long *outgot, char *setck, size_t ckcap) {
    if (rbuf) *rbuf = NULL;
    if (outgot) *outgot = 0;
    if (setck) setck[0] = 0;
    int fd = cl_connect(G.cl.ip, G.cl.port);
    if (fd < 0) return -1;
    char ck[80] = "";
    if (G.cl.cookie[0]) snprintf(ck, sizeof ck, "Cookie: mt=%s\r\n", G.cl.cookie);
    long long total = upfd >= 0 ? uplen : (long long)blen;
    char h[900];
    int n = snprintf(h, sizeof h, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nX-LanMsg: 1\r\n%sContent-Length: %lld\r\nConnection: close\r\n\r\n",
                     method, path, G.cl.ip, G.cl.port, ck, total);
    int status = -1;
    char *resp = NULL;
    size_t rl = 0, rcap = 0;
    if (n <= 0 || (size_t)n >= sizeof h || send_all(fd, h, (size_t)n)) goto done;
    if (upfd >= 0) {
        char *chunk = malloc(65536);
        if (!chunk) goto done;
        long long left = uplen;
        while (left > 0 && G.cl.run) {
            ssize_t r = read(upfd, chunk, left > 65536 ? 65536 : (size_t)left);
            if (r <= 0 || send_all(fd, chunk, (size_t)r)) { free(chunk); goto done; }
            left -= r;
        }
        free(chunk);
        if (left > 0) goto done;
    } else if (blen && send_all(fd, body, blen)) goto done;

    char head[HDR_MAX + 1];
    size_t hl = 0;
    char *he = NULL;
    while (!he && hl < HDR_MAX) {
        ssize_t r = recv(fd, head + hl, HDR_MAX - hl, 0);
        if (r <= 0) goto done;
        hl += (size_t)r;
        head[hl] = 0;
        he = strstr(head, "\r\n\r\n");
    }
    if (!he) goto done;
    *he = 0;
    if (sscanf(head, "HTTP/1.%*d %d", &status) != 1) { status = -1; goto done; }
    if (setck) {
        char v[200];
        if (get_header(head, "Set-Cookie", v, sizeof v)) {
            const char *p = strstr(v, "mt=");
            if (p) snprintf(setck, ckcap, "%.32s", p + 3);
        }
    }
    char *b0 = he + 4;
    size_t b0l = hl - (size_t)(b0 - head);
    long long got = 0;
    int to_file = outfd >= 0 && status == 200;
    if (to_file) {
        if (b0l && write(outfd, b0, b0l) != (ssize_t)b0l) { status = -1; goto done; }
        got = (long long)b0l;
        char *chunk = malloc(65536);
        if (!chunk) { status = -1; goto done; }
        ssize_t r;
        while (G.cl.run && (r = recv(fd, chunk, 65536, 0)) > 0) {
            got += r;
            if (got > MAX_UPLOAD || write(outfd, chunk, (size_t)r) != r) { status = -1; break; }
        }
        free(chunk);
        if (!G.cl.run) status = -1;
        if (outgot) *outgot = got;
    } else if (rbuf) {
        rcap = 4096;
        resp = malloc(rcap);
        if (!resp) { status = -1; goto done; }
        memcpy(resp, b0, b0l);
        rl = b0l;
        for (;;) {
            if (rl + 4096 + 1 > rcap) {
                if (rcap >= (256u << 10)) break;
                rcap *= 2;
                char *nb = realloc(resp, rcap);
                if (!nb) break;
                resp = nb;
            }
            ssize_t r = recv(fd, resp + rl, rcap - rl - 1, 0);
            if (r <= 0) break;
            rl += (size_t)r;
        }
        resp[rl] = 0;
        *rbuf = resp;
        resp = NULL;
    }
done:
    free(resp);
    LOCK();
    G.cl.fd = -1;
    UNLOCK();
    close(fd);
    return status;
}

/* ---- a tiny reader for the host's own JSON ---- */

/* p points at an opening quote; copies the string (unescaped, cut to cap) and returns the position after the closing quote */
static const char *js_str(const char *p, char *out, size_t cap) {
    size_t o = 0;
    p++;
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (*p == 'u') {
                unsigned v = 0;
                int k;
                for (k = 1; k <= 4 && hexv(p[k]) >= 0; k++) v = v * 16 + (unsigned)hexv(p[k]);
                p += k - 1;
                c = v < 128 ? v : '?';
            } else if (*p == 'n' || *p == 't' || *p == 'r') c = ' ';
            else if (*p) c = (unsigned char)*p;
            else break;
        }
        if (o + 1 < cap) out[o++] = (char)c;
        p++;
    }
    out[o] = 0;
    return *p == '"' ? p + 1 : p;
}

static const char *js_skip_value(const char *p) {   /* a string or a bare number/word */
    if (*p == '"') { char t[2]; return js_str(p, t, sizeof t); }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

typedef struct { unsigned id; int from, dl; char who[20], text[257], file[96]; } rmsg_t;

/* parses {"host":..,"online":[..],"msgs":[{..},..]}; fills up to max messages, host name and the online list */
static int parse_state(const char *s, rmsg_t *out, int max, char *host, size_t hcap, char *online, size_t ocap) {
    int n = 0;
    const char *p = strstr(s, "\"host\":");
    if (p) { p += 7; while (*p == ' ') p++; if (*p == '"') js_str(p, host, hcap); }
    p = strstr(s, "\"online\":[");
    if (p) {
        p += 10;
        size_t o = 0;
        online[0] = 0;
        while (*p && *p != ']') {
            if (*p == '"') {
                char nm[24];
                p = js_str(p, nm, sizeof nm);
                if (o + strlen(nm) + 3 < ocap) o += (size_t)snprintf(online + o, ocap - o, "%s%s", o ? ", " : "", nm);
            } else p++;
        }
    }
    p = strstr(s, "\"msgs\":[");
    if (!p) return 0;
    p += 8;
    while (*p && *p != ']' && n < max) {
        if (*p != '{') { p++; continue; }
        p++;
        rmsg_t m;
        memset(&m, 0, sizeof m);
        while (*p && *p != '}') {
            if (*p != '"') { p++; continue; }
            char key[16];
            p = js_str(p, key, sizeof key);
            while (*p == ' ' || *p == ':') p++;
            if (!strcmp(key, "id")) m.id = (unsigned)strtoul(p, NULL, 10);
            else if (!strcmp(key, "from")) m.from = atoi(p);
            else if (!strcmp(key, "dl")) m.dl = atoi(p);
            else if (!strcmp(key, "who") && *p == '"') { js_str(p, m.who, sizeof m.who); }
            else if (!strcmp(key, "text") && *p == '"') { js_str(p, m.text, sizeof m.text); }
            else if (!strcmp(key, "file") && *p == '"') { js_str(p, m.file, sizeof m.file); }
            p = js_skip_value(p);
        }
        if (*p == '}') p++;
        if (m.id) out[n++] = m;
    }
    return n;
}

static void cl_fail(const char *why) {
    LOCK();
    G.cl.state = 3;
    snprintf(G.cl.err, sizeof G.cl.err, "%s", why);
    UNLOCK();
}

static void cl_download(const char *name) {
    char path[400], final[100], url[300], enc[300];
    if (!name_ok(name)) return;
    int ofd = inbox_create(name, final, sizeof final, path, sizeof path);
    if (ofd < 0) return;
    url_encode(name, enc, sizeof enc);
    snprintf(url, sizeof url, "/dl/%s", enc);
    long long got = 0;
    int st = http_req("GET", url, NULL, 0, -1, 0, NULL, ofd, &got, NULL, 0);
    close(ofd);
    LOCK();
    int idx = -1;
    for (int i = G.nmsg - 1; i >= 0; i--) if (G.msgs[i].box == 'P' && !strcmp(G.msgs[i].file, name)) { idx = i; break; }
    if (st == 200) {
        if (idx >= 0) { G.msgs[idx].box = 'I'; snprintf(G.msgs[idx].lfile, sizeof G.msgs[idx].lfile, "%s", final); }
    } else {
        unlink(path);
        if (idx >= 0) G.msgs[idx].box = 'F';
    }
    UNLOCK();
    char hostn[20];
    snprintf(hostn, sizeof hostn, "%s", G.cl.host);
    if (st == 200) thumb_from_file(path, hostn, final);
    atomic_store(&G.snd_req, st == 200 ? 4 : 0);
    scan_files(1);
}

static void cl_send_file(const char *name) {
    char path[400], url[300], enc[300];
    if (!name_ok(name)) return;
    LOCK();
    snprintf(path, sizeof path, "%s/%s", G.outbox, name);
    UNLOCK();
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > MAX_UPLOAD) { if (fd >= 0) close(fd); return; }
    url_encode(name, enc, sizeof enc);
    snprintf(url, sizeof url, "/api/upload?name=%s", enc);
    http_req("POST", url, NULL, 0, fd, (long long)st.st_size, NULL, -1, NULL, NULL, 0);
    close(fd);
}

static void *client_loop(void *arg) {
    (void)arg;
    char pb[96], ck[40] = "";
    snprintf(pb, sizeof pb, "pin=%s&name=%s", G.cl.pin, G.cl.name);
    int st = http_req("POST", "/api/pair", pb, strlen(pb), -1, 0, NULL, -1, NULL, ck, sizeof ck);
    if (st != 200 || strlen(ck) != 32) {
        cl_fail(st == 429 ? "Locked for a minute (too many wrong codes)" : st == 401 ? "Wrong code" : st == -1 ? "Cannot reach the host" : "Join refused");
        goto out;
    }
    LOCK();
    snprintf(G.cl.cookie, sizeof G.cl.cookie, "%s", ck);
    G.cl.state = 2;
    G.cl.last = 0;
    UNLOCK();
    atomic_store(&G.snd_req, 1);
    make_dirs();
    scan_files(1);
    int fails = 0;
    double next_poll = 0;
    while (G.cl.run) {
        /* things the user asked for first */
        char t[64] = "", f[96] = "";
        LOCK();
        if (G.cl.req_n > 0) { snprintf(t, sizeof t, "%s", G.cl.req_text[0]); memmove(G.cl.req_text[0], G.cl.req_text[1], sizeof G.cl.req_text[0] * 3); G.cl.req_n--; }
        snprintf(f, sizeof f, "%s", G.cl.req_file);
        G.cl.req_file[0] = 0;
        UNLOCK();
        if (t[0]) http_req("POST", "/api/msg", t, strlen(t), -1, 0, NULL, -1, NULL, NULL, 0);
        if (f[0]) cl_send_file(f);
        double now = mono();
        if (now >= next_poll || t[0] || f[0]) {
            char url[64], *rb = NULL;
            LOCK();
            snprintf(url, sizeof url, "/api/state?since=%u", G.cl.last);
            UNLOCK();
            int s2 = http_req("GET", url, NULL, 0, -1, 0, &rb, -1, NULL, NULL, 0);
            next_poll = mono() + 1.0;
            if (s2 != 200 || !rb) {
                free(rb);
                if (!G.cl.run) break;
                if (++fails >= 4) { cl_fail(s2 == 401 ? "The host closed the room" : "Connection lost"); break; }
                continue;
            }
            fails = 0;
            static __thread rmsg_t ms[40];
            char host[20] = "", onl[160] = "";
            int n = parse_state(rb, ms, 40, host, sizeof host, onl, sizeof onl);
            free(rb);
            LOCK();
            if (host[0]) snprintf(G.cl.host, sizeof G.cl.host, "%s", host);
            snprintf(G.cl.online, sizeof G.cl.online, "%s", onl);
            UNLOCK();
            for (int i = 0; i < n && G.cl.run; i++) {
                const rmsg_t *m = &ms[i];
                LOCK();
                G.cl.last = m->id > G.cl.last ? m->id : G.cl.last;
                int mine = !strcmp(m->who, G.cl.name) && m->from == 0;
                UNLOCK();
                if (m->from == 2) add_msg(2, "", m->text, "", 0);
                else if (mine) add_msg(1, G.cl.name, m->text, m->file, m->file[0] ? 'O' : 0);
                else if (m->file[0] && m->dl) {
                    add_msg(0, m->who, m->text, m->file, 'P');
                    atomic_store(&G.snd_req, 3);
                    cl_download(m->file);
                } else {
                    add_msg(0, m->who, m->text, m->file, 0);
                    if (m->from == 0 || !m->file[0]) atomic_store(&G.snd_req, 3);
                }
            }
        }
        usleep(100000);
    }
out:
    LOCK();
    G.cl.cookie[0] = 0;
    if (G.cl.state == 2) G.cl.state = 0;
    UNLOCK();
    return NULL;
}

static void cl_stop(void) {
    if (!G.cl.alive) return;
    G.cl.run = 0;
    LOCK();
    if (G.cl.fd >= 0) shutdown(G.cl.fd, SHUT_RDWR);
    UNLOCK();
    pthread_join(G.cl.th, NULL);
    G.cl.alive = 0;
    LOCK();
    if (G.cl.state != 3) G.cl.state = 0;
    UNLOCK();
}

static void cl_start(void) {
    if (G.on) return;
    if (G.cl.alive) {   /* a finished run (failed) can be started again */
        if (G.cl.state == 1 || G.cl.state == 2) return;
        cl_stop();
    }
    LOCK();
    if (G.peer_sel < 0 || G.peer_sel >= G.npeers || strlen(G.jcode) != 6) { UNLOCK(); return; }
    snprintf(G.cl.ip, sizeof G.cl.ip, "%s", G.peers[G.peer_sel].ip);
    G.cl.port = G.peers[G.peer_sel].port;
    snprintf(G.cl.host, sizeof G.cl.host, "%s", G.peers[G.peer_sel].name);
    snprintf(G.cl.pin, sizeof G.cl.pin, "%s", G.jcode);
    G.cl.req_n = 0;
    G.cl.req_file[0] = 0;
    G.cl.online[0] = 0;
    G.cl.err[0] = 0;
    G.cl.state = 1;
    G.cl.run = 1;
    G.cl.fd = -1;
    G.nmsg = 0;   /* a new room, a new chat */
    UNLOCK();
    ip_refresh();
    LOCK();
    snprintf(G.cl.name, sizeof G.cl.name, "%s", G.devname);
    UNLOCK();
    make_dirs();
    if (pthread_create(&G.cl.th, NULL, client_loop, NULL)) { cl_fail("No thread"); G.cl.run = 0; return; }
    G.cl.alive = 1;
}

/* ---------- QR code for the phone ---------- */

static void qr_refresh(void) {   /* called with mu held */
    char t[80] = "";
    if (G.on) {
        snprintf(t, sizeof t, "http://%s:%d/?c=%s", G.ip, G.port, G.pin);
        if (strlen(t) > 53) snprintf(t, sizeof t, "http://%s:%d/", G.ip, G.port);
    }
    if (!strcmp(t, G.qr_text) && G.qr_ok >= 0) return;
    snprintf(G.qr_text, sizeof G.qr_text, "%s", t);
    memset(G.qr, 0, sizeof G.qr);
    G.qr_ok = t[0] && qr_encode(t, G.qr) == 0 ? 1 : 0;
}

/* value of meter `i` (3 x 3 modules at group column i%10, row i/10): the frame number as a value in 0..1 that every common
 * way of turning a value into a frame (floor or round of v*(N-1), floor of v*N) maps to the same frame */
static double meter_value(int i) {
    int gx = i % 10, gy = i / 10, f = 0;
    if (G.qr_ok == 1)
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
            int x = gx * 3 + c, y = gy * 3 + r;
            if (x < QR_N && y < QR_N && G.qr[y][x]) f |= 1 << (r * 3 + c);
        }
    const double N = METER_FRAMES;
    if (f == METER_FRAMES - 1) return 1.0;
    double lo = f / (N - 1), hi = (f + 0.5) / (N - 1), h2 = (f + 1) / N;
    if (h2 < hi) hi = h2;
    return (lo + hi) / 2;
}

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

/* ---------- the pad set is kept on the card, so the pads are there again after a restart (pads/padNN.wav + pads/pads.cfg) ---------- */

/* the sound shaping of a pad: every value is a step of a table */
static const int ATT_MS[] = { 0, 2, 5, 10, 20, 50, 100, 200, 500 };
static const int DEC_MS[] = { 0, 50, 100, 200, 400, 800, 1500, 3000 };      /* 0 = off: the whole sample plays */
static const int REL_MS[] = { 5, 20, 50, 100, 200, 400, 800, 1500 };
static const int TEMPO_PCT[] = { 50, 67, 80, 90, 100, 110, 125, 150, 200 };
#define N_ATT ((int)(sizeof ATT_MS / sizeof *ATT_MS))
#define N_DEC ((int)(sizeof DEC_MS / sizeof *DEC_MS))
#define N_REL ((int)(sizeof REL_MS / sizeof *REL_MS))
#define N_TEMPO ((int)(sizeof TEMPO_PCT / sizeof *TEMPO_PCT))
#define N_GRP 9
#define N_START 50     /* start: 0..49 = 0..98 % of the sample */
#define N_END 51       /* end: 1..50 = 2..100 % */
#define N_PITCH 25     /* 0..24 = -12..+12 semitones */
static void pc_default(void) {
    for (int i = 0; i < NPADS; i++) { G.pc[i].att = 0; G.pc[i].dec = 0; G.pc[i].rel = 1; G.pc[i].grp = 0; G.pc[i].mode = 0; G.pc[i].start = 0; G.pc[i].end = 50; G.pc[i].pitch = 12; G.pc[i].tempo = 4; G.pc[i].stretch = 0; }
}

static void cfg_save(void) {
    char dir[300], path[330], tmp[340];
    signed char map[128];
    LOCK();
    snprintf(dir, sizeof dir, "%s/pads", G.base);
    memcpy(map, G.padmap, sizeof map);
    int have = G.base[0];
    UNLOCK();
    if (!have) return;
    mkdir(dir, 0755);
    char names[NPADS][40];
    unsigned char pcs[NPADS][10];
    pthread_mutex_lock(&G.padmu);
    for (int i = 0; i < NPADS; i++) { snprintf(names[i], sizeof names[i], "%s", G.pad[i].d ? G.pad[i].name : ""); pcs[i][0] = G.pc[i].att; pcs[i][1] = G.pc[i].dec; pcs[i][2] = G.pc[i].rel; pcs[i][3] = G.pc[i].grp; pcs[i][4] = G.pc[i].mode; pcs[i][5] = G.pc[i].start; pcs[i][6] = G.pc[i].end; pcs[i][7] = G.pc[i].pitch; pcs[i][8] = G.pc[i].tempo; pcs[i][9] = G.pc[i].stretch; }
    pthread_mutex_unlock(&G.padmu);
    snprintf(path, sizeof path, "%s/pads.cfg", dir);
    snprintf(tmp, sizeof tmp, "%s/pads.tmp", dir);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < NPADS; i++) { if (names[i][0]) fprintf(f, "name %d %s\n", i, names[i]); fprintf(f, "env %d %d %d %d %d %d %d %d %d %d %d\n", i, pcs[i][0], pcs[i][1], pcs[i][2], pcs[i][3], pcs[i][4], pcs[i][5], pcs[i][6], pcs[i][7], pcs[i][8], pcs[i][9]); }
    fprintf(f, "maps\n");
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
    /* the pads of the MPC send notes 36-51 (drum program) or, on a plugin track, a scale from C-something: all the usual ranges play the 16 pads */
    static const int BASE[] = { 0, 36, 48, 60, 72 };
    for (size_t b = 0; b < sizeof BASE / sizeof *BASE; b++) for (int k = 0; k < 16; k++) if (BASE[b] + k < 128) G.padmap[BASE[b] + k] = (signed char)k;
}

static void *pads_boot(void *a) {
    (void)a;
    char dir[300], line[200], names[NPADS][40];
    memset(names, 0, sizeof names);
    LOCK(); snprintf(dir, sizeof dir, "%s/pads", G.base); UNLOCK();
    char path[330];
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
            else if (!strncmp(line, "env ", 4)) {
                int pi, a, d, rl, g, md, st, en, pt, tp, sx;
                if (sscanf(line, "env %d %d %d %d %d %d %d %d %d %d %d", &pi, &a, &d, &rl, &g, &md, &st, &en, &pt, &tp, &sx) == 11 && pi >= 0 && pi < NPADS &&
                    a >= 0 && a < N_ATT && d >= 0 && d < N_DEC && rl >= 0 && rl < N_REL && g >= 0 && g < N_GRP && st >= 0 && st < N_START && en >= 1 && en < N_END && st < en &&
                    pt >= 0 && pt < N_PITCH && tp >= 0 && tp < N_TEMPO) {
                    pthread_mutex_lock(&G.padmu);
                    G.pc[pi].att = (unsigned char)a; G.pc[pi].dec = (unsigned char)d; G.pc[pi].rel = (unsigned char)rl; G.pc[pi].grp = (unsigned char)g; G.pc[pi].mode = (unsigned char)(md ? 1 : 0);
                    G.pc[pi].start = (unsigned char)st; G.pc[pi].end = (unsigned char)en; G.pc[pi].pitch = (unsigned char)pt; G.pc[pi].tempo = (unsigned char)tp; G.pc[pi].stretch = (unsigned char)(sx ? 1 : 0);
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
    return NULL;
}

typedef struct { int pad; char path[500]; } padjob_t;

static int has_ext(const char *p, const char *e) { const char *d = strrchr(p, '.'); return d && !strcasecmp(d, e); }

static void *pad_loader(void *arg) {
    padjob_t *j = arg;
    const char *why = "Cannot read the file";
    int16_t *d = NULL;
    int frames = 0, ok = 0, was_mp3 = 0;
    int fd = open(j->path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 44 && st.st_size < (200LL << 20)) {
        uint8_t *buf = malloc((size_t)st.st_size);
        if (buf) {
            size_t got = 0;
            ssize_t r;
            while (got < (size_t)st.st_size && (r = read(fd, buf + got, (size_t)st.st_size - got)) > 0) got += (size_t)r;
            if (got == (size_t)st.st_size) {
                if (!memcmp(buf, "RIFF", 4)) ok = wav_decode(buf, got, &d, &frames, &why) == 0;
                else { ok = mp3_decode(buf, got, &d, &frames, &why) == 0; was_mp3 = ok; }
                if (!ok && !has_ext(j->path, ".mp3") && memcmp(buf, "RIFF", 4)) why = "Only WAV and MP3 files can go on a pad";
            }
            free(buf);
        }
    } else if (fd >= 0 && !fstat(fd, &st) && st.st_size >= (200LL << 20)) why = "File too big";
    if (fd >= 0) close(fd);
    char msg[100];
    if (ok) {
        const char *b = strrchr(j->path, '/');
        b = b ? b + 1 : j->path;
        char base[200], dir[300], pp[330];
        LOCK(); snprintf(base, sizeof base, "%s", G.base); UNLOCK();
        snprintf(dir, sizeof dir, "%s/pads", base);
        mkdir(dir, 0755);
        snprintf(pp, sizeof pp, "%s/pad%02d.wav", dir, j->pad + 1);
        wav_write(pp, d, frames, 0);   /* the pad's own copy on the card */
        if (was_mp3) {   /* the converted WAV also lands next to the MP3 in the inbox, so it can be used elsewhere on the MPC */
            char wn[200], wp[330];
            snprintf(wn, sizeof wn, "%s", b);
            char *dot = strrchr(wn, '.');
            if (dot) *dot = 0;
            snprintf(wp, sizeof wp, "%s/%.150s.wav", G.inbox, wn);
            wav_write(wp, d, frames, 1);
        }
        pthread_mutex_lock(&G.padmu);
        int16_t *old = G.pad[j->pad].d;
        G.pad[j->pad].d = d;
        G.pad[j->pad].frames = frames;
        disp_text(G.pad[j->pad].name, sizeof G.pad[j->pad].name, b, 26);
        pthread_mutex_unlock(&G.padmu);
        free(old);
        cfg_save();
        scan_files(1);
        snprintf(msg, sizeof msg, was_mp3 ? "Pad %d loaded (MP3 converted to WAV)" : "Pad %d loaded", j->pad + 1);
    } else snprintf(msg, sizeof msg, "Pad %d: %s", j->pad + 1, why);
    note_set(msg);
    free(j);
    return NULL;
}

static void pad_load(int pad, const char *path) {
    if (pad < 0 || pad >= NPADS) return;
    padjob_t *j = calloc(1, sizeof *j);
    if (!j) return;
    j->pad = pad;
    snprintf(j->path, sizeof j->path, "%s", path);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&t, &at, pad_loader, j)) free(j);
    else note_set("Loading...");
    pthread_attr_destroy(&at);
}

static void pad_free_all(void) {
    pthread_mutex_lock(&G.padmu);
    for (int i = 0; i < NPADS; i++) { free(G.pad[i].d); G.pad[i].d = NULL; G.pad[i].frames = 0; G.pad[i].name[0] = 0; }
    pthread_mutex_unlock(&G.padmu);
}

/* ---------- browsing the MPC's folders and copying files to the outbox ---------- */

static int cmp_bent(const void *a, const void *b) {
    const bent_t *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void browse_scan(void) {   /* mu not held */
    char path[400];
    LOCK();
    if (!G.bpath[0]) { struct stat sb; snprintf(G.bpath, sizeof G.bpath, "%s", !stat("/sdcard", &sb) ? "/sdcard" : "/"); }
    snprintf(path, sizeof path, "%s", G.bpath);
    UNLOCK();
    static __thread bent_t tmp[MAX_BENT];
    int n = 0;
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < MAX_BENT) {
            if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof tmp[0].name) continue;
            char p[1000];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", !strcmp(path, "/") ? "" : path, e->d_name);
            if (stat(p, &st)) continue;
            if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
            snprintf(tmp[n].name, sizeof tmp[n].name, "%s", e->d_name);
            tmp[n].dir = S_ISDIR(st.st_mode);
            tmp[n].size = st.st_size;
            n++;
        }
        closedir(d);
    }
    qsort(tmp, (size_t)n, sizeof tmp[0], cmp_bent);
    LOCK();
    memcpy(G.bent, tmp, sizeof(bent_t) * (size_t)n);
    G.nbent = n;
    G.bpage = 0;
    G.bsel = -1;
    UNLOCK();
}

static void browse_enter(int k) {
    char np[400] = "";
    LOCK();
    if (k >= 0 && k < G.nbent && G.bent[k].dir) {
        snprintf(np, sizeof np, "%s%s%s", G.bpath, !strcmp(G.bpath, "/") ? "" : "/", G.bent[k].name);
        if (strlen(np) < sizeof G.bpath) snprintf(G.bpath, sizeof G.bpath, "%s", np); else np[0] = 0;
    } else if (k >= 0 && k < G.nbent) G.bsel = k;
    UNLOCK();
    if (np[0]) browse_scan();
}

static void browse_up(void) {
    LOCK();
    char *sl = strrchr(G.bpath, '/');
    if (sl && sl != G.bpath) *sl = 0; else snprintf(G.bpath, sizeof G.bpath, "/");
    UNLOCK();
    browse_scan();
}

static int browse_selected_path(char *out, size_t cap) {
    int ok = 0;
    LOCK();
    if (G.bsel >= 0 && G.bsel < G.nbent && !G.bent[G.bsel].dir) {
        snprintf(out, cap, "%s%s%s", G.bpath, !strcmp(G.bpath, "/") ? "" : "/", G.bent[G.bsel].name);
        ok = 1;
    }
    UNLOCK();
    return ok;
}

static void *copy_worker(void *arg) {
    char *src = arg;
    char name[100], final[100], path[400];
    const char *b = strrchr(src, '/');
    clean_name(b ? b + 1 : src, name, sizeof name);
    int in = open(src, O_RDONLY | O_CLOEXEC);
    struct stat st;
    int ok = 0;
    if (in >= 0 && !fstat(in, &st) && S_ISREG(st.st_mode) && st.st_size <= MAX_UPLOAD) {
        int ofd = -1;
        for (int k = 0; k < 100 && ofd < 0; k++) {
            if (k == 0) snprintf(final, sizeof final, "%s", name);
            else {
                char *dot = strrchr(name, '.');
                if (dot && dot != name) snprintf(final, sizeof final, "%.*s-%d%s", (int)(dot - name), name, k, dot);
                else snprintf(final, sizeof final, "%s-%d", name, k);
            }
            LOCK();
            snprintf(path, sizeof path, "%s/%s", G.outbox, final);
            UNLOCK();
            ofd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (ofd < 0 && errno != EEXIST) break;
        }
        if (ofd >= 0) {
            char *chunk = malloc(65536);
            ok = chunk != NULL;
            ssize_t r;
            while (ok && (r = read(in, chunk, 65536)) > 0) if (write(ofd, chunk, (size_t)r) != r) ok = 0;
            free(chunk);
            if (close(ofd)) ok = 0;
            if (!ok) unlink(path);
        }
    }
    if (in >= 0) close(in);
    char msg[100];
    if (ok) snprintf(msg, sizeof msg, "Copied to OUTBOX: %.60s", final);
    else snprintf(msg, sizeof msg, "Copy failed (file too big or no access)");
    note_set(msg);
    scan_files(1);
    free(src);
    return NULL;
}

static void copy_to_outbox(const char *src) {
    make_dirs();
    char *s = strdup(src);
    if (!s) return;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&t, &at, copy_worker, s)) free(s);
    else note_set("Copying...");
    pthread_attr_destroy(&at);
}

/* ---------- engine ---------- */

static int chat_visible(void) { return G.on || G.cl.state >= 2; }

typedef struct { float f; int ms; } note_t;   /* f == 0: end of the tone */
static const note_t S_ONLINE[] = { {523.25f, 90}, {659.25f, 90}, {783.99f, 160}, {0, 0} };
static const note_t S_SENT[] = { {880.0f, 60}, {1318.5f, 90}, {0, 0} };
static const note_t S_RECV[] = { {783.99f, 110}, {1046.5f, 200}, {0, 0} };
static const note_t S_FILE[] = { {659.25f, 70}, {987.77f, 70}, {1318.5f, 70}, {1760.0f, 150}, {0, 0} };   /* a file or a picture arrived */
static const note_t *const SEQ[4] = { S_ONLINE, S_SENT, S_RECV, S_FILE };

typedef struct {
    int quick, obrowse;
    const note_t *seq; int ni; long pos; double ph; double born;
    atomic_int trig[NPADS];                                  /* pad taps and MIDI notes wait here for the audio thread */
    atomic_int rel[NPADS];                                   /* a note-off (hold mode) */
    atomic_int stop;                                         /* the transport stopped */
    int was_playing;
    struct { int on; double pos; float gain; long age, rage; int ngr, hopc; struct { double src; long age; } gr[5]; } voice[NPADS];   /* every plugin instance plays its own pads, so they sound with any view open */
} inst_t;

static void *create(const char *dir) {
    (void)dir;
    inst_t *s = calloc(1, sizeof *s);
    if (s) {
        s->born = mono();
        if (!G.base[0]) make_dirs();   /* so the INBOX, OUTBOX and PADS can use the folders without a server */
        LOCK();
        G.refs++;
        int boot = !G.pads_boot;
        if (boot) { G.pads_boot = 1; map_default(); pc_default(); }
        UNLOCK();
        if (boot) {
            pthread_t t;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&at, 256 * 1024);
            pthread_create(&t, &at, pads_boot, NULL);
            pthread_attr_destroy(&at);
        }
        ip_refresh();
    }
    return s;
}

static void destroy(void *i) {
    LOCK();
    int last = --G.refs <= 0;
    if (last) G.pads_boot = 0;
    UNLOCK();
    free(i);
    if (last) { cl_stop(); scan_stop(); srv_stop(); pad_free_all(); }
}

static void pad_trigger(inst_t *s, int p, int vel) {
    if (p < 0 || p >= NPADS) return;
    atomic_store(&s->trig[p], vel < 1 ? 1 : vel > 127 ? 127 : vel);
}

/* the physical pads: every MIDI note is looked up in the note table (default notes 0-15 and 36-51). LEARN rewrites that table and saves it. */
static void midi(void *i, const uint8_t *m, int n) {
    inst_t *s = i;
    if (n >= 3 && ((m[0] & 0xF0) == 0x80 || ((m[0] & 0xF0) == 0x90 && m[2] == 0))) {   /* note off: lets go of a hold-mode pad */
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
        G.last_note = note; G.last_pad = p;
        UNLOCK();
        if (learned) { note_set(msg); cfg_save_async(); }
        if (p >= 0) atomic_store(&s->rel[p], 0);
        pad_trigger(s, p, m[2]);
    }
}

/* the host's transport: when it stops, every pad sound lets go (with its release), like the other sounds in the project */
static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    (void)ppq; (void)bpm; (void)frames;
    inst_t *s = inst;
    if (s->was_playing && !playing) atomic_store(&s->stop, 1);
    s->was_playing = playing ? 1 : 0;
}

/* MPC sends a tap twice now and then: the same trigger again within 250 ms is one tap */
static int tap_repeat(const char *key) {
    static char last[24];
    static double t;
    double now = mono();
    if (!strcmp(last, key) && now - t < 0.25) return 1;
    snprintf(last, sizeof last, "%s", key);
    t = now;
    return 0;
}

/* "ct3" with prefix "ct" and max 7 -> 3; 0 when the key does not have that shape */
static int key_idx(const char *key, const char *prefix, int max) {
    size_t pl = strlen(prefix);
    if (strncmp(key, prefix, pl) || !isdigit((unsigned char)key[pl])) return 0;
    char *end;
    long v = strtol(key + pl, &end, 10);
    return *end == 0 && v >= 1 && v <= max ? (int)v : 0;
}

static void send_text(const char *t) {
    if (G.on) { LOCK(); char nm[20]; snprintf(nm, sizeof nm, "%s", G.devname); UNLOCK(); add_msg(1, nm, t, "", 0); atomic_store(&G.snd_req, 2); }
    else if (G.cl.state == 2) {
        LOCK();
        if (G.cl.req_n < 4) snprintf(G.cl.req_text[G.cl.req_n++], sizeof G.cl.req_text[0], "%s", t);
        UNLOCK();
        atomic_store(&G.snd_req, 2);
    }
}

static void send_file_sel(void) {
    char nm[96] = "";
    scan_files(1);
    LOCK();
    if (G.out_sel >= 0 && G.out_sel < G.nfiles) snprintf(nm, sizeof nm, "%s", G.files[G.out_sel].name);
    UNLOCK();
    if (!nm[0]) { note_set("Pick a file in the OUTBOX first"); return; }
    if (G.on) { LOCK(); char dn[20]; snprintf(dn, sizeof dn, "%s", G.devname); UNLOCK(); add_msg(1, dn, "", nm, 'O'); atomic_store(&G.snd_req, 2); }
    else if (G.cl.state == 2) { LOCK(); snprintf(G.cl.req_file, sizeof G.cl.req_file, "%s", nm); UNLOCK(); atomic_store(&G.snd_req, 2); }
    else note_set("Not connected: open HOST or JOIN first");
}

static int page_of(int sel, int cur) { return sel >= 0 ? sel / ROWS : cur; }

/* a tap on a chat row with a file selects that file in the INBOX or OUTBOX list, so the tab shows it when opened */
static void chat_link(int r) {
    LOCK();
    int idx = G.nmsg - ROWS + r;
    char box = 0, lf[96] = "";
    if (chat_visible() && idx >= 0 && idx < G.nmsg) { box = G.msgs[idx].box; snprintf(lf, sizeof lf, "%s", G.msgs[idx].lfile); }
    UNLOCK();
    if (box != 'I' && box != 'O') return;
    scan_files(1);
    LOCK();
    fent_t *arr = box == 'I' ? G.infiles : G.files;
    int n = box == 'I' ? G.ninfiles : G.nfiles, k = -1;
    for (int i = 0; i < n; i++) if (!strcmp(arr[i].name, lf)) k = i;
    if (k >= 0) {
        if (box == 'I') { G.in_sel = k; G.in_page = page_of(k, G.in_page); }
        else { G.out_sel = k; G.out_page = page_of(k, G.out_page); }
    }
    UNLOCK();
    note_set(box == 'I' ? "File selected: open the INBOX tab" : "File selected: open the OUTBOX tab");
}

static void inbox_to_pad(void) {
    char path[500] = "";
    LOCK();
    if (G.in_sel >= 0 && G.in_sel < G.ninfiles) snprintf(path, sizeof path, "%s/%s", G.inbox, G.infiles[G.in_sel].name);
    int p = G.pad_sel;
    UNLOCK();
    if (path[0]) pad_load(p, path); else note_set("Pick a file in the INBOX first");
}

/* CLEAR INBOX / OUTBOX: the files are moved into a folder called _cleared inside it (nothing is deleted, so a mistake can be undone from a computer) */
static void move_all_files(const char *dir, int dots) {
    char sub[420];
    snprintf(sub, sizeof sub, "%s/_cleared", dir);
    mkdir(sub, 0755);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *en;
    while ((en = readdir(d))) {
        if (en->d_name[0] == '.') continue;
        if (!dots && !name_ok(en->d_name)) continue;
        char from[420], to[520];
        struct stat st;
        snprintf(from, sizeof from, "%s/%s", dir, en->d_name);
        if (lstat(from, &st) || !S_ISREG(st.st_mode)) continue;
        snprintf(to, sizeof to, "%s/%s", sub, en->d_name);
        if (!access(to, F_OK)) snprintf(to, sizeof to, "%s/%ld-%s", sub, (long)time(NULL), en->d_name);
        rename(from, to);
    }
    closedir(d);
}
static void clear_box(int inbox) {
    char dir[260];
    LOCK(); snprintf(dir, sizeof dir, "%s", inbox ? G.inbox : G.outbox); UNLOCK();
    if (dir[0]) move_all_files(dir, 0);
    LOCK(); if (inbox) { G.thumb_rev = 0; G.in_sel = -1; G.in_page = 0; } else { G.out_sel = -1; G.out_page = 0; } UNLOCK();
    scan_files(1);
    note_set(inbox ? "Inbox is empty (files moved to the folder _cleared)" : "Outbox is empty (files moved to the folder _cleared)");
}

static void set_param(void *inst, const char *key, const char *val) {
    inst_t *s = inst;
    int v = atoi(val);
    int fire = atof(val) > 0.5;
    /* a project that was saved with the server on must not open the network by itself: ignore "on" while the project loads */
    if (!strcmp(key, "server")) { if (v) { if (mono() - s->born > 3.0) srv_start(); } else srv_stop(); return; }
    if (!strcmp(key, "scan")) { if (v) { if (mono() - s->born > 3.0) scan_start(); } else scan_stop(); return; }
    if (!strcmp(key, "quick")) { s->quick = v < 0 ? 0 : v >= NQUICK ? NQUICK - 1 : v; return; }
    if (!fire) return;
    int k;
    if ((k = key_idx(key, "pd", NPADS))) {   /* a pad on the screen: selects it and plays it (no repeat filter, drum rolls) */
        LOCK(); G.pad_sel = k - 1; UNLOCK();
        pad_trigger(s, k - 1, 110);
        return;
    }
    if (tap_repeat(key)) return;
    if (!strcmp(key, "ob_on")) { s->obrowse = 1; browse_scan(); return; }
    if (!strcmp(key, "ob_off")) { s->obrowse = 0; return; }
    if (!strcmp(key, "learn_pad") || !strcmp(key, "learn_base")) {
        int want = key[6] == 'p' ? 1 : 2;
        LOCK();
        G.learn = G.learn == want ? 0 : want;
        int l = G.learn;
        UNLOCK();
        note_set(l == 1 ? "Now hit the physical pad that should play this pad" : l == 2 ? "Now hit the physical pad that is PAD 1 (bottom left)" : "Learn cancelled");
        return;
    }
    if (!strcmp(key, "sendtxt")) { send_text(QUICK[s->quick]); return; }
    if (!strcmp(key, "sendfile")) { send_file_sel(); return; }
    if (!strcmp(key, "newcode")) { if (G.on) { new_pin(); LOCK(); G.ntok = 0; UNLOCK(); add_msg(2, "", "New code: everyone has to join again", "", 0); } return; }
    if (!strcmp(key, "chat_clear")) { LOCK(); G.nmsg = 0; G.epoch++; UNLOCK(); note_set("Chat is empty"); return; }
    if (!strcmp(key, "in_clear")) { clear_box(1); return; }
    if (!strcmp(key, "out_clear")) { clear_box(0); return; }
    if (!strcmp(key, "pad_prev") || !strcmp(key, "pad_next")) { LOCK(); G.pad_sel = (G.pad_sel + (key[4] == 'n' ? 1 : NPADS - 1)) % NPADS; UNLOCK(); return; }
    if (!strcmp(key, "pad_test")) { LOCK(); int p = G.pad_sel; UNLOCK(); pad_trigger(s, p, 110); atomic_store(&s->rel[p], 0); return; }
    {   /* the sound shaping of the selected pad */
        static const struct { const char *m, *p; int w; } SH[] = { {"att_m", "att_p", 1}, {"dec_m", "dec_p", 2}, {"rel_m", "rel_p", 3}, {"grp_m", "grp_p", 4}, {"st_m", "st_p", 6},
                                                                  {"en_m", "en_p", 7}, {"pi_m", "pi_p", 8}, {"tp_m", "tp_p", 9} };
        int which = 0, dir = 0;
        for (size_t q = 0; q < sizeof SH / sizeof *SH; q++) { if (!strcmp(key, SH[q].m)) { which = SH[q].w; dir = -1; } else if (!strcmp(key, SH[q].p)) { which = SH[q].w; dir = 1; } }
        if (!strcmp(key, "mode_t")) which = 5;
        if (!strcmp(key, "sx_t")) which = 10;
        if (which) {
            LOCK(); int p = G.pad_sel; UNLOCK();
            pthread_mutex_lock(&G.padmu);
            unsigned char *f = which == 1 ? &G.pc[p].att : which == 2 ? &G.pc[p].dec : which == 3 ? &G.pc[p].rel : which == 4 ? &G.pc[p].grp : which == 5 ? &G.pc[p].mode :
                               which == 6 ? &G.pc[p].start : which == 7 ? &G.pc[p].end : which == 8 ? &G.pc[p].pitch : which == 9 ? &G.pc[p].tempo : &G.pc[p].stretch;
            int lo = which == 7 ? 1 : 0;
            int mx = which == 1 ? N_ATT : which == 2 ? N_DEC : which == 3 ? N_REL : which == 4 ? N_GRP : which == 6 ? N_START : which == 7 ? N_END : which == 8 ? N_PITCH : which == 9 ? N_TEMPO : 2;
            int nv = *f + dir;
            if (which == 5 || which == 10) nv = (*f + 1) % 2;
            if (which == 6 && nv >= G.pc[p].end) nv = *f;              /* start stays before the end */
            if (which == 7 && nv <= G.pc[p].start) nv = *f;
            if (nv >= lo && nv < mx) *f = (unsigned char)nv;
            pthread_mutex_unlock(&G.padmu);
            LOCK(); G.rev_pad++; UNLOCK();
            cfg_save_async();
            return;
        }
    }
    if ((k = key_idx(key, "ct", ROWS))) { chat_link(k - 1); return; }
    if ((k = key_idx(key, "it", ROWS))) { LOCK(); int i = G.in_page * ROWS + k - 1; if (i < G.ninfiles) G.in_sel = i; UNLOCK(); return; }
    if ((k = key_idx(key, "ot", ROWS))) { LOCK(); int i = G.out_page * ROWS + k - 1; if (i < G.nfiles) G.out_sel = i; UNLOCK(); return; }
    if ((k = key_idx(key, "bt", ROWS))) { LOCK(); int i = G.bpage * ROWS + k - 1; UNLOCK(); browse_enter(i); return; }
    if (!strcmp(key, "in_prev")) { LOCK(); if (G.in_page > 0) G.in_page--; UNLOCK(); return; }
    if (!strcmp(key, "in_next")) { LOCK(); if ((G.in_page + 1) * ROWS < G.ninfiles) G.in_page++; UNLOCK(); return; }
    if (!strcmp(key, "out_prev")) { LOCK(); if (G.out_page > 0) G.out_page--; UNLOCK(); return; }
    if (!strcmp(key, "out_next")) { LOCK(); if ((G.out_page + 1) * ROWS < G.nfiles) G.out_page++; UNLOCK(); return; }
    if (!strcmp(key, "b_prev")) { LOCK(); if (G.bpage > 0) G.bpage--; UNLOCK(); return; }
    if (!strcmp(key, "b_next")) { LOCK(); if ((G.bpage + 1) * ROWS < G.nbent) G.bpage++; UNLOCK(); return; }
    if (!strcmp(key, "b_up")) { browse_up(); return; }
    if (!strcmp(key, "b_out")) {
        char p[500];
        if (browse_selected_path(p, sizeof p)) copy_to_outbox(p); else note_set("Tap a file first");
        return;
    }
    if (!strcmp(key, "b_pad")) {
        char p[500];
        LOCK(); int pad = G.pad_sel; UNLOCK();
        if (browse_selected_path(p, sizeof p)) pad_load(pad, p); else note_set("Tap a WAV file first");
        return;
    }
    if (!strcmp(key, "in_pad") || !strcmp(key, "pad_load")) { inbox_to_pad(); return; }
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
    if ((k = key_idx(key, "pt", NPEERS))) { LOCK(); if (k - 1 < G.npeers) G.peer_sel = k - 1; UNLOCK(); return; }
    if (key[0] == 'k' && key[1] == 'p' && key[2] >= '0' && key[2] <= '9' && !key[3]) {
        LOCK(); size_t l = strlen(G.jcode); if (l < 6) { G.jcode[l] = key[2]; G.jcode[l + 1] = 0; } UNLOCK(); return;
    }
    if (!strcmp(key, "kp_back")) { LOCK(); size_t l = strlen(G.jcode); if (l) G.jcode[l - 1] = 0; UNLOCK(); return; }
    if (!strcmp(key, "cl_btn")) {
        if (G.cl.alive && (G.cl.state == 1 || G.cl.state == 2)) cl_stop();
        else if (!G.on) cl_start();
        return;
    }
}

static int put(char *b, int n, const char *s) { int r = snprintf(b, (size_t)n, "%s", s[0] ? s : " "); return r > 0 ? r : 1; }

static void file_row(char *t, size_t cap, const fent_t *f, int sel) {
    char sz[24], nm[70];
    fmt_size(f->size, sz, sizeof sz);
    disp_text(nm, sizeof nm, f->name, 44);
    snprintf(t, cap, "%s%s   %s", sel ? "> " : "   ", nm, sz);
}

static void chat_row(char *t, size_t cap, const msg_t *m) {
    char who[40], txt[200], f[110], link[24] = "";
    disp_text(who, sizeof who, m->from == 1 ? "Me" : m->who, 14);
    disp_text(txt, sizeof txt, m->text, 70);
    disp_text(f, sizeof f, m->file, 36);
    if (m->box == 'I') snprintf(link, sizeof link, "   >> INBOX");
    else if (m->box == 'O') snprintf(link, sizeof link, "   >> OUTBOX");
    else if (m->box == 'P') snprintf(link, sizeof link, "   (loading...)");
    else if (m->box == 'F') snprintf(link, sizeof link, "   (failed)");
    if (m->from == 2) snprintf(t, cap, "* %.80s", txt);
    else if (m->file[0]) snprintf(t, cap, "%s: %s%s", who, f, link);
    else snprintf(t, cap, "%s: %s", who, txt);
}

static int is_trigger(const char *key) {
    static const char *const fixed[] = { "sendtxt", "sendfile", "newcode", "in_prev", "in_next", "out_prev", "out_next", "b_prev", "b_next", "b_up",
                                         "b_out", "b_pad", "in_pad", "pad_load", "pad_clear", "cl_btn", "kp_back", "ob_on", "ob_off", "learn_pad", "learn_base",
                                         "chat_clear", "in_clear", "out_clear", "pad_prev", "pad_next", "pad_test", "att_m", "att_p", "dec_m", "dec_p", "rel_m", "rel_p", "grp_m", "grp_p", "mode_t",
                                         "st_m", "st_p", "en_m", "en_p", "pi_m", "pi_p", "tp_m", "tp_p", "sx_t" };
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++) if (!strcmp(key, fixed[i])) return 1;
    return key_idx(key, "ct", ROWS) || key_idx(key, "it", ROWS) || key_idx(key, "ot", ROWS) || key_idx(key, "bt", ROWS) ||
           key_idx(key, "pt", NPEERS) || key_idx(key, "pd", NPADS) || (key[0] == 'k' && key[1] == 'p' && key[2] && !key[3]);
}

/* the value (0..1) that every common way of turning a value into a frame number (floor or round of v*(N-1), floor of v*N) maps to frame f of n */
static double frame_value(int f, int n) {
    if (f >= n - 1) return 1.0;
    double lo = f / (double)(n - 1), hi = (f + 0.5) / (double)(n - 1), h2 = (f + 1) / (double)n;
    if (h2 < hi) hi = h2;
    return (lo + hi) / 2;
}

static int get_param(void *inst, const char *key, char *b, int n) {
    inst_t *s = inst;
    char t[300] = "";
    int k;
    if (n <= 1) return 0;
    if (!strcmp(key, "server")) return snprintf(b, (size_t)n, "%d", G.on);
    if (!strcmp(key, "scan")) return snprintf(b, (size_t)n, "%d", G.scan_alive);
    if (!strcmp(key, "quick")) return snprintf(b, (size_t)n, "%d", s->quick);
    if (!strcmp(key, "obrowse")) return snprintf(b, (size_t)n, "%d", s->obrowse);
    if (!strcmp(key, "learn_txt")) {
        LOCK();
        int ln = G.learn, ps = G.pad_sel, first = -1, cnt = 0;
        for (int k = 0; k < 128; k++) if (G.padmap[k] == ps) { if (first < 0) first = k; cnt++; }
        UNLOCK();
        if (ln == 1) snprintf(t, sizeof t, "WAITING: hit the pad for PAD %d", ps + 1);
        else if (ln == 2) snprintf(t, sizeof t, "WAITING: hit PAD 1 (bottom left)");
        else if (cnt) snprintf(t, sizeof t, "PAD %d = MIDI note %d%s", ps + 1, first, cnt > 1 ? " +" : "");
        else snprintf(t, sizeof t, "PAD %d has no MIDI note: use LEARN", ps + 1);
        return put(b, n, t);
    }
    if (!strcmp(key, "qr_show")) return snprintf(b, (size_t)n, "%d", G.on ? 1 : 0);
    if (is_trigger(key)) return snprintf(b, (size_t)n, "0");
    if (!strcmp(key, "status")) {
        ip_refresh();
        LOCK();
        if (!G.on) snprintf(t, sizeof t, G.err[0] ? "%s" : "Server off", G.err);
        else snprintf(t, sizeof t, "http://%s:%d", G.ip, G.port);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "code_txt")) { if (G.on) { LOCK(); snprintf(t, sizeof t, "CODE  %.3s %.3s", G.pin, G.pin + 3); UNLOCK(); } return put(b, n, t); }
    if (!strcmp(key, "online_txt")) {
        if (G.on) {
            time_t now = time(NULL);
            size_t o = 0;
            LOCK();
            for (int i = 0; i < G.ntok; i++) {
                if (now - G.tk[i].seen > ONLINE_SECS) continue;
                o += (size_t)snprintf(t + o, sizeof t - o, "%s%s", o ? ", " : "", G.tk[i].name);
                if (o > 90) break;
            }
            UNLOCK();
            if (o) { char u[300]; snprintf(u, sizeof u, "Online: %s", t); snprintf(t, sizeof t, "%s", u); }
            else snprintf(t, sizeof t, "Waiting for someone to join");
        }
        return put(b, n, t);
    }
    if (!strcmp(key, "hdr_txt")) {
        time_t now = time(NULL);
        LOCK();
        if (G.on) { int c = 0; for (int i = 0; i < G.ntok; i++) if (now - G.tk[i].seen <= ONLINE_SECS) c++; snprintf(t, sizeof t, "HOSTING  -  %d online", c); }
        else if (G.cl.state == 2) snprintf(t, sizeof t, "CONNECTED to %s", G.cl.host);
        else if (G.cl.state == 1) snprintf(t, sizeof t, "CONNECTING...");
        else snprintf(t, sizeof t, "OFFLINE");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "note_txt")) {
        LOCK();
        if (G.note[0] && mono() - G.note_t < 10.0) snprintf(t, sizeof t, "%s", G.note);
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "m", ROWS))) {
        LOCK();
        int idx = G.nmsg - ROWS + (k - 1);   /* m<ROWS> is the newest */
        if (chat_visible() && idx >= 0 && idx < G.nmsg) chat_row(t, sizeof t, &G.msgs[idx]);
        else if (!chat_visible() && k == ROWS) snprintf(t, sizeof t, "Open the HOST tab and switch the server on, or open JOIN.");
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "i", ROWS)) || (k = key_idx(key, "o", ROWS))) {
        int in = key[0] == 'i';
        scan_files(0);
        LOCK();
        int idx = (in ? G.in_page : G.out_page) * ROWS + (k - 1), cnt = in ? G.ninfiles : G.nfiles;
        if (idx < cnt) file_row(t, sizeof t, in ? &G.infiles[idx] : &G.files[idx], idx == (in ? G.in_sel : G.out_sel));
        else if (k == 1 && cnt == 0) snprintf(t, sizeof t, in ? "No files received yet" : "Outbox is empty");
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "b", ROWS))) {
        LOCK();
        int idx = G.bpage * ROWS + (k - 1);
        if (idx < G.nbent) {
            char nm[100], sz[24] = "";
            disp_text(nm, sizeof nm, G.bent[idx].name, 44);
            if (!G.bent[idx].dir) fmt_size(G.bent[idx].size, sz, sizeof sz);
            snprintf(t, sizeof t, "%s%s%s   %s", idx == G.bsel ? "> " : "   ", G.bent[idx].dir ? "[+] " : "", nm, sz);
        } else if (k == 1 && G.nbent == 0) snprintf(t, sizeof t, "(empty folder)");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "in_page_txt") || !strcmp(key, "out_page_txt") || !strcmp(key, "b_page_txt")) {
        int w = key[0] == 'i' ? 0 : key[0] == 'o' ? 1 : 2;
        LOCK();
        int cnt = w == 0 ? G.ninfiles : w == 1 ? G.nfiles : G.nbent, pg = w == 0 ? G.in_page : w == 1 ? G.out_page : G.bpage;
        if (cnt) snprintf(t, sizeof t, "%d-%d of %d", pg * ROWS + 1, (pg + 1) * ROWS < cnt ? (pg + 1) * ROWS : cnt, cnt);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "in_sel_txt") || !strcmp(key, "out_sel_txt")) {
        int in = key[0] == 'i';
        LOCK();
        int sel = in ? G.in_sel : G.out_sel, cnt = in ? G.ninfiles : G.nfiles;
        if (sel >= 0 && sel < cnt) { char nm[110]; disp_text(nm, sizeof nm, in ? G.infiles[sel].name : G.files[sel].name, 40); snprintf(t, sizeof t, "%s: %s", in ? "Selected" : "Will send", nm); }
        else snprintf(t, sizeof t, in ? "Tap a file" : "Tap the file to send");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "b_sel_txt")) {
        LOCK();
        if (G.bsel >= 0 && G.bsel < G.nbent) { char nm[110]; disp_text(nm, sizeof nm, G.bent[G.bsel].name, 40); snprintf(t, sizeof t, "Selected: %s", nm); }
        else snprintf(t, sizeof t, "Tap a folder to open it, a file to select it");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "b_path_txt")) {
        LOCK();
        char p[400];
        disp_text(p, sizeof p, G.bpath, 70);
        snprintf(t, sizeof t, "%s", p);
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "pn", NPADS))) {
        pthread_mutex_lock(&G.padmu);
        char pn[40];
        disp_text(pn, sizeof pn, G.pad[k - 1].d ? G.pad[k - 1].name : "-", 11);
        snprintf(t, sizeof t, "%d%s %s", k, k - 1 == G.pad_sel ? " >" : "", pn);
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
    if (!strcmp(key, "last_txt")) {
        LOCK();
        if (G.last_note < 0) snprintf(t, sizeof t, "Last MIDI note: none yet (hit a pad)");
        else if (G.last_pad >= 0) snprintf(t, sizeof t, "Last MIDI note: %d = pad %d", G.last_note, G.last_pad + 1);
        else snprintf(t, sizeof t, "Last MIDI note: %d (no pad: use LEARN)", G.last_note);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "snd_pad_txt")) {
        LOCK(); int p = G.pad_sel; UNLOCK();
        pthread_mutex_lock(&G.padmu);
        if (G.pad[p].d) snprintf(t, sizeof t, "PAD %d:  %s  (%.1f s)", p + 1, G.pad[p].name, G.pad[p].frames / 44100.0);
        else snprintf(t, sizeof t, "PAD %d:  empty", p + 1);
        pthread_mutex_unlock(&G.padmu);
        return put(b, n, t);
    }
    if (!strcmp(key, "att_txt") || !strcmp(key, "dec_txt") || !strcmp(key, "rel_txt") || !strcmp(key, "grp_txt") || !strcmp(key, "mode_txt") || !strcmp(key, "st_txt") ||
        !strcmp(key, "en_txt") || !strcmp(key, "pi_txt") || !strcmp(key, "tp_txt") || !strcmp(key, "sx_txt")) {
        LOCK(); int p = G.pad_sel; UNLOCK();
        pthread_mutex_lock(&G.padmu);
        int a = G.pc[p].att, d = G.pc[p].dec, rl = G.pc[p].rel, g = G.pc[p].grp, md = G.pc[p].mode, st = G.pc[p].start, en = G.pc[p].end, pt = G.pc[p].pitch, tp = G.pc[p].tempo, sx = G.pc[p].stretch;
        pthread_mutex_unlock(&G.padmu);
        if (!strcmp(key, "att_txt")) snprintf(t, sizeof t, "%d ms", ATT_MS[a]);
        else if (!strcmp(key, "dec_txt")) { if (DEC_MS[d]) snprintf(t, sizeof t, "%d ms", DEC_MS[d]); else snprintf(t, sizeof t, "OFF"); }
        else if (!strcmp(key, "rel_txt")) snprintf(t, sizeof t, "%d ms", REL_MS[rl]);
        else if (!strcmp(key, "grp_txt")) { if (g) snprintf(t, sizeof t, "GROUP %d", g); else snprintf(t, sizeof t, "NONE"); }
        else if (!strcmp(key, "mode_txt")) snprintf(t, sizeof t, "%s", md ? "HOLD" : "ONE SHOT");
        else if (!strcmp(key, "st_txt")) snprintf(t, sizeof t, "%d %%", st * 2);
        else if (!strcmp(key, "en_txt")) snprintf(t, sizeof t, "%d %%", en * 2);
        else if (!strcmp(key, "pi_txt")) snprintf(t, sizeof t, "%+d st", pt - 12);
        else if (!strcmp(key, "tp_txt")) snprintf(t, sizeof t, "%d %%", TEMPO_PCT[tp]);
        else snprintf(t, sizeof t, "%s", sx ? "KEEP LENGTH" : "VINYL");
        return put(b, n, t);
    }
    if (!strcmp(key, "dir_txt")) { LOCK(); disp_text(t, sizeof t, G.base, 60); UNLOCK(); return put(b, n, t); }
    if (!strcmp(key, "in_pad_txt")) { LOCK(); snprintf(t, sizeof t, "TO PAD %d", G.pad_sel + 1); UNLOCK(); return put(b, n, t); }
    if (!strcmp(key, "pic_txt")) {
        LOCK();
        if (G.thumb_rev) { char f[110], w[40]; disp_text(f, sizeof f, G.thumb_file, 30); disp_text(w, sizeof w, G.thumb_who, 14); snprintf(t, sizeof t, "%s: %s", w, f); }
        else snprintf(t, sizeof t, "Pictures from the phone show up here");
        UNLOCK();
        return put(b, n, t);
    }
    if ((k = key_idx(key, "p", NPEERS))) {
        int slot = k - 1;
        LOCK();
        if (slot < G.npeers) { char nm[30]; disp_text(nm, sizeof nm, G.peers[slot].name, 16); snprintf(t, sizeof t, "%s%s   %s", slot == G.peer_sel ? "> " : "   ", nm, G.peers[slot].ip); }
        else if (slot == 0) snprintf(t, sizeof t, G.scan_alive ? "Searching...   (SERVER on on the other MPC)" : "Switch SEARCH on");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "jcode_txt")) {
        LOCK();
        char d[8];
        for (int i = 0; i < 6; i++) d[i] = (size_t)i < strlen(G.jcode) ? G.jcode[i] : '_';
        d[6] = 0;
        snprintf(t, sizeof t, "%.3s %.3s", d, d + 3);
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "cl_btn_txt")) {
        LOCK();
        snprintf(t, sizeof t, "%s", G.cl.state == 2 ? "LEAVE" : G.cl.state == 1 ? "CANCEL" : "CONNECT");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "cl_status")) {
        LOCK();
        if (G.on) snprintf(t, sizeof t, "Hosting: switch the server off to join.");
        else if (G.cl.state == 1) snprintf(t, sizeof t, "Connecting to %s...", G.cl.host);
        else if (G.cl.state == 2) snprintf(t, sizeof t, "Connected to %s", G.cl.host);
        else if (G.cl.state == 3) snprintf(t, sizeof t, "%s", G.cl.err);
        else if (G.peer_sel < 0) snprintf(t, sizeof t, "1. Switch SEARCH on, tap a host");
        else if (strlen(G.jcode) < 6) snprintf(t, sizeof t, "2. Type the host's code");
        else snprintf(t, sizeof t, "3. Press CONNECT");
        UNLOCK();
        return put(b, n, t);
    }
    if (!strcmp(key, "name_txt")) { ip_refresh(); LOCK(); snprintf(t, sizeof t, "This MPC is  %s", G.devname); UNLOCK(); return put(b, n, t); }
    if (key[0] == 'q' && isdigit((unsigned char)key[1])) {
        int i = atoi(key + 1);
        if (i < 0 || i >= NMETER) return 0;
        LOCK();
        qr_refresh();
        double v = meter_value(i);
        UNLOCK();
        return snprintf(b, (size_t)n, "%.9f", v);
    }
    if (key[0] == 't' && isdigit((unsigned char)key[1])) {   /* tile i of the picture preview */
        int i = atoi(key + 1);
        if (i < 0 || i >= NTILE) return 0;
        int f = G.thumb_rev ? G.thumb[i] : 0;   /* read without the lock: one byte, and the wrapper asks for every tile at once */
        return snprintf(b, (size_t)n, "%.9f", frame_value(f, TILE_FRAMES));
    }
    if (!strcmp(key, "display_rev")) {
        /* changes whenever anything that is on screen may have changed, so MPC redraws it */
        ip_refresh();
        scan_files(0);
        time_t now = time(NULL);
        pthread_mutex_lock(&G.padmu);
        unsigned padh = 0;
        for (int i = 0; i < NPADS; i++) padh = padh * 31u + (G.pad[i].d ? (unsigned)G.pad[i].frames : 0u) + G.pc[i].att * 3u + G.pc[i].dec * 5u + G.pc[i].rel * 7u + G.pc[i].grp * 11u + G.pc[i].mode * 13u + G.pc[i].start * 17u + G.pc[i].end * 19u + G.pc[i].pitch * 23u + G.pc[i].tempo * 29u + G.pc[i].stretch * 31u;
        pthread_mutex_unlock(&G.padmu);
        LOCK();
        int online = 0;
        for (int i = 0; i < G.ntok; i++) if (now - G.tk[i].seen <= ONLINE_SECS) online++;
        char sn[1500];
        snprintf(sn, sizeof sn, "%d|%s|%s|%d|%u|%d|%d|%d|%d|%d|%d|%d|%d|%s|%d|%d|%s|%d|%d|%s|%d|%u|%u|%d|%s|%d|%d|%d|%s|%d", G.on, G.pin, G.ip, online, G.next_id,
                 G.nmsg, G.nfiles, G.ninfiles, G.in_page, G.in_sel, G.out_page, G.out_sel, G.newin, G.jcode, G.npeers, G.peer_sel,
                 G.cl.online, G.cl.state, (int)G.cl.last, G.cl.err, s->quick, padh, G.thumb_rev, G.pad_sel, G.bpath, G.nbent, G.bpage, G.bsel, G.note,
                 G.scan_alive);
        for (int i = G.nmsg > ROWS ? G.nmsg - ROWS : 0; i < G.nmsg; i++) {   /* a row may change its link while a file arrives */
            size_t l = strlen(sn);
            snprintf(sn + l, sizeof sn - l, "|%c", G.msgs[i].box ? G.msgs[i].box : '-');
        }
        {
            size_t l = strlen(sn);
            unsigned fm = 0;
            for (int k = 0; k < 128; k++) fm = fm * 3u + (unsigned)(G.padmap[k] + 1);
            snprintf(sn + l, sizeof sn - l, "|%d|%d|%u|%u|%d|%d|%u", s->obrowse, G.learn, fm, G.rev_pad, G.last_note, G.last_pad, G.epoch);
        }
        UNLOCK();
        uint32_t h = 2166136261u;
        for (const unsigned char *p = (const unsigned char *)sn; *p; p++) { h ^= *p; h *= 16777619u; }
        return snprintf(b, (size_t)n, "%08x", h);
    }
    return 0;
}

/* the signal tones, and the pads: a sine with a soft attack and an exponential decay per note; the pads mix their samples */
static void render(void *inst, int16_t *out, int frames) {
    inst_t *s = inst;
    int r = atomic_exchange(&G.snd_req, 0);
    if (r >= 1 && r <= 4) { s->seq = SEQ[r - 1]; s->ni = 0; s->pos = 0; s->ph = 0; }
    for (int i = 0; i < frames; i++) {
        float v = 0;
        if (s->seq) {
            const note_t *nt = &s->seq[s->ni];
            if (nt->ms == 0) s->seq = NULL;
            else {
                long len = (long)nt->ms * 44100 / 1000;
                float t = (float)s->pos / (float)len;
                float env = (s->pos < 220 ? (float)s->pos / 220.0f : 1.0f) * expf(-3.5f * t);
                v = (sinf((float)s->ph) + 0.25f * sinf(2.0f * (float)s->ph)) * env * 0.22f;
                s->ph += 6.283185307 * nt->f / 44100.0;
                if (s->ph > 6.283185307) s->ph -= 6.283185307;
                if (++s->pos >= len) { s->ni++; s->pos = 0; }
            }
        }
        int16_t x = (int16_t)(v * 32767.0f);
        out[2 * i] = out[2 * i + 1] = x;
    }
    if (pthread_mutex_trylock(&G.padmu) == 0) {   /* never wait on the audio thread: a busy lock skips one block */
        int stopreq = atomic_exchange(&s->stop, 0);
        enum { GL = 1024, HS = 256 };
        static float win[GL]; static int win_ok;
        if (!win_ok) { for (int k = 0; k < GL; k++) win[k] = 0.5f - 0.5f * cosf(6.2831853f * (float)k / GL); win_ok = 1; }
        for (int p = 0; p < NPADS; p++) {
            int vel = atomic_exchange(&s->trig[p], 0);
            const int16_t *d = G.pad[p].d;
            long total = G.pad[p].frames;
            long s0 = (long)((double)total * G.pc[p].start * 2 / 100.0), s1 = (long)((double)total * G.pc[p].end * 2 / 100.0);
            if (s1 > total) s1 = total;
            if (s1 < s0 + 64) s1 = s0 + 64 < total ? s0 + 64 : total;
            if (vel && d) {
                int g = G.pc[p].grp;
                if (g) for (int q = 0; q < NPADS; q++) if (q != p && G.pc[q].grp == g && s->voice[q].on && s->voice[q].rage < 0) s->voice[q].rage = 0;   /* mute group: the others let go */
                s->voice[p].on = 1; s->voice[p].pos = (double)s0; s->voice[p].age = 0; s->voice[p].rage = -1; s->voice[p].ngr = 0; s->voice[p].hopc = 0; s->voice[p].gain = (float)vel / 127.0f;
            }
            if (atomic_exchange(&s->rel[p], 0) && G.pc[p].mode == 1 && s->voice[p].on && s->voice[p].rage < 0) s->voice[p].rage = 0;
            if (stopreq && s->voice[p].on && s->voice[p].rage < 0) s->voice[p].rage = 0;
            if (!s->voice[p].on || !d) continue;
            double pos = s->voice[p].pos;
            long age = s->voice[p].age, rage = s->voice[p].rage;
            long att = (long)ATT_MS[G.pc[p].att] * 44100 / 1000, dec = (long)DEC_MS[G.pc[p].dec] * 44100 / 1000, rel = (long)REL_MS[G.pc[p].rel] * 44100 / 1000;
            if (rel < 1) rel = 1;
            double P = pow(2.0, (G.pc[p].pitch - 12) / 12.0), T = TEMPO_PCT[G.pc[p].tempo] / 100.0;
            if (!G.pc[p].stretch) T *= P;      /* vinyl: pitch and speed go together */
            int direct = fabs(T - P) < 1e-4;
            float gain = s->voice[p].gain * 0.9f;
            for (int i = 0; i < frames; i++, age++) {
                float e = 1.0f;
                if (att > 0 && age < att) e = (float)age / (float)att;
                if (dec > 0) { long da = age - att; if (da < 0) da = 0; if (da >= dec) { s->voice[p].on = 0; break; } e *= 1.0f - (float)da / (float)dec; }
                if (rage >= 0) { if (rage >= rel) { s->voice[p].on = 0; break; } e *= 1.0f - (float)rage / (float)rel; rage++; }
                float l, rr;
                if (direct) {
                    if (pos >= (double)(s1 - 1)) { s->voice[p].on = 0; break; }
                    long ix = (long)pos; float fr = (float)(pos - ix);
                    l = d[2 * ix] * (1 - fr) + d[2 * ix + 2] * fr; rr = d[2 * ix + 1] * (1 - fr) + d[2 * ix + 3] * fr;
                    pos += P;
                } else {   /* time stretch / pitch shift: overlapping grains read at the pitch, their start follows the tempo */
                    if (pos >= (double)s1 && s->voice[p].ngr == 0) { s->voice[p].on = 0; break; }
                    if (s->voice[p].hopc <= 0) {
                        if (pos < (double)s1 && s->voice[p].ngr < 5) { s->voice[p].gr[s->voice[p].ngr].src = pos; s->voice[p].gr[s->voice[p].ngr].age = 0; s->voice[p].ngr++; }
                        pos += HS * T; s->voice[p].hopc += HS;
                    }
                    s->voice[p].hopc--;
                    l = rr = 0;
                    for (int k = 0; k < s->voice[p].ngr; k++) {
                        double x = s->voice[p].gr[k].src + (double)s->voice[p].gr[k].age * P;
                        long ix = (long)x;
                        if (ix >= 0 && ix < s1 - 1) {
                            float fr = (float)(x - ix), w = win[s->voice[p].gr[k].age] * 0.5f;
                            l += w * (d[2 * ix] * (1 - fr) + d[2 * ix + 2] * fr); rr += w * (d[2 * ix + 1] * (1 - fr) + d[2 * ix + 3] * fr);
                        }
                        s->voice[p].gr[k].age++;
                    }
                    for (int k = 0; k < s->voice[p].ngr; ) { if (s->voice[p].gr[k].age >= GL) { s->voice[p].gr[k] = s->voice[p].gr[s->voice[p].ngr - 1]; s->voice[p].ngr--; } else k++; }
                }
                float g2 = gain * e;
                int a = out[2 * i] + (int)(l * g2), c = out[2 * i + 1] + (int)(rr * g2);
                out[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                out[2 * i + 1] = (int16_t)(c > 32767 ? 32767 : c < -32768 ? -32768 : c);
            }
            s->voice[p].pos = pos; s->voice[p].age = age; s->voice[p].rage = rage;
        }
        pthread_mutex_unlock(&G.padmu);
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
