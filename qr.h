/* Minimal QR Code generator: byte mode, error correction L, version 3 (29 x 29 modules, up to 53 bytes).
 * Written for this plugin (the MPC shows the chat address as a QR code). Fixed size on purpose: the screen
 * shows a fixed grid of tiles. Returns 0 on success, -1 when the text is longer than 53 bytes. */
#include <stdint.h>
#include <string.h>

#define QR_N 29
#define QR_DATA_CW 55
#define QR_ECC_CW 15

static uint8_t qr_exp_[512], qr_log_[256];
static void qr_gf_init_(void) {
    if (qr_exp_[1]) return;
    int x = 1;
    for (int i = 0; i < 255; i++) { qr_exp_[i] = (uint8_t)x; qr_log_[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
    for (int i = 255; i < 512; i++) qr_exp_[i] = qr_exp_[i - 255];
}
static uint8_t qr_mul_(uint8_t a, uint8_t b) { return (a && b) ? qr_exp_[qr_log_[a] + qr_log_[b]] : 0; }

static void qr_rs_(const uint8_t *data, int n, uint8_t *ecc) {   /* ecc[QR_ECC_CW] */
    uint8_t g[QR_ECC_CW + 1] = { 1 };
    for (int i = 0; i < QR_ECC_CW; i++) {   /* multiply by (x - a^i) */
        for (int j = QR_ECC_CW; j > 0; j--) g[j] = (uint8_t)(g[j - 1] ^ qr_mul_(g[j], qr_exp_[i]));
        g[0] = qr_mul_(g[0], qr_exp_[i]);
    }
    uint8_t r[QR_ECC_CW] = { 0 };
    for (int i = 0; i < n; i++) {
        uint8_t f = (uint8_t)(data[i] ^ r[0]);
        memmove(r, r + 1, QR_ECC_CW - 1);
        r[QR_ECC_CW - 1] = 0;
        for (int j = 0; j < QR_ECC_CW; j++) r[j] ^= qr_mul_(g[QR_ECC_CW - 1 - j], f);
    }
    memcpy(ecc, r, QR_ECC_CW);
}

typedef struct { uint8_t m[QR_N][QR_N]; uint8_t fn[QR_N][QR_N]; } qr_t_;

static void qr_set_(qr_t_ *q, int x, int y, int dark, int fn) { q->m[y][x] = (uint8_t)dark; if (fn) q->fn[y][x] = 1; }

static void qr_finder_(qr_t_ *q, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= QR_N || y >= QR_N) continue;
        int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        qr_set_(q, x, y, d != 2 && d != 4, 1);
    }
}

static void qr_format_(qr_t_ *q, int mask) {
    int data = (1 << 3) | mask;   /* error correction L = 01 */
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    for (int i = 0; i <= 5; i++) qr_set_(q, 8, i, (bits >> i) & 1, 1);
    qr_set_(q, 8, 7, (bits >> 6) & 1, 1);
    qr_set_(q, 8, 8, (bits >> 7) & 1, 1);
    qr_set_(q, 7, 8, (bits >> 8) & 1, 1);
    for (int i = 9; i < 15; i++) qr_set_(q, 14 - i, 8, (bits >> i) & 1, 1);
    for (int i = 0; i < 8; i++) qr_set_(q, QR_N - 1 - i, 8, (bits >> i) & 1, 1);
    for (int i = 8; i < 15; i++) qr_set_(q, 8, QR_N - 15 + i, (bits >> i) & 1, 1);
    qr_set_(q, 8, QR_N - 8, 1, 1);   /* the always-dark module */
}

static int qr_mask_bit_(int mask, int x, int y) {
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return (x * y) % 2 + (x * y) % 3 == 0;
    case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
}

static int qr_penalty_(const qr_t_ *q) {
    int p = 0, dark = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < QR_N; a++) {
            int run = 1;
            for (int b = 1; b < QR_N; b++) {
                int c = pass ? q->m[b][a] : q->m[a][b], pc = pass ? q->m[b - 1][a] : q->m[a][b - 1];
                if (c == pc) { run++; if (run == 5) p += 3; else if (run > 5) p++; } else run = 1;
            }
            for (int b = 0; b + 10 < QR_N; b++) {   /* finder-like 1:1:3:1:1 with 4 light on a side */
                static const int A[11] = { 1,0,1,1,1,0,1,0,0,0,0 }, B[11] = { 0,0,0,0,1,0,1,1,1,0,1 };
                int ma = 1, mb = 1;
                for (int k = 0; k < 11; k++) {
                    int c = pass ? q->m[b + k][a] : q->m[a][b + k];
                    if (c != A[k]) ma = 0;
                    if (c != B[k]) mb = 0;
                }
                p += 40 * (ma + mb);
            }
        }
    }
    for (int y = 0; y + 1 < QR_N; y++) for (int x = 0; x + 1 < QR_N; x++) {
        int c = q->m[y][x];
        if (c == q->m[y][x + 1] && c == q->m[y + 1][x] && c == q->m[y + 1][x + 1]) p += 3;
    }
    for (int y = 0; y < QR_N; y++) for (int x = 0; x < QR_N; x++) dark += q->m[y][x];
    int k = abs(dark * 20 - QR_N * QR_N * 10) / (QR_N * QR_N);   /* 5% steps away from 50% */
    return p + k * 10;
}

/* out[y][x] = 1 for a dark module */
static int qr_encode(const char *text, uint8_t out[QR_N][QR_N]) {
    size_t n = strlen(text);
    if (n > 53) return -1;
    qr_gf_init_();
    uint8_t cw[QR_DATA_CW + QR_ECC_CW];
    memset(cw, 0, sizeof cw);
    uint8_t bits[QR_DATA_CW * 8 + 8];
    int nb = 0;
#define PUTB(v, len) do { for (int i_ = (len) - 1; i_ >= 0; i_--) bits[nb++] = (uint8_t)(((v) >> i_) & 1); } while (0)
    PUTB(4, 4);
    PUTB((int)n, 8);
    for (size_t i = 0; i < n; i++) PUTB((unsigned char)text[i], 8);
    int cap = QR_DATA_CW * 8;
    for (int i = 0; i < 4 && nb < cap; i++) bits[nb++] = 0;
    while (nb % 8) bits[nb++] = 0;
    for (int i = 0; i < nb; i += 8) { int v = 0; for (int k = 0; k < 8; k++) v = v << 1 | bits[i + k]; cw[i / 8] = (uint8_t)v; }
    for (int i = nb / 8, pad = 0; i < QR_DATA_CW; i++, pad ^= 1) cw[i] = pad ? 0x11 : 0xEC;
    qr_rs_(cw, QR_DATA_CW, cw + QR_DATA_CW);

    qr_t_ q;
    memset(&q, 0, sizeof q);
    qr_finder_(&q, 3, 3); qr_finder_(&q, QR_N - 4, 3); qr_finder_(&q, 3, QR_N - 4);
    for (int i = 8; i < QR_N - 8; i++) { qr_set_(&q, i, 6, i % 2 == 0, 1); qr_set_(&q, 6, i, i % 2 == 0, 1); }
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++)   /* the alignment pattern of version 3 at (22,22) */
        qr_set_(&q, 22 + dx, 22 + dy, (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) != 1, 1);
    qr_format_(&q, 0);   /* reserves the format areas */

    int total = (QR_DATA_CW + QR_ECC_CW) * 8, bi = 0;
    uint8_t base[QR_N][QR_N];
    memset(base, 0, sizeof base);
    for (int right = QR_N - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int v = 0; v < QR_N; v++) for (int j = 0; j < 2; j++) {
            int x = right - j, up = ((right + 1) & 2) == 0, y = up ? QR_N - 1 - v : v;
            if (q.fn[y][x]) continue;
            int bit = bi < total ? (cw[bi >> 3] >> (7 - (bi & 7))) & 1 : 0;
            bi++;
            base[y][x] = (uint8_t)bit;
        }
    }
    int best = -1, bestp = 1 << 30;
    qr_t_ t;
    for (int mask = 0; mask < 8; mask++) {
        t = q;
        for (int y = 0; y < QR_N; y++) for (int x = 0; x < QR_N; x++)
            if (!t.fn[y][x]) t.m[y][x] = (uint8_t)(base[y][x] ^ qr_mask_bit_(mask, x, y));
        qr_format_(&t, mask);
        int p = qr_penalty_(&t);
        if (p < bestp) { bestp = p; best = mask; }
    }
    t = q;
    for (int y = 0; y < QR_N; y++) for (int x = 0; x < QR_N; x++)
        if (!t.fn[y][x]) t.m[y][x] = (uint8_t)(base[y][x] ^ qr_mask_bit_(best, x, y));
    qr_format_(&t, best);
    memcpy(out, t.m, sizeof t.m);
    return 0;
}
