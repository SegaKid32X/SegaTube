/*
 * stv_dec.c - SegaTube picture and audio decoding.
 *
 * The same file builds for the SH-2 (the player) and for the PC (the
 * encoder, which runs it on its own output to know exactly what each frame
 * buffer will hold, and the host tests). Written for the SH-2: 32-bit stores
 * of whole block rows, lookup tables instead of per-pixel branches, no
 * division, shifts by 2/8/16 where it matters.
 */
#include "stv_dec.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define STV_BIG 1
#else
#define STV_BIG 0
#endif

/* Streams are 4-byte aligned in the file and in the player's buffers. */
static inline uint32_t ld32(const uint8_t *p)
{
#if STV_BIG
    return *(const uint32_t *)(const void *)p;
#else
    return stv_rd32(p);
#endif
}

static inline uint16_t ld16(const uint8_t *p)
{
#if STV_BIG
    return *(const uint16_t *)(const void *)p;
#else
    return stv_rd16(p);
#endif
}

/* A 32-bit store of four P8 pixels a, b, c, d (left to right) */
#if STV_BIG
#define PACK4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define PACK2(a, b)       ((uint32_t)(a) << 16 | (uint32_t)(b))
#else
#define PACK4(a, b, c, d) ((uint32_t)(d) << 24 | (uint32_t)(c) << 16 | (uint32_t)(b) << 8 | (uint32_t)(a))
#define PACK2(a, b)       ((uint32_t)(b) << 16 | (uint32_t)(a))
#endif

/* nibble (bit 3 = leftmost pixel) -> byte mask of the pixels that are set */
#define M(n) PACK4(((n) & 8) ? 0xFF : 0, ((n) & 4) ? 0xFF : 0, ((n) & 2) ? 0xFF : 0, ((n) & 1) ? 0xFF : 0)
static const uint32_t nib_mask[16] = {
    M(0), M(1), M(2), M(3), M(4), M(5), M(6), M(7),
    M(8), M(9), M(10), M(11), M(12), M(13), M(14), M(15)
};
#undef M

typedef struct {
    const uint8_t *ops, *ops_end;
    const uint8_t *col, *col_end;
    const uint8_t *msk, *msk_end;
    const uint8_t *raw, *raw_end;
    const uint8_t *mv, *mv_end;
    int row0, row1;
} streams;

static uint32_t pad4(uint32_t n) { return (n + 3) & ~3u; }

int stv_part_layout_of(const uint8_t *part, uint32_t len, stv_part_layout *out)
{
    if (len < sizeof(stv_part_header))
        return STV_ERR_HEADER;
    uint32_t o = sizeof(stv_part_header);
    out->ops = o; o += pad4(stv_rd16(part + 4));
    out->col = o; o += pad4(stv_rd16(part + 6));
    out->msk = o; o += pad4(stv_rd16(part + 8));
    out->raw = o; o += pad4(stv_rd16(part + 10));
    out->mv  = o; o += pad4(stv_rd16(part + 12));
    out->end = o;
    return o <= len ? STV_OK : STV_ERR_HEADER;
}

static int open_streams(const uint8_t *part, uint32_t len, const stv_target *t, streams *s)
{
    stv_part_layout l;
    if (stv_part_layout_of(part, len, &l) != STV_OK)
        return STV_ERR_HEADER;
    s->row0 = stv_rd16(part);
    s->row1 = stv_rd16(part + 2);
    if (s->row0 >= s->row1 || s->row1 > t->bh)
        return STV_ERR_HEADER;
    s->ops = part + l.ops; s->ops_end = s->ops + stv_rd16(part + 4);
    s->col = part + l.col; s->col_end = s->col + stv_rd16(part + 6);
    s->msk = part + l.msk; s->msk_end = s->msk + stv_rd16(part + 8);
    s->raw = part + l.raw; s->raw_end = s->raw + stv_rd16(part + 10);
    s->mv  = part + l.mv;  s->mv_end  = s->mv + stv_rd16(part + 12);
    return STV_OK;
}

/* The source of a MOVE must be a whole 4x4 inside the part's pixel rows. */
static int move_ok(const streams *s, const stv_target *t, int x, int y, int dx, int dy)
{
    int sx = x + dx, sy = y + dy;
    return sx >= 0 && sx <= t->bw * 4 - 4 && sy >= s->row0 * 4 && sy <= s->row1 * 4 - 4;
}

/* ------------------------------------------------------------ P8 pictures */

static int decode_p8(streams *s, const stv_target *t)
{
    const int pitch = t->pitch, bw = t->bw;
    int blocks = (s->row1 - s->row0) * bw;
    int col = 0, row = s->row0;
    uint8_t *rowbase = t->base + row * 4 * pitch;
    uint8_t *dst = rowbase;
    const uint8_t *ops = s->ops, *c = s->col, *m = s->msk, *r = s->raw, *v = s->mv;

#define NEXT() do { dst += 4; if (++col == bw) { col = 0; row++; rowbase += 4 * pitch; dst = rowbase; } } while (0)
#define ROW(k) (*(uint32_t *)(void *)(dst + (k) * pitch))

    while (blocks > 0) {
        if (ops >= s->ops_end)
            return STV_ERR_DATA;
        unsigned op = *ops++;
        int mode = op >> 5, n = (op & 31) + 1;
        if (mode == STV_OP_SKIPLONG) {
            mode = STV_OP_SKIP;
            n *= 32;
        }
        if (n > blocks)
            return STV_ERR_OPS;
        blocks -= n;
        switch (mode) {
        case STV_OP_SKIP:
            col += n;
            while (col >= bw) {
                col -= bw;
                row++;
                rowbase += 4 * pitch;
            }
            dst = rowbase + col * 4;
            break;
        case STV_OP_FILL:
            if (c + n > s->col_end) return STV_ERR_DATA;
            while (n--) {
                uint32_t w = *c++ * 0x01010101u;
                ROW(0) = w; ROW(1) = w; ROW(2) = w; ROW(3) = w;
                NEXT();
            }
            break;
        case STV_OP_FILLRUN: {
            if (c + 1 > s->col_end) return STV_ERR_DATA;
            uint32_t w = *c++ * 0x01010101u;
            while (n--) {
                ROW(0) = w; ROW(1) = w; ROW(2) = w; ROW(3) = w;
                NEXT();
            }
            break;
        }
        case STV_OP_PAIR:
            if (c + 2 * n > s->col_end || m + 2 * n > s->msk_end) return STV_ERR_DATA;
            while (n--) {
                uint32_t a = c[0] * 0x01010101u, d = (c[0] ^ c[1]) * 0x01010101u;
                unsigned mk = ld16(m);
                c += 2; m += 2;
                ROW(0) = a ^ (d & nib_mask[mk >> 12]);
                ROW(1) = a ^ (d & nib_mask[(mk >> 8) & 15]);
                ROW(2) = a ^ (d & nib_mask[(mk >> 4) & 15]);
                ROW(3) = a ^ (d & nib_mask[mk & 15]);
                NEXT();
            }
            break;
        case STV_OP_QUAD:
            if (c + 4 * n > s->col_end || m + 4 * n > s->msk_end) return STV_ERR_DATA;
            while (n--) {
                const uint8_t *q = c;
                unsigned i0 = ld16(m), i1 = ld16(m + 2);
                c += 4; m += 4;
                ROW(0) = PACK4(q[i0 >> 14], q[(i0 >> 12) & 3], q[(i0 >> 10) & 3], q[(i0 >> 8) & 3]);
                ROW(1) = PACK4(q[(i0 >> 6) & 3], q[(i0 >> 4) & 3], q[(i0 >> 2) & 3], q[i0 & 3]);
                ROW(2) = PACK4(q[i1 >> 14], q[(i1 >> 12) & 3], q[(i1 >> 10) & 3], q[(i1 >> 8) & 3]);
                ROW(3) = PACK4(q[(i1 >> 6) & 3], q[(i1 >> 4) & 3], q[(i1 >> 2) & 3], q[i1 & 3]);
                NEXT();
            }
            break;
        case STV_OP_RAW:
            if (r + 16 * n > s->raw_end) return STV_ERR_DATA;
            while (n--) {
                const uint32_t *w = (const uint32_t *)(const void *)r;  /* byte order kept */
                ROW(0) = w[0]; ROW(1) = w[1]; ROW(2) = w[2]; ROW(3) = w[3];
                r += 16;
                NEXT();
            }
            break;
        case STV_OP_MOVE:
            if (v + 2 * n > s->mv_end) return STV_ERR_DATA;
            while (n--) {
                int dx = (int8_t)v[0], dy = (int8_t)v[1];
                v += 2;
                if (move_ok(s, t, col * 4, row * 4, dx, dy)) {
                    const uint8_t *src = dst + dy * pitch + dx;
                    uint32_t w0, w1, w2, w3;
                    if (((uintptr_t)src & 3) == 0) {
                        w0 = *(const uint32_t *)(const void *)src;
                        w1 = *(const uint32_t *)(const void *)(src + pitch);
                        w2 = *(const uint32_t *)(const void *)(src + 2 * pitch);
                        w3 = *(const uint32_t *)(const void *)(src + 3 * pitch);
                    } else {
                        const uint8_t *p = src;
                        w0 = PACK4(p[0], p[1], p[2], p[3]); p += pitch;
                        w1 = PACK4(p[0], p[1], p[2], p[3]); p += pitch;
                        w2 = PACK4(p[0], p[1], p[2], p[3]); p += pitch;
                        w3 = PACK4(p[0], p[1], p[2], p[3]);
                    }
                    ROW(0) = w0; ROW(1) = w1; ROW(2) = w2; ROW(3) = w3;
                }
                NEXT();
            }
            break;
        }
    }
#undef ROW
#undef NEXT
    return STV_OK;
}

/* ------------------------------------------------------------ DC pictures */

#if STV_BIG
#define RAWPIX(p) (*(const uint32_t *)(const void *)(p))
#else
#define RAWPIX(p) PACK2(stv_rd16(p), stv_rd16((p) + 2))
#endif

static int decode_dc(streams *s, const stv_target *t)
{
    const int pitch = t->pitch, bw = t->bw;
    int blocks = (s->row1 - s->row0) * bw;
    int col = 0, row = s->row0;
    uint8_t *rowbase = t->base + row * 4 * pitch;
    uint8_t *dst = rowbase;
    const uint8_t *ops = s->ops, *c = s->col, *m = s->msk, *r = s->raw, *v = s->mv;

#define NEXT() do { dst += 8; if (++col == bw) { col = 0; row++; rowbase += 4 * pitch; dst = rowbase; } } while (0)
#define W(k, h) (*(uint32_t *)(void *)(dst + (k) * pitch + 4 * (h)))

    while (blocks > 0) {
        if (ops >= s->ops_end)
            return STV_ERR_DATA;
        unsigned op = *ops++;
        int mode = op >> 5, n = (op & 31) + 1;
        if (mode == STV_OP_SKIPLONG) {
            mode = STV_OP_SKIP;
            n *= 32;
        }
        if (n > blocks)
            return STV_ERR_OPS;
        blocks -= n;
        switch (mode) {
        case STV_OP_SKIP:
            col += n;
            while (col >= bw) {
                col -= bw;
                row++;
                rowbase += 4 * pitch;
            }
            dst = rowbase + col * 8;
            break;
        case STV_OP_FILL:
            if (c + 2 * n > s->col_end) return STV_ERR_DATA;
            while (n--) {
                unsigned k = ld16(c);
                uint32_t w = PACK2(k, k);
                c += 2;
                W(0, 0) = w; W(0, 1) = w; W(1, 0) = w; W(1, 1) = w;
                W(2, 0) = w; W(2, 1) = w; W(3, 0) = w; W(3, 1) = w;
                NEXT();
            }
            break;
        case STV_OP_FILLRUN: {
            if (c + 2 > s->col_end) return STV_ERR_DATA;
            unsigned k = ld16(c);
            uint32_t w = PACK2(k, k);
            c += 2;
            while (n--) {
                W(0, 0) = w; W(0, 1) = w; W(1, 0) = w; W(1, 1) = w;
                W(2, 0) = w; W(2, 1) = w; W(3, 0) = w; W(3, 1) = w;
                NEXT();
            }
            break;
        }
        case STV_OP_PAIR:
            if (c + 4 * n > s->col_end || m + 2 * n > s->msk_end) return STV_ERR_DATA;
            while (n--) {
                unsigned k0 = ld16(c), k1 = ld16(c + 2), mk = ld16(m);
                uint32_t q[4];
                q[0] = PACK2(k0, k0); q[1] = PACK2(k0, k1); q[2] = PACK2(k1, k0); q[3] = PACK2(k1, k1);
                c += 4; m += 2;
                W(0, 0) = q[mk >> 14];         W(0, 1) = q[(mk >> 12) & 3];
                W(1, 0) = q[(mk >> 10) & 3];   W(1, 1) = q[(mk >> 8) & 3];
                W(2, 0) = q[(mk >> 6) & 3];    W(2, 1) = q[(mk >> 4) & 3];
                W(3, 0) = q[(mk >> 2) & 3];    W(3, 1) = q[mk & 3];
                NEXT();
            }
            break;
        case STV_OP_QUAD:
            if (c + 8 * n > s->col_end || m + 4 * n > s->msk_end) return STV_ERR_DATA;
            while (n--) {
                unsigned k[4] = { ld16(c), ld16(c + 2), ld16(c + 4), ld16(c + 6) };
                unsigned i0 = ld16(m), i1 = ld16(m + 2);
                c += 8; m += 4;
                W(0, 0) = PACK2(k[i0 >> 14], k[(i0 >> 12) & 3]);
                W(0, 1) = PACK2(k[(i0 >> 10) & 3], k[(i0 >> 8) & 3]);
                W(1, 0) = PACK2(k[(i0 >> 6) & 3], k[(i0 >> 4) & 3]);
                W(1, 1) = PACK2(k[(i0 >> 2) & 3], k[i0 & 3]);
                W(2, 0) = PACK2(k[i1 >> 14], k[(i1 >> 12) & 3]);
                W(2, 1) = PACK2(k[(i1 >> 10) & 3], k[(i1 >> 8) & 3]);
                W(3, 0) = PACK2(k[(i1 >> 6) & 3], k[(i1 >> 4) & 3]);
                W(3, 1) = PACK2(k[(i1 >> 2) & 3], k[i1 & 3]);
                NEXT();
            }
            break;
        case STV_OP_RAW:
            if (r + 32 * n > s->raw_end) return STV_ERR_DATA;
            while (n--) {
                W(0, 0) = RAWPIX(r);      W(0, 1) = RAWPIX(r + 4);
                W(1, 0) = RAWPIX(r + 8);  W(1, 1) = RAWPIX(r + 12);
                W(2, 0) = RAWPIX(r + 16); W(2, 1) = RAWPIX(r + 20);
                W(3, 0) = RAWPIX(r + 24); W(3, 1) = RAWPIX(r + 28);
                r += 32;
                NEXT();
            }
            break;
        case STV_OP_MOVE:
            if (v + 2 * n > s->mv_end) return STV_ERR_DATA;
            while (n--) {
                int dx = (int8_t)v[0], dy = (int8_t)v[1];
                v += 2;
                if (move_ok(s, t, col * 4, row * 4, dx, dy)) {
                    const uint8_t *src = dst + dy * pitch + dx * 2;
                    uint32_t w[8];
                    if (((uintptr_t)src & 3) == 0) {
                        for (int k = 0; k < 4; k++) {
                            const uint32_t *p = (const uint32_t *)(const void *)(src + k * pitch);
                            w[2 * k] = p[0];
                            w[2 * k + 1] = p[1];
                        }
                    } else {
                        for (int k = 0; k < 4; k++) {
                            const uint16_t *p = (const uint16_t *)(const void *)(src + k * pitch);
                            w[2 * k] = PACK2(p[0], p[1]);
                            w[2 * k + 1] = PACK2(p[2], p[3]);
                        }
                    }
                    for (int k = 0; k < 4; k++) {
                        W(k, 0) = w[2 * k];
                        W(k, 1) = w[2 * k + 1];
                    }
                }
                NEXT();
            }
            break;
        }
    }
#undef W
#undef NEXT
    return STV_OK;
}

int stv_decode_part(const uint8_t *part, uint32_t len, const stv_target *t)
{
    streams s;
    int e = open_streams(part, len, t, &s);
    if (e != STV_OK)
        return e;
    return t->depth == STV_DEPTH_DC ? decode_dc(&s, t) : decode_p8(&s, t);
}

/* ------------------------------------------------------------ IMA ADPCM */

const int16_t stv_adpcm_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

const int8_t stv_adpcm_index_adj[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

void stv_adpcm_load(stv_adpcm *s, const uint8_t *state4)
{
    s->predictor = (int16_t)stv_rd16(state4);
    s->index = state4[2] > 88 ? 88 : state4[2];
}

int stv_adpcm_step(stv_adpcm *s, int nib)
{
    int step = stv_adpcm_steps[s->index];
    int diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    int p = s->predictor + ((nib & 8) ? -diff : diff);
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    s->predictor = p;
    int i = s->index + stv_adpcm_index_adj[nib];
    s->index = i < 0 ? 0 : i > 88 ? 88 : i;
    return p;
}

void stv_adpcm_decode(stv_adpcm *l, stv_adpcm *r, const uint8_t *src, int n, int16_t *out)
{
    while (n--) {
        unsigned b = *src++;
        *out++ = (int16_t)stv_adpcm_step(l, b >> 4);
        *out++ = (int16_t)stv_adpcm_step(r, b & 15);
    }
}
