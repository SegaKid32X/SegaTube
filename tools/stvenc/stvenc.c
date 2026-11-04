/*
 * stvenc - the SegaTube movie encoder.
 *
 * Reads raw RGB24 video frames on stdin (already scaled to the picture size
 * by ffmpeg) and raw s16le stereo audio from a file, and writes one profile
 * file (FILM.STV / FULL.STV / I480.STV, see src/codec/stv_format.h).
 *
 * The encoder runs the player's own decoder (src/codec/stv_dec.c) on every
 * picture it writes, into a model of the 32X frame buffers, so what it
 * refers to (SKIP, MOVE) is exactly what the console's buffers will hold.
 *
 *   stvenc --profile full --size 320x224 --pos 0,0 --fps 30000/1001 \
 *          --audio audio.raw --audio-rate 22050 --frames 1234 \
 *          --title "My movie" --out MOVIES/My_movie/FULL < frames.rgb
 *
 * Options: see usage() below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include "stv_dec.h"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define MAX_W           320
#define MAX_H           224
#define MAX_GOP         64
#define MAX_PARTS       2
#define PART_MAX_BYTES  65535       /* each stream length is a u16 */

/* ------------------------------------------------------------ options */

typedef struct {
    int profile;
    int w, h, x, y;                 /* picture (field) size and position */
    uint32_t fps_num, fps_den;
    const char *audio_path;
    uint32_t audio_rate;
    uint32_t frames;                /* frames to encode (0: until stdin ends) */
    uint32_t max_frames;            /* bound for the index reservation */
    const char *title;
    const char *out;                /* path without extension */
    double quality;                 /* 1 (small) .. 10 (best) */
    uint32_t rate_cap;              /* bytes per second the SD stream can sustain */
    uint32_t max_record;            /* largest record the player buffers */
    int temporal_fields;            /* I480: two input frames per record */
    int gop;                        /* frames per chunk (max) */
    int verbose;
    double cost_budget;             /* decode cycles per picture per CPU */
} options;

static options opt;

static void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "stvenc: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(1);
}

/* ------------------------------------------------------------ byte output */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} bytes;

static void need(bytes *b, size_t extra)
{
    if (b->n + extra <= b->cap) return;
    size_t c = b->cap ? b->cap * 2 : 4096;
    while (c < b->n + extra) c *= 2;
    b->p = realloc(b->p, c);
    if (!b->p) die("%s", "out of memory");
    b->cap = c;
}
static void put8(bytes *b, unsigned v) { need(b, 1); b->p[b->n++] = (uint8_t)v; }
static void put16(bytes *b, unsigned v) { put8(b, v >> 8); put8(b, v); }
static void put32(bytes *b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
static void putn(bytes *b, const void *src, size_t n) { need(b, n); memcpy(b->p + b->n, src, n); b->n += n; }
static void pad4b(bytes *b) { while (b->n & 3) put8(b, 0); }
static void set16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void set32(uint8_t *p, uint32_t v) { set16(p, v >> 16); set16(p + 2, v & 0xFFFF); }

/* ------------------------------------------------------------ colour */

/* Error weights (R, G, B): green matters most to the eye, blue least. */
#define WR 3
#define WG 4
#define WB 2

typedef struct { uint8_t r, g, b; } rgb;

static inline int cerr(int r0, int g0, int b0, int r1, int g1, int b1)
{
    int dr = r0 - r1, dg = g0 - g1, db = b0 - b1;
    return WR * dr * dr + WG * dg * dg + WB * db * db;
}

/* 32X colour: BGR555, R in bits 0-4 */
static inline uint16_t to555(int r, int g, int b)
{
    int r5 = (r * 31 + 127) / 255, g5 = (g * 31 + 127) / 255, b5 = (b * 31 + 127) / 255;
    return (uint16_t)(b5 << 10 | g5 << 5 | r5);
}
static inline rgb from555(unsigned c)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    rgb o = { (uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 3 | g >> 2), (uint8_t)(b << 3 | b >> 2) };
    return o;
}

/* ------------------------------------------------------------ palette (P8) */

typedef struct {
    uint16_t c555[256];
    rgb      c[256];
    uint8_t  lut[32768];            /* BGR555 -> nearest palette index */
    int      valid;
} palette;

struct histogram_s;
static double palette_error(const palette *p, const struct histogram_s *h);
static void build_lut(palette *p, int count)
{
    for (int c = 0; c < 32768; c++) {
        rgb q = from555((unsigned)c);
        int best = 0x7FFFFFFF, bi = 0;
        for (int i = 0; i < count; i++) {
            int e = cerr(q.r, q.g, q.b, p->c[i].r, p->c[i].g, p->c[i].b);
            if (e < best) { best = e; bi = i; if (!e) break; }
        }
        p->lut[c] = (uint8_t)bi;
    }
}

/* A histogram of BGR555 colours: what the palette is made from. */
typedef struct histogram_s {
    uint32_t count[32768];
    uint16_t uniq[32768];       /* the colours present */
    uint32_t n;
    double total;
} histogram;

static void hist_finish(histogram *h)
{
    h->n = 0;
    h->total = 0;
    for (int c = 0; c < 32768; c++)
        if (h->count[c]) {
            h->uniq[h->n++] = (uint16_t)c;
            h->total += h->count[c];
        }
}

typedef struct {
    int lo, hi;                 /* range of h->uniq */
    double score;               /* how much splitting it helps */
    int axis;
} box;

static const histogram *sort_hist;
static int sort_shift;
static int cmp_axis_c(const void *a, const void *b)
{
    int va = (*(const uint16_t *)a >> sort_shift) & 31, vb = (*(const uint16_t *)b >> sort_shift) & 31;
    return va - vb;
}

static void box_measure(const histogram *h, box *bx)
{
    int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
    double w = 0;
    for (int i = bx->lo; i < bx->hi; i++) {
        unsigned c = h->uniq[i];
        int v[3] = { (int)(c & 31), (int)(c >> 5 & 31), (int)(c >> 10 & 31) };
        for (int k = 0; k < 3; k++) {
            if (v[k] < lo[k]) lo[k] = v[k];
            if (v[k] > hi[k]) hi[k] = v[k];
        }
        w += h->count[c];
    }
    static const int wgt[3] = { WR, WG, WB };
    double best = -1;
    for (int k = 0; k < 3; k++) {
        double s2 = (double)(hi[k] - lo[k]) * (hi[k] - lo[k]) * wgt[k];
        if (s2 > best) { best = s2; bx->axis = k; }
    }
    bx->score = bx->hi - bx->lo < 2 ? -1 : best * sqrt(w);
}

/* Median cut over the histogram (entry 0 stays black), then k-means. */
static void make_palette(palette *p, const histogram *h)
{
    box boxes[256];
    int nb = 1;
    boxes[0].lo = 0;
    boxes[0].hi = (int)h->n;
    box_measure(h, &boxes[0]);
    uint16_t *u = (uint16_t *)h->uniq;
    while (nb < 255) {
        int bi = -1;
        double best = 0;
        for (int i = 0; i < nb; i++)
            if (boxes[i].score > best) { best = boxes[i].score; bi = i; }
        if (bi < 0) break;
        box *bx = &boxes[bi];
        sort_hist = h;
        sort_shift = bx->axis * 5;
        qsort(u + bx->lo, (size_t)(bx->hi - bx->lo), 2, cmp_axis_c);
        /* split at the weighted mean along the axis (a weighted median
         * would peel off one dominant colour at a time and leave the
         * wide rest unsplit) */
        double tot = 0, sum = 0;
        for (int i = bx->lo; i < bx->hi; i++) {
            double c = h->count[u[i]];
            tot += c;
            sum += c * ((u[i] >> sort_shift) & 31);
        }
        double mean = tot > 0 ? sum / tot : 0;
        int cut = bx->lo;
        while (cut < bx->hi && ((u[cut] >> sort_shift) & 31) <= mean) cut++;
        if (cut <= bx->lo || cut >= bx->hi) cut = (bx->lo + bx->hi) / 2;
        boxes[nb].lo = cut;
        boxes[nb].hi = bx->hi;
        bx->hi = cut;
        box_measure(h, bx);
        box_measure(h, &boxes[nb]);
        nb++;
    }
    p->c555[0] = 0;
    p->c[0] = from555(0);
    for (int i = 0; i < nb; i++) {
        double r = 0, g = 0, b = 0, w = 0;
        for (int k = boxes[i].lo; k < boxes[i].hi; k++) {
            rgb q = from555(u[k]);
            double c = h->count[u[k]];
            r += q.r * c; g += q.g * c; b += q.b * c; w += c;
        }
        if (w <= 0) w = 1;
        p->c555[i + 1] = to555((int)(r / w + 0.5), (int)(g / w + 0.5), (int)(b / w + 0.5));
        p->c[i + 1] = from555(p->c555[i + 1]);
    }
    for (int i = nb + 1; i < 256; i++) {
        p->c555[i] = 0;
        p->c[i] = from555(0);
    }
    nb++;                               /* black + the boxes */
#ifdef PALDEBUG
    build_lut(p, nb);
    p->valid = 1;
    fprintf(stderr, "median cut: %d entries, error %.1f\n", nb, palette_error(p, h));
#endif
    /* k-means refinement over the histogram, via the LUT */
    static double acc[256][4];
    for (int it = 0; it < 3; it++) {
        build_lut(p, nb);
        memset(acc, 0, sizeof acc);
        for (uint32_t k = 0; k < h->n; k++) {
            unsigned c = u[k];
            rgb q = from555(c);
            double wgt = h->count[c];
            double *t = acc[p->lut[c]];
            t[0] += q.r * wgt; t[1] += q.g * wgt; t[2] += q.b * wgt; t[3] += wgt;
        }
        for (int i = 1; i < nb; i++) {     /* (entry 0 stays black) */
            if (acc[i][3] <= 0) continue;
            p->c555[i] = to555((int)(acc[i][0] / acc[i][3] + 0.5), (int)(acc[i][1] / acc[i][3] + 0.5),
                               (int)(acc[i][2] / acc[i][3] + 0.5));
            p->c[i] = from555(p->c555[i]);
        }
    }
    build_lut(p, nb);
    p->valid = 1;
}

/* Mean error of the histogram's pixels against a palette */
static double palette_error(const palette *p, const struct histogram_s *h)
{
    double e = 0;
    for (uint32_t k = 0; k < h->n; k++) {
        unsigned c = h->uniq[k];
        rgb q = from555(c), d = p->c[p->lut[c]];
        e += (double)cerr(q.r, q.g, q.b, d.r, d.g, d.b) * h->count[c];
    }
    return h->total > 0 ? e / h->total : 0;
}

/* ------------------------------------------------------------ frame buffer model */

typedef struct {
    int depth, bw, bh, pitch;
    uint8_t *mem;                   /* native pixels, as the decoder writes them */
} fbuf;

static void fb_init(fbuf *f, int depth, int bw, int bh)
{
    f->depth = depth;
    f->bw = bw;
    f->bh = bh;
    f->pitch = bw * 4 * depth;
    f->mem = calloc((size_t)f->pitch * bh * 4 + 16, 1);
    if (!f->mem) die("%s", "out of memory");
}

static inline unsigned fb_px(const fbuf *f, int x, int y)
{
    if (f->depth == STV_DEPTH_P8) return f->mem[y * f->pitch + x];
    return ((const uint16_t *)(const void *)(f->mem + y * f->pitch))[x];
}
static inline void fb_set(fbuf *f, int x, int y, unsigned v)
{
    if (f->depth == STV_DEPTH_P8) f->mem[y * f->pitch + x] = (uint8_t)v;
    else ((uint16_t *)(void *)(f->mem + y * f->pitch))[x] = (uint16_t)v;
}

/* ------------------------------------------------------------ block coding */

typedef struct {
    int mode;
    uint16_t col[4];
    int ncol;
    uint16_t msk[2];
    int nmsk;
    uint16_t px[16];                /* the decoded block (index or BGR555) */
    int8_t dx, dy;
    long err;
    int bytes;
} choice;

/* Per-mode decode cost estimates in SH-2 cycles, P8 and DC (tuned by
 * the benchmark build; see docs/DESIGN.md). */
static const int cost_p8[8] = { 6, 40, 30, 70, 110, 50, 140, 60 };   /* [7]: GRAD */
static const int cost_dc[8] = { 6, 60, 50, 110, 160, 90, 220, 260 };

typedef struct {
    const palette *pal;             /* P8 */
    int depth;
    double lambda;
} coder;

static inline rgb px_rgb(const coder *c, unsigned v)
{
    if (c->depth == STV_DEPTH_P8) return c->pal->c[v];
    return from555(v);
}

static inline unsigned nearest(const coder *c, int r, int g, int b)
{
    unsigned k = to555(r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g, b < 0 ? 0 : b > 255 ? 255 : b);
    return c->depth == STV_DEPTH_P8 ? c->pal->lut[k] : k;
}

/* Plain squared error (the PSNR figures) */
static long block_mse(const coder *c, const rgb *src, const uint16_t *px)
{
    long e = 0;
    for (int i = 0; i < 16; i++) {
        rgb q = px_rgb(c, px[i]);
        e += cerr(src[i].r, src[i].g, src[i].b, q.r, q.g, q.b);
    }
    return e;
}

/* What the encoder minimises: the squared error, plus the error of the four
 * 2x2 averages (what the eye, and a CRT, see of fine dither): a dithered ramp
 * is then as good as it looks, and a flat block on a gradient as bad. */
static long block_err(const coder *c, const rgb *src, const uint16_t *px)
{
    long e = 0;
    int ar[4] = { 0 }, ag[4] = { 0 }, ab[4] = { 0 }, sr[4] = { 0 }, sg[4] = { 0 }, sb[4] = { 0 };
    for (int i = 0; i < 16; i++) {
        rgb q = px_rgb(c, px[i]);
        e += cerr(src[i].r, src[i].g, src[i].b, q.r, q.g, q.b);
        int k = ((i >> 3) & 1) * 2 + ((i >> 1) & 1);
        ar[k] += q.r; ag[k] += q.g; ab[k] += q.b;
        sr[k] += src[i].r; sg[k] += src[i].g; sb[k] += src[i].b;
    }
    long lp = 0;
    for (int k = 0; k < 4; k++)
        lp += cerr(sr[k], sg[k], sb[k], ar[k], ag[k], ab[k]);  /* sums of 4: x16 */
    return e + lp / 4;                  /* (same scale as 4 pixels' error x 4) */
}

/* GRAD (direct colour): corners from a least-squares fit of the bilinear
 * blend; pinv is the pseudo-inverse of the 16 x 4 blend weights. */
static double grad_pinv[4][16];

static void grad_init(void)
{
    double a[16][4], ata[4][8];
    for (int i = 0; i < 16; i++) {
        double wx = (2 * (i & 3) + 1) / 8.0, wy = (2 * (i >> 2) + 1) / 8.0;
        a[i][0] = (1 - wx) * (1 - wy); a[i][1] = wx * (1 - wy);
        a[i][2] = (1 - wx) * wy;       a[i][3] = wx * wy;
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double v = 0;
            for (int i = 0; i < 16; i++) v += a[i][r] * a[i][k];
            ata[r][k] = v;
            ata[r][k + 4] = r == k;
        }
    for (int col = 0; col < 4; col++) {         /* Gauss-Jordan */
        int piv = col;
        for (int r = col + 1; r < 4; r++) if (fabs(ata[r][col]) > fabs(ata[piv][col])) piv = r;
        for (int k = 0; k < 8; k++) { double t = ata[col][k]; ata[col][k] = ata[piv][k]; ata[piv][k] = t; }
        double d = ata[col][col];
        for (int k = 0; k < 8; k++) ata[col][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            double f = ata[r][col];
            for (int k = 0; k < 8; k++) ata[r][k] -= f * ata[col][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 16; i++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += ata[r][k + 4] * a[i][k];
            grad_pinv[r][i] = v;
        }
}

static int clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5); }

/* Assign each pixel the nearest of k colours; returns the error. */
static long assign(const coder *c, const rgb *src, const uint16_t *cols, int k, uint8_t *idx)
{
    rgb cr[4];
    for (int j = 0; j < k; j++) cr[j] = px_rgb(c, cols[j]);
    long e = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0x7FFFFFFF, bj = 0;
        for (int j = 0; j < k; j++) {
            int d = cerr(src[i].r, src[i].g, src[i].b, cr[j].r, cr[j].g, cr[j].b);
            if (d < best) { best = d; bj = j; }
        }
        idx[i] = (uint8_t)bj;
        e += best;
    }
    return e;
}

/* k-means (k = 2 or 4) on the block's 16 pixels, centroids snapped to what
 * the target can show; returns the error, fills cols and idx. */
static long kmeans(const coder *c, const rgb *src, int k, uint16_t *cols, uint8_t *idx)
{
    /* seed: split along the brightest/darkest pixels */
    int lo = 0, hi = 0, ll = 1 << 30, lh = -1;
    for (int i = 0; i < 16; i++) {
        int l = 3 * src[i].r + 6 * src[i].g + src[i].b;
        if (l < ll) { ll = l; lo = i; }
        if (l > lh) { lh = l; hi = i; }
    }
    float cen[4][3];
    cen[0][0] = src[lo].r; cen[0][1] = src[lo].g; cen[0][2] = src[lo].b;
    cen[1][0] = src[hi].r; cen[1][1] = src[hi].g; cen[1][2] = src[hi].b;
    if (k == 4) {
        for (int a = 0; a < 3; a++) {
            cen[2][a] = cen[0][a] * 2.0f / 3 + cen[1][a] / 3;
            cen[3][a] = cen[0][a] / 3 + cen[1][a] * 2.0f / 3;
        }
    }
    uint8_t lab[16];
    for (int it = 0; it < 3; it++) {
        for (int i = 0; i < 16; i++) {
            float best = 1e30f;
            int bj = 0;
            for (int j = 0; j < k; j++) {
                float dr = src[i].r - cen[j][0], dg = src[i].g - cen[j][1], db = src[i].b - cen[j][2];
                float d = WR * dr * dr + WG * dg * dg + WB * db * db;
                if (d < best) { best = d; bj = j; }
            }
            lab[i] = (uint8_t)bj;
        }
        float s[4][4] = { { 0 } };
        for (int i = 0; i < 16; i++) {
            s[lab[i]][0] += src[i].r; s[lab[i]][1] += src[i].g; s[lab[i]][2] += src[i].b; s[lab[i]][3]++;
        }
        for (int j = 0; j < k; j++)
            if (s[j][3] > 0)
                for (int a = 0; a < 3; a++) cen[j][a] = s[j][a] / s[j][3];
    }
    for (int j = 0; j < k; j++)
        cols[j] = (uint16_t)nearest(c, (int)(cen[j][0] + 0.5), (int)(cen[j][1] + 0.5), (int)(cen[j][2] + 0.5));
    return assign(c, src, cols, k, idx);
}

static void read_block(const fbuf *f, int bx, int by, uint16_t *px)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            px[y * 4 + x] = (uint16_t)fb_px(f, bx * 4 + x, by * 4 + y);
}

/* Choose how to code one block. ref: the buffer as the decoder has it at
 * this point (for SKIP and MOVE); intra: no references allowed. */
static void choose(const coder *c, const rgb *src, const fbuf *ref, int bx, int by,
                   int intra, int prow0, int prow1, const int8_t (*mvc)[2], int nmvc, choice *out)
{
    const int dc = c->depth == STV_DEPTH_DC;
    /* smooth blocks show blocking most: spend more there (lower lambda),
     * less in busy texture, which hides it */
    double act = 0;
    {
        int l[16], mean = 0;
        for (int i = 0; i < 16; i++) { l[i] = 3 * src[i].r + 6 * src[i].g + src[i].b; mean += l[i]; }
        mean /= 16;
        for (int i = 0; i < 16; i++) act += abs(l[i] - mean);
        act /= 160.0;                   /* mean deviation, 0..255 luma */
    }
    const double lam = c->lambda * (0.3 + 1.0 * (act > 16 ? 1.0 : act / 16.0));
    double bestj = 1e300;
    choice t;

#define CONSIDER() do { double j = (double)t.err + lam * t.bytes; if (j < bestj) { bestj = j; *out = t; } } while (0)

    if (!intra) {
        memset(&t, 0, sizeof t);
        t.mode = STV_OP_SKIP;
        read_block(ref, bx, by, t.px);
        t.err = block_err(c, src, t.px);
        t.bytes = 0;
        CONSIDER();
        if (t.err == 0)
            return;
        /* MOVE: a few candidate vectors (neighbours, the frame's guesses) */
        for (int m = 0; m < nmvc; m++) {
            int dx = mvc[m][0], dy = mvc[m][1];
            if (!dx && !dy) continue;
            int sx = bx * 4 + dx, sy = by * 4 + dy;
            if (sx < 0 || sx > ref->bw * 4 - 4 || sy < prow0 * 4 || sy > prow1 * 4 - 4) continue;
            memset(&t, 0, sizeof t);
            t.mode = STV_OP_MOVE;
            t.dx = (int8_t)dx; t.dy = (int8_t)dy;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    t.px[y * 4 + x] = (uint16_t)fb_px(ref, sx + x, sy + y);
            t.err = block_err(c, src, t.px);
            t.bytes = 2;
            CONSIDER();
        }
    }
    /* FILL */
    {
        int r = 0, g = 0, b = 0;
        for (int i = 0; i < 16; i++) { r += src[i].r; g += src[i].g; b += src[i].b; }
        memset(&t, 0, sizeof t);
        t.mode = STV_OP_FILL;
        t.col[0] = (uint16_t)nearest(c, (r + 8) / 16, (g + 8) / 16, (b + 8) / 16);
        t.ncol = 1;
        for (int i = 0; i < 16; i++) t.px[i] = t.col[0];
        t.err = block_err(c, src, t.px);
        t.bytes = dc ? 2 : 1;
        CONSIDER();
        /* the bigger modes cost at least (PAIR - FILL) more bytes: if that
         * much lambda is more than all the error left, none can win */
        if (bestj <= lam * (dc ? 4 : 3)) return;
    }
    /* GRAD: four corner colours, blended (direct colour) or dithered (256) */
    {
        memset(&t, 0, sizeof t);
        t.mode = STV_OP_GRAD;
        t.ncol = 4;
        if (dc) {
            for (int k = 0; k < 4; k++) {
                double r = 0, g = 0, b = 0;
                for (int i = 0; i < 16; i++) {
                    r += grad_pinv[k][i] * src[i].r; g += grad_pinv[k][i] * src[i].g; b += grad_pinv[k][i] * src[i].b;
                }
                t.col[k] = to555(clamp8(r), clamp8(g), clamp8(b));
            }
            stv_grad_dc(t.col, t.px);
            t.bytes = 8;
        } else {
            for (int k = 0; k < 4; k++) {
                int r = 0, g = 0, b = 0;
                for (int i = 0; i < 16; i++)
                    if (stv_grad_pattern[i] == k) { r += src[i].r; g += src[i].g; b += src[i].b; }
                t.col[k] = (uint16_t)nearest(c, (r + 2) / 4, (g + 2) / 4, (b + 2) / 4);
            }
            for (int i = 0; i < 16; i++) t.px[i] = t.col[stv_grad_pattern[i]];
            t.bytes = 4;
        }
        t.err = block_err(c, src, t.px);
        CONSIDER();
    }
    /* PAIR */
    {
        uint8_t idx[16];
        memset(&t, 0, sizeof t);
        t.mode = STV_OP_PAIR;
        (void)kmeans(c, src, 2, t.col, idx);
        if (t.col[0] != t.col[1]) {
            t.ncol = 2;
            unsigned mk = 0;
            for (int i = 0; i < 16; i++) {
                mk = mk << 1 | idx[i];
                t.px[i] = t.col[idx[i]];
            }
            t.err = block_err(c, src, t.px);
            t.msk[0] = (uint16_t)mk;
            t.nmsk = 1;
            t.bytes = dc ? 6 : 4;
            CONSIDER();
        }
    }
    if (bestj <= lam * (dc ? 6 : 4)) return;   /* (QUAD costs that much more again) */
    /* QUAD */
    {
        uint8_t idx[16];
        memset(&t, 0, sizeof t);
        t.mode = STV_OP_QUAD;
        (void)kmeans(c, src, 4, t.col, idx);
        t.ncol = 4;
        uint32_t ix = 0;
        for (int i = 0; i < 16; i++) {
            ix = ix << 2 | idx[i];
            t.px[i] = t.col[idx[i]];
        }
        t.err = block_err(c, src, t.px);
        t.msk[0] = (uint16_t)(ix >> 16);
        t.msk[1] = (uint16_t)(ix & 0xFFFF);
        t.nmsk = 2;
        t.bytes = dc ? 12 : 8;
        CONSIDER();
    }
    /* RAW */
    {
        memset(&t, 0, sizeof t);
        t.mode = STV_OP_RAW;
        for (int i = 0; i < 16; i++) t.px[i] = (uint16_t)nearest(c, src[i].r, src[i].g, src[i].b);
        t.err = block_err(c, src, t.px);
        t.bytes = dc ? 32 : 16;
        CONSIDER();
    }
#undef CONSIDER
}

/* ------------------------------------------------------------ picture coding */

typedef struct {
    bytes ops, col, msk, raw, mv;
    int row0, row1;
    long cost;
} part_out;

typedef struct {
    int mode;
    uint16_t fillcol;
} opsched;

static void emit_run(part_out *p, int mode, int n)
{
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        put8(&p->ops, STV_OP(mode, k));
        n -= k;
    }
}

static void put_col(part_out *p, int dc, unsigned v)
{
    if (dc) put16(&p->col, v);
    else put8(&p->col, v);
}

/* Code block rows [row0, row1) of a picture into p, updating f as the
 * decoder will. src: the picture's RGB pixels (w x h). */
static void code_part(const coder *c, const rgb *src, int w, fbuf *f, int intra, int row0, int row1,
                      part_out *p, long *dist)
{
    const int dc = c->depth == STV_DEPTH_DC;
    const int bw = f->bw;
    const int *cost = dc ? cost_dc : cost_p8;
    int nblocks = (row1 - row0) * bw;
    choice *ch = malloc(sizeof(choice) * (size_t)nblocks);
    int8_t (*vec)[2] = calloc((size_t)nblocks, 2);
    p->row0 = row0;
    p->row1 = row1;
    p->cost = 0;
    long d = 0;

    /* global motion guess: none here; candidates come from neighbours and a
     * small fixed set (pans of a few pixels are what film mostly does) */
    static const int8_t fixed[][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 },
        { 4, 0 }, { -4, 0 }, { 0, 4 }, { 0, -4 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    for (int by = row0; by < row1; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int i = (by - row0) * bw + bx;
            rgb blk[16];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    blk[y * 4 + x] = src[(by * 4 + y) * w + bx * 4 + x];
            int8_t cand[24][2];
            int nc = 0;
            if (!intra) {
                if (bx > 0 && ch[i - 1].mode == STV_OP_MOVE) { cand[nc][0] = vec[i - 1][0]; cand[nc][1] = vec[i - 1][1]; nc++; }
                if (by > row0 && ch[i - bw].mode == STV_OP_MOVE) { cand[nc][0] = vec[i - bw][0]; cand[nc][1] = vec[i - bw][1]; nc++; }
                for (size_t k = 0; k < sizeof fixed / sizeof fixed[0]; k++) {
                    cand[nc][0] = fixed[k][0];
                    cand[nc][1] = fixed[k][1];
                    nc++;
                }
            }
            choose(c, blk, f, bx, by, intra, row0, row1, (const int8_t (*)[2])cand, nc, &ch[i]);
            vec[i][0] = ch[i].dx;
            vec[i][1] = ch[i].dy;
            /* the decoder writes the block now (MOVE reads the current state) */
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    fb_set(f, bx * 4 + x, by * 4 + y, ch[i].px[y * 4 + x]);
            d += block_mse(c, blk, ch[i].px);
            p->cost += cost[ch[i].mode];
        }
    }

    /* pack into ops and streams */
    int i = 0;
    while (i < nblocks) {
        int m = ch[i].mode;
        int j = i + 1;
        if (m == STV_OP_FILL) {
            /* a run of the same colour -> FILLRUN */
            while (j < nblocks && ch[j].mode == STV_OP_FILL && ch[j].col[0] == ch[i].col[0]) j++;
            if (j - i >= 2) {
                int n = j - i;
                while (n > 0) {
                    int k = n > 32 ? 32 : n;
                    put8(&p->ops, STV_OP(STV_OP_FILLRUN, k));
                    put_col(p, dc, ch[i].col[0]);
                    n -= k;
                }
                i = j;
                continue;
            }
            /* single fills, until the next same-colour pair */
            j = i + 1;
            while (j < nblocks && ch[j].mode == STV_OP_FILL &&
                   !(j + 1 < nblocks && ch[j + 1].mode == STV_OP_FILL && ch[j + 1].col[0] == ch[j].col[0]))
                j++;
        } else {
            while (j < nblocks && ch[j].mode == m) j++;
        }
        int n = j - i;
        if (m == STV_OP_SKIP) {
            emit_run(p, m, n);
        } else {
            emit_run(p, m, n);
            for (int k = i; k < j; k++) {
                const choice *q = &ch[k];
                for (int a = 0; a < q->ncol; a++) put_col(p, dc, q->col[a]);
                for (int a = 0; a < q->nmsk; a++) put16(&p->msk, q->msk[a]);
                if (m == STV_OP_RAW) {
                    for (int a = 0; a < 16; a++) {
                        if (dc) put16(&p->raw, q->px[a]);
                        else put8(&p->raw, q->px[a]);
                    }
                }
                if (m == STV_OP_MOVE) {
                    put8(&p->mv, (uint8_t)q->dx);
                    put8(&p->mv, (uint8_t)q->dy);
                }
            }
        }
        i = j;
    }
    free(ch);
    free(vec);
    *dist += d;
}

static void part_free(part_out *p)
{
    free(p->ops.p); free(p->col.p); free(p->msk.p); free(p->raw.p); free(p->mv.p);
    memset(p, 0, sizeof *p);
}

static void write_part(bytes *b, const part_out *p)
{
    put16(b, p->row0); put16(b, p->row1);
    put16(b, (unsigned)p->ops.n); put16(b, (unsigned)p->col.n); put16(b, (unsigned)p->msk.n);
    put16(b, (unsigned)p->raw.n); put16(b, (unsigned)p->mv.n);
    long cost = p->cost / 64;
    put16(b, cost > 65535 ? 65535 : (unsigned)cost);
    putn(b, p->ops.p, p->ops.n); pad4b(b);
    putn(b, p->col.p, p->col.n); pad4b(b);
    putn(b, p->msk.p, p->msk.n); pad4b(b);
    putn(b, p->raw.p, p->raw.n); pad4b(b);
    putn(b, p->mv.p, p->mv.n); pad4b(b);
}

typedef struct {
    double row_cost[MAX_H / 4];     /* last picture's cost per block row */
    int have;
} split_state;

/* Pick the split row so both SH-2s get about the same work. */
static int pick_split(const split_state *s, int bh)
{
    if (!s->have) return bh / 2;
    double total = 0;
    for (int r = 0; r < bh; r++) total += s->row_cost[r];
    double acc = 0;
    for (int r = 0; r < bh; r++) {
        acc += s->row_cost[r];
        if (acc >= total / 2) {
            int sp = r + 1;
            if (sp < 1) sp = 1;
            if (sp > bh - 1) sp = bh - 1;
            return sp;
        }
    }
    return bh / 2;
}

/* Code one picture into out. f is updated to what the decoder will hold.
 * Returns the distortion. Checks the result with the real decoder. */
static long code_picture(const coder *c, const rgb *src, int w, fbuf *f, int intra, int flags,
                         const palette *newpal, split_state *ss, bytes *out, long *cost_max)
{
    const int bh = f->bh;
    int split = pick_split(ss, bh);
    int rows[3] = { 0, split, bh };
    int nparts = bh >= 2 ? 2 : 1;
    if (nparts == 1) rows[1] = bh;

    fbuf before = *f;
    before.mem = malloc((size_t)f->pitch * bh * 4 + 16);
    memcpy(before.mem, f->mem, (size_t)f->pitch * bh * 4);

    part_out parts[MAX_PARTS];
    memset(parts, 0, sizeof parts);
    long dist = 0;
    *cost_max = 0;
    for (int k = 0; k < nparts; k++) {
        code_part(c, src, w, f, intra, rows[k], rows[k + 1], &parts[k], &dist);
        if (parts[k].cost > *cost_max) *cost_max = parts[k].cost;
    }
    /* per-row costs for the next split */
    {
        const int *cost = c->depth == STV_DEPTH_DC ? cost_dc : cost_p8;
        (void)cost;
        for (int r = 0; r < bh; r++) ss->row_cost[r] = 0;
        /* approximate: spread each part's cost over its rows */
        for (int k = 0; k < nparts; k++) {
            int n = rows[k + 1] - rows[k];
            for (int r = rows[k]; r < rows[k + 1]; r++)
                ss->row_cost[r] = (double)parts[k].cost / n;
        }
        ss->have = 1;
    }

    size_t start = out->n;
    put8(out, intra ? STV_PIC_I : STV_PIC_P);
    put8(out, (unsigned)flags | (newpal ? STV_PIC_PALETTE : 0));
    put8(out, (unsigned)nparts);
    put8(out, 0);
    put32(out, 0);                  /* size, set below */
    if (newpal)
        for (int i = 0; i < 256; i++) put16(out, newpal->c555[i]);
    size_t offs = out->n;
    for (int k = 0; k < nparts; k++) put32(out, 0);
    size_t part_at[MAX_PARTS];
    for (int k = 0; k < nparts; k++) {
        part_at[k] = out->n;
        set32(out->p + offs + 4 * k, (uint32_t)(out->n - start));
        write_part(out, &parts[k]);
        if (parts[k].ops.n > PART_MAX_BYTES || parts[k].col.n > PART_MAX_BYTES ||
            parts[k].msk.n > PART_MAX_BYTES || parts[k].raw.n > PART_MAX_BYTES)
            die("%s", "a picture part is too big (lower --quality)");
    }
    set32(out->p + start + 4, (uint32_t)(out->n - start));

    /* the real decoder, on what we wrote, must give the same buffer */
    stv_target t = { before.mem, before.pitch, before.bw, before.bh, before.depth, STV_VERSION };
    for (int k = 0; k < nparts; k++) {
        int e = stv_decode_part(out->p + part_at[k], (uint32_t)(out->n - part_at[k]), &t);
        if (e != STV_OK) {
            fprintf(stderr, "stvenc: decoder rejected part %d (%d)\n", k, e);
            exit(2);
        }
    }
    if (memcmp(before.mem, f->mem, (size_t)f->pitch * bh * 4)) {
        fprintf(stderr, "stvenc: internal error: decoder and encoder disagree\n");
        exit(2);
    }
    free(before.mem);
    for (int k = 0; k < nparts; k++) part_free(&parts[k]);
    return dist;
}

/* ------------------------------------------------------------ audio */

typedef struct {
    FILE *f;
    uint64_t total;                 /* stereo samples in the file */
    stv_adpcm l, r;                 /* the continuous stream's state */
    uint64_t pos;                   /* next sample of the continuous stream */
} audio_in;

static void audio_open(audio_in *a, const char *path)
{
    memset(a, 0, sizeof *a);
    if (!path) return;
    a->f = fopen(path, "rb");
    if (!a->f) die("cannot open audio %s", path);
    fseek(a->f, 0, SEEK_END);
    a->total = (uint64_t)ftell(a->f) / 4;
    fseek(a->f, 0, SEEK_SET);
}

/* Read n stereo samples from position pos (zeros past the end). */
static void audio_read(audio_in *a, uint64_t pos, int n, int16_t *out)
{
    memset(out, 0, (size_t)n * 4);
    if (!a->f || pos >= a->total) return;
    int m = (int)(pos + (uint64_t)n > a->total ? a->total - pos : (uint64_t)n);
    fseek(a->f, (long)(pos * 4), SEEK_SET);
    uint8_t *raw = malloc((size_t)m * 4);
    size_t got = fread(raw, 4, (size_t)m, a->f);
    for (size_t i = 0; i < got * 2; i++)
        out[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    free(raw);
}

static int adpcm_encode(stv_adpcm *s, int sample)
{
    int step = stv_adpcm_steps[s->index];
    int diff = sample - s->predictor, nib = 0;
    if (diff < 0) { nib = 8; diff = -diff; }
    if (diff >= step) { nib |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { nib |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) nib |= 1;
    stv_adpcm_step(s, nib);
    return nib;
}

static void adpcm_fresh(stv_adpcm *s, const int16_t *pcm, int n, int ch)
{
    s->predictor = n ? pcm[ch] : 0;
    int d = 0;
    for (int i = 1; i < n && i < 32; i++) {
        int v = pcm[2 * i + ch] - pcm[2 * (i - 1) + ch];
        d += v < 0 ? -v : v;
    }
    d /= 31;
    int idx = 0;
    while (idx < 88 && stv_adpcm_steps[idx] < d) idx++;
    s->index = idx;
}

static void put_state(bytes *b, const stv_adpcm *s)
{
    put16(b, (unsigned)(s->predictor & 0xFFFF));
    put8(b, (unsigned)s->index);
    put8(b, 0);
}

static void encode_samples(stv_adpcm *l, stv_adpcm *r, const int16_t *pcm, int n, bytes *out)
{
    for (int i = 0; i < n; i++) {
        int a = adpcm_encode(l, pcm[2 * i]);
        int b = adpcm_encode(r, pcm[2 * i + 1]);
        put8(out, (unsigned)(a << 4 | b));
    }
}

static uint64_t sample_of_frame(uint64_t f)
{
    return f * opt.audio_rate * opt.fps_den / opt.fps_num;
}

/* ------------------------------------------------------------ input */

static int read_frame(rgb *dst, int w, int h)
{
    size_t need_n = (size_t)w * h * 3, got = 0;
    uint8_t *p = (uint8_t *)dst;
    while (got < need_n) {
        size_t k = fread(p + got, 1, need_n - got, stdin);
        if (!k) return got ? -1 : 0;
        got += k;
    }
    return 1;
}

/* ------------------------------------------------------------ main loop */

typedef struct {
    rgb *pic[2];                    /* progressive: pic[0]; I480: even, odd field */
    double diff;                    /* scene change measure vs. the previous frame */
} frame;

static double frame_diff(const rgb *a, const rgb *b, int w, int h)
{
    long s = 0, n = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const rgb *p = &a[y * w + x], *q = &b[y * w + x];
            int la = p->r * 3 + p->g * 6 + p->b, lb = q->r * 3 + q->g * 6 + q->b;
            s += la > lb ? la - lb : lb - la;
            n++;
        }
    return n ? (double)s / n / 10.0 : 0;
}

typedef struct {
    frame *gop;
    rgb *inbuf, *prev_full;
    int *have_prev, *eof;
    int w, h, fields;
    size_t pic_px;
} input_ctx;

/* Read one record's pictures into gop[slot]; 0 at the end of the input. */
static int read_input(input_ctx *c, int slot)
{
    if (*c->eof) return 0;
    const int w = c->w, h = c->h;
    frame *fr = &c->gop[slot];
    int ok;
    if (c->fields == 1) {
        ok = read_frame(fr->pic[0], w, h) == 1;
    } else {
        ok = read_frame(c->inbuf, w, 2 * h) == 1;
        if (ok)
            for (int y = 0; y < h; y++) {
                memcpy(fr->pic[0] + y * w, c->inbuf + (2 * y) * w, (size_t)w * sizeof(rgb));
                memcpy(fr->pic[1] + y * w, c->inbuf + (2 * y + 1) * w, (size_t)w * sizeof(rgb));
            }
        if (ok && opt.temporal_fields) {        /* the odd field from the next frame */
            ok = read_frame(c->inbuf, w, 2 * h) == 1;
            if (ok)
                for (int y = 0; y < h; y++)
                    memcpy(fr->pic[1] + y * w, c->inbuf + (2 * y + 1) * w, (size_t)w * sizeof(rgb));
        }
    }
    if (!ok) {
        *c->eof = 1;
        return 0;
    }
    fr->diff = *c->have_prev ? frame_diff(fr->pic[0], c->prev_full, w, h) : 0;
    memcpy(c->prev_full, fr->pic[0], c->pic_px * sizeof(rgb));
    *c->have_prev = 1;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: stvenc --profile film|full|i480 --size WxH --pos X,Y --fps N/D\n"
        "              --out PATH [--audio FILE --audio-rate HZ] [--frames N]\n"
        "              [--title TEXT] [--quality 1-10] [--rate-cap BYTES_PER_S]\n"
        "              [--max-record BYTES] [--temporal-fields] [--gop N] [-v]\n"
        "video: raw RGB24 frames on stdin, WxH (I480: Wx2H, or two WxH-field\n"
        "frames per record with --temporal-fields: Wx2H frames, alternate fields)\n");
    exit(1);
}

static void parse(int argc, char **argv)
{
    memset(&opt, 0, sizeof opt);
    opt.profile = -1;
    opt.fps_num = 30000; opt.fps_den = 1001;
    opt.audio_rate = 22050;
    opt.quality = 6;
    opt.rate_cap = 900000;
    opt.max_record = 48 * 1024;
    opt.title = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && (i++, 1))
        if (ARG("--profile")) {
            opt.profile = !strcmp(v, "film") ? STV_PROF_FILM : !strcmp(v, "full") ? STV_PROF_FULL :
                          !strcmp(v, "i480") ? STV_PROF_I480 : -1;
        } else if (ARG("--size")) {
            if (sscanf(v, "%dx%d", &opt.w, &opt.h) != 2) usage();
        } else if (ARG("--pos")) {
            if (sscanf(v, "%d,%d", &opt.x, &opt.y) != 2) usage();
        } else if (ARG("--fps")) {
            if (sscanf(v, "%u/%u", &opt.fps_num, &opt.fps_den) != 2) usage();
        } else if (ARG("--audio")) opt.audio_path = v;
        else if (ARG("--audio-rate")) opt.audio_rate = (uint32_t)atoi(v);
        else if (ARG("--frames")) opt.frames = (uint32_t)atoi(v);
        else if (ARG("--title")) opt.title = v;
        else if (ARG("--out")) opt.out = v;
        else if (ARG("--quality")) opt.quality = atof(v);
        else if (ARG("--rate-cap")) opt.rate_cap = (uint32_t)atoi(v);
        else if (ARG("--max-record")) opt.max_record = (uint32_t)atoi(v);
        else if (ARG("--gop")) opt.gop = atoi(v);
        else if (!strcmp(a, "--temporal-fields")) opt.temporal_fields = 1;
        else if (!strcmp(a, "-v")) opt.verbose = 1;
        else usage();
#undef ARG
    }
    if (opt.profile < 0 || !opt.out || opt.w <= 0 || opt.h <= 0) usage();
    if (opt.w % 4 || opt.h % 4 || opt.x % 4 || opt.w > MAX_W || opt.h > MAX_H ||
        opt.x + opt.w > MAX_W || opt.y + opt.h > MAX_H)
        die("%s", "picture must be multiples of 4 and fit 320x224 (a field, for i480)");
    if (opt.profile == STV_PROF_FILM && opt.h > STV_DC_MAX_ROWS)
        die("%s", "film pictures are at most 203 rows (direct colour)");
    if (!opt.gop) {
        opt.gop = (int)((opt.fps_num + opt.fps_den / 2) / opt.fps_den);
        if (opt.gop < 8) opt.gop = 8;
    }
    if (opt.gop > MAX_GOP) opt.gop = MAX_GOP;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);    /* raw frames, not text */
#endif
    parse(argc, argv);
    grad_init();
    const int depth = opt.profile == STV_PROF_FILM ? STV_DEPTH_DC : STV_DEPTH_P8;
    const int fields = opt.profile == STV_PROF_I480 ? 2 : 1;
    const int w = opt.w, h = opt.h, bw = w / 4, bh = h / 4;
    const double fps = (double)opt.fps_num / opt.fps_den;
    /* decode budget per picture per SH-2: ~23 MHz, most of a frame (or a
     * field for i480), minus audio, copying and bus contention */
    double pic_time = fields == 2 ? 1.0 / 59.94 : 1.0 / (fps > 30 ? 30 : fps);
    opt.cost_budget = 23.0e6 * pic_time * (fields == 2 ? 0.60 : 0.70);

    int lead = (int)(fps * 0.25 + 0.5);
    if (lead < 2) lead = 2;

    audio_in au;
    audio_open(&au, opt.audio_path);

    /* output: part files */
    char path[4096];
    const char *ext[] = { ".STV", ".ST1", ".ST2", ".ST3", ".ST4", ".ST5", ".ST6", ".ST7", ".ST8", ".ST9" };
    int part = 0;
    snprintf(path, sizeof path, "%s%s", opt.out, ext[0]);
    FILE *out = fopen(path, "wb");
    if (!out) die("cannot write %s", path);

    uint32_t max_frames = opt.frames ? opt.frames : 4000000;
    uint32_t max_chunks = max_frames / 4 + 16;
    uint32_t index_bytes = (max_chunks * 16 + 511) & ~511u;
    uint32_t data_offset = STV_FILE_HEADER + index_bytes;
    {
        uint8_t *z = calloc(1, data_offset);
        fwrite(z, 1, data_offset, out);
        free(z);
    }
    uint64_t part_pos = data_offset;
    stv_index_entry *index = calloc(max_chunks, sizeof *index);
    uint32_t chunks = 0;

    fbuf fb[2];
    fb_init(&fb[0], depth, bw, bh);
    fb_init(&fb[1], depth, bw, bh);
    palette pal_cur, pal_new;
    memset(&pal_cur, 0, sizeof pal_cur);
    split_state ss[2];
    memset(ss, 0, sizeof ss);

    /* input frames: a GOP plus one of lookahead */
    frame *gop = calloc(MAX_GOP + 1, sizeof *gop);
    size_t pic_px = (size_t)w * h;
    for (int i = 0; i <= MAX_GOP; i++)
        for (int k = 0; k < fields; k++)
            gop[i].pic[k] = malloc(pic_px * sizeof(rgb));
    rgb *inbuf = malloc(pic_px * 2 * sizeof(rgb));
    rgb *prev_full = malloc(pic_px * sizeof(rgb));
    int have_prev = 0;

    /* lambda from quality: higher quality, lower lambda */
    double q = opt.quality < 1 ? 1 : opt.quality > 10 ? 10 : opt.quality;
    double lambda0 = 60.0 * pow(2.0, (10.0 - q) * 0.7) * (depth == STV_DEPTH_DC ? 0.6 : 1.0);
    double lambda = lambda0;

    uint32_t frame_no = 0;          /* frames written */
    int pending = 0;                /* frames in gop[] */
    int eof = 0;
    uint64_t total_bytes = 0;
    uint32_t max_record = 0, max_chunk = 0;
    double total_dist = 0;
    long total_px = 0;

    input_ctx in = { gop, inbuf, prev_full, &have_prev, &eof, w, h, fields, pic_px };

    for (;;) {
        while (pending < opt.gop + 1 &&
               (!opt.frames || frame_no + (uint32_t)pending < opt.frames) &&
               read_input(&in, pending))
            pending++;
        if (!pending)
            break;
        if (opt.frames && frame_no + (uint32_t)pending >= opt.frames)
            eof = 1;
        /* the chunk: up to gop frames, ending before a scene cut */
        int n = pending > opt.gop ? opt.gop : pending;
        for (int i = 6; i < n; i++)
            if (gop[i].diff > 28.0) { n = i; break; }

        /* palette for the chunk (P8) */
        const palette *chunk_pal = NULL;
        if (depth == STV_DEPTH_P8) {
            static histogram hist;
            memset(hist.count, 0, sizeof hist.count);
            for (int i = 0; i < n; i++)
                for (int k = 0; k < fields; k++)
                    for (int y = (i & 1); y < h; y += 2)
                        for (int x = (i >> 1 & 1); x < w; x += 2) {
                            const rgb *p = &gop[i].pic[k][y * w + x];
                            hist.count[to555(p->r, p->g, p->b)]++;
                        }
            hist_finish(&hist);
            int reuse = 0;
            make_palette(&pal_new, &hist);
            if (pal_cur.valid) {
                double e_old = palette_error(&pal_cur, &hist);
                double e_new = palette_error(&pal_new, &hist);
                reuse = e_old <= e_new * 1.08 + 2.0;
            }
            if (opt.verbose) fprintf(stderr, "palette: %u colours, error %.1f\n", hist.n, palette_error(&pal_new, &hist));
            if (!reuse)
                pal_cur = pal_new;
            /* every chunk's first picture carries the palette, so a seek or a
             * scan can start there; reuse only keeps colours steady */
            chunk_pal = &pal_cur;
        }

        /* code the chunk; if it is over the rate cap, raise lambda and redo */
        double budget = opt.rate_cap * (n / fps);
        bytes chunk;
        memset(&chunk, 0, sizeof chunk);
        fbuf save[2];
        split_state ss_save[2];
        for (int k = 0; k < 2; k++) {
            save[k] = fb[k];
            save[k].mem = malloc((size_t)fb[k].pitch * bh * 4 + 16);
            memcpy(save[k].mem, fb[k].mem, (size_t)fb[k].pitch * bh * 4);
            ss_save[k] = ss[k];
        }
        stv_adpcm al_save = au.l, ar_save = au.r;
        uint64_t apos_save = au.pos;
        double lam = lambda;
        long chunk_dist = 0;
        uint32_t rec_max = 0;
        for (int attempt = 0; attempt < 8; attempt++) {
            chunk.n = 0;
            chunk_dist = 0;
            rec_max = 0;
            for (int k = 0; k < 2; k++) {
                memcpy(fb[k].mem, save[k].mem, (size_t)fb[k].pitch * bh * 4);
                ss[k] = ss_save[k];
            }
            au.l = al_save; au.r = ar_save; au.pos = apos_save;
            coder cd = { &pal_cur, depth, lam };
            int over_cost = 0;

            put32(&chunk, STV_CHUNK_MAGIC);
            put32(&chunk, chunks);
            put32(&chunk, frame_no);
            put16(&chunk, (unsigned)n);
            put16(&chunk, 0);
            put32(&chunk, 0);               /* size */
            put32(&chunk, 0);               /* first_sample */
            put32(&chunk, 0);
            put32(&chunk, 0);
            for (int i = 0; i < n; i++) {
                uint32_t f = frame_no + (uint32_t)i;
                size_t rec = chunk.n;
                int key = i == 0;
                uint64_t a0 = sample_of_frame(f + (uint32_t)lead), a1 = sample_of_frame(f + (uint32_t)lead + 1);
                int last = eof && i == n - 1 && pending == n;
                if (last && au.total > a0) a1 = au.total;          /* the rest, at the end */
                if (opt.frames && f + 1 == opt.frames && au.total > a1) a1 = au.total;
                if (a1 > a0 + 60000) a1 = a0 + 60000;
                int main_n = au.f ? (int)(a1 - a0) : 0;
                if (au.f && a0 >= au.total) main_n = 0;
                int pre_n = 0;
                uint64_t p0 = sample_of_frame(f);
                if (key && au.f) pre_n = (int)(a0 - p0);
                put32(&chunk, 0);                                  /* size, set below */
                put16(&chunk, (unsigned)main_n);
                put16(&chunk, (unsigned)pre_n);
                put16(&chunk, (unsigned)fields);
                put16(&chunk, (unsigned)((key ? STV_REC_KEY | (au.f ? STV_REC_ADPCM_STATE : 0) : 0) | (last ? STV_REC_LAST : 0)));
                /* the continuous stream must be at a0 here */
                if (au.f && au.pos != a0) {
                    /* the first record of the movie: start the stream fresh */
                    int16_t *tmp = malloc(64 * 4);
                    audio_read(&au, a0, 32, tmp);
                    adpcm_fresh(&au.l, tmp, 32, 0);
                    adpcm_fresh(&au.r, tmp, 32, 1);
                    free(tmp);
                    au.pos = a0;
                }
                if (key && au.f) {
                    put_state(&chunk, &au.l);
                    put_state(&chunk, &au.r);
                }
                if (pre_n) {
                    int16_t *pcm = malloc((size_t)pre_n * 4);
                    audio_read(&au, p0, pre_n, pcm);
                    stv_adpcm pl, pr;
                    adpcm_fresh(&pl, pcm, pre_n, 0);
                    adpcm_fresh(&pr, pcm, pre_n, 1);
                    put_state(&chunk, &pl);
                    put_state(&chunk, &pr);
                    encode_samples(&pl, &pr, pcm, pre_n, &chunk);
                    free(pcm);
                }
                if (main_n) {
                    int16_t *pcm = malloc((size_t)main_n * 4);
                    audio_read(&au, a0, main_n, pcm);
                    encode_samples(&au.l, &au.r, pcm, main_n, &chunk);
                    au.pos = a0 + (uint64_t)main_n;
                    free(pcm);
                }
                pad4b(&chunk);
                /* pictures */
                for (int k = 0; k < fields; k++) {
                    fbuf *target = fields == 2 ? &fb[k] : &fb[f & 1];
                    split_state *s = fields == 2 ? &ss[k] : &ss[f & 1];
                    int intra = fields == 2 ? key : (i < 2);
                    const palette *np = (i == 0 && k == 0) ? chunk_pal : NULL;
                    long cmax = 0;
                    chunk_dist += code_picture(&cd, gop[i].pic[k], w, target, intra,
                                               k ? STV_PIC_FIELD_ODD : 0, np, s, &chunk, &cmax);
                    if (cmax > opt.cost_budget) over_cost = 1;
                }
                uint32_t rsize = (uint32_t)(chunk.n - rec);
                set32(chunk.p + rec, rsize);
                if (rsize > rec_max) rec_max = rsize;
            }
            set32(chunk.p + 16, (uint32_t)chunk.n);
            set32(chunk.p + 20, (uint32_t)sample_of_frame(frame_no));
            if ((chunk.n <= budget && rec_max <= opt.max_record && !over_cost) || attempt == 7)
                break;
            lam *= chunk.n > budget * 1.5 || rec_max > opt.max_record * 1.5 ? 2.0 : 1.4;
        }
        /* drift back towards the quality setting */
        lambda = lam > lambda0 ? lambda0 + (lam - lambda0) * 0.5 : lambda0;
        for (int k = 0; k < 2; k++) free(save[k].mem);

        /* write, starting a new part file before 4 GB */
        if (part_pos + chunk.n > 4000000000ull) {
            fclose(out);
            part++;
            if (part >= 10) die("%s", "movie too long");
            snprintf(path, sizeof path, "%s%s", opt.out, ext[part]);
            out = fopen(path, "wb");
            if (!out) die("cannot write %s", path);
            part_pos = 0;
        }
        if (chunks >= max_chunks) die("%s", "more chunks than --frames allowed for");
        index[chunks].offset = (uint32_t)part_pos;
        index[chunks].size = (uint32_t)chunk.n;
        index[chunks].first_frame = frame_no;
        index[chunks].part = (uint16_t)part;
        index[chunks].frames = (uint16_t)n;
        chunks++;
        fwrite(chunk.p, 1, chunk.n, out);
        part_pos += chunk.n;
        total_bytes += chunk.n;
        if (chunk.n > max_chunk) max_chunk = (uint32_t)chunk.n;
        if (rec_max > max_record) max_record = rec_max;
        total_dist += (double)chunk_dist;
        total_px += (long)n * fields * w * h;
        if (opt.verbose)
            fprintf(stderr, "chunk %u: frames %u-%u, %zu bytes (%.0f KB/s), lambda %.0f, psnr %.2f dB%s\n",
                    chunks - 1, frame_no, frame_no + n - 1, chunk.n, chunk.n / (n / fps) / 1024, lam,
                    10 * log10(255.0 * 255 * (WR + WG + WB) / ((double)chunk_dist / ((long)n * fields * w * h) + 1e-9)),
                    chunk_pal ? " (palette)" : "");
        free(chunk.p);
        frame_no += (uint32_t)n;
        /* keep the frames after the chunk */
        for (int i = n; i < pending; i++) {
            frame t = gop[i - n];
            gop[i - n] = gop[i];
            gop[i] = t;
        }
        pending -= n;
    }

    if (!frame_no) die("%s", "no video frames on stdin");

    /* header and index (in part 0) */
    if (part) {
        fclose(out);
        snprintf(path, sizeof path, "%s%s", opt.out, ext[0]);
        out = fopen(path, "r+b");
        if (!out) die("cannot reopen %s", path);
    }
    bytes hdr;
    memset(&hdr, 0, sizeof hdr);
    put32(&hdr, STV_MAGIC);
    put16(&hdr, STV_VERSION);
    put16(&hdr, (unsigned)opt.profile);
    put16(&hdr, (unsigned)depth);
    put16(&hdr, fields == 2);
    put16(&hdr, (unsigned)w); put16(&hdr, (unsigned)h);
    put16(&hdr, (unsigned)opt.x); put16(&hdr, (unsigned)opt.y);
    put32(&hdr, opt.fps_num); put32(&hdr, opt.fps_den);
    put32(&hdr, au.f ? opt.audio_rate : 0);
    put32(&hdr, frame_no);
    put32(&hdr, chunks);
    put32(&hdr, STV_FILE_HEADER);
    put32(&hdr, data_offset);
    put32(&hdr, max_record);
    put32(&hdr, max_chunk);
    put32(&hdr, (uint32_t)(frame_no / fps * 1000.0));
    put16(&hdr, (unsigned)(part + 1));
    put16(&hdr, (unsigned)lead);
    put32(&hdr, (uint32_t)(total_bytes / (frame_no / fps)));
    {
        char t[64] = { 0 };
        strncpy(t, opt.title, 63);
        putn(&hdr, t, 64);
    }
    while (hdr.n < STV_FILE_HEADER) put8(&hdr, 0);
    for (uint32_t i = 0; i < chunks; i++) {
        put32(&hdr, index[i].offset);
        put32(&hdr, index[i].size);
        put32(&hdr, index[i].first_frame);
        put16(&hdr, index[i].part);
        put16(&hdr, index[i].frames);
    }
    fseek(out, 0, SEEK_SET);
    fwrite(hdr.p, 1, hdr.n, out);
    fclose(out);
    double secs = frame_no / fps;
    fprintf(stderr, "stvenc: %u frames (%.1f s), %u chunks, %.1f MB, %.0f KB/s average, "
            "largest record %u bytes, psnr %.2f dB\n",
            frame_no, secs, chunks, total_bytes / 1048576.0, total_bytes / secs / 1024, max_record,
            10 * log10(255.0 * 255 * (WR + WG + WB) / (total_dist / (double)total_px + 1e-9)));
    return 0;
}
