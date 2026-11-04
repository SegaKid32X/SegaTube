/*
 * probe.c - SegaTube's hardware test ROM (make probe -> rom/segatube-probe.32x).
 *
 * Measures what only a real console can tell: whether the Mega EverDrive
 * PRO answers with the 32X attached, how fast it reads the SD card into
 * cartridge memory, whether that data is stable, how long the SH-2s take to
 * decode a picture of your first movie, and that the PWM makes sound.
 * Results go on screen for you to read out (or photograph).
 *
 * Put at least one converted movie in /MOVIES on the SD card first.
 */
#include <string.h>
#include "hw.h"
#include "io.h"
#include "audio.h"
#include "stv_dec.h"
#include "player.h"

#ifdef PROBE

/* ---- vblanks counted here (the 68000's counter stops while it waits on the card) ---- */
static uint32_t vticks;
static int vlast;
static void vpoll(void)
{
    int v = hw_in_vblank();
    if (v && !vlast) vticks++;
    vlast = v;
}

static int cmd_timed(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    md_cmd_start(cmd, p1, p2, p3);
    int r;
    while ((r = md_cmd_done()) < 0) vpoll();
    return r;
}

/* ---- text ---- */
static void puts_at(int row, int col, const char *s, int high)
{
    while (*s) {
        uint16_t pair = (uint16_t)((uint8_t)s[0] << 8 | (s[1] ? (uint8_t)s[1] : ' '));
        md_cmd(CMD_PUTS, (uint16_t)(row << 8 | col), pair, (uint16_t)high);
        col += 2;
        s += s[1] ? 2 : 1;
    }
}

static char *num(char *o, uint32_t v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}

static char *str(char *o, const char *s)
{
    while (*s) *o++ = *s++;
    *o = 0;
    return o;
}

static void line(int row, const char *label, const char *value, int good)
{
    puts_at(row, 1, label, 0);
    puts_at(row, 16, value, good);
}

static void wait_button(void)
{
    for (;;) {
        hw_wait_vblank();
        pad_poll();
        if (pad_pressed & (PAD_A | PAD_START | PAD_B | PAD_C)) return;
    }
}

/* ---- the tests ---- */

static int find_profile(int *kind)
{
    static const int kinds[2] = { KIND_FULL, KIND_FILM };
    for (int i = 0; i < 2; i++)
        if (md_cmd(CMD_OPEN, 0, (uint16_t)kinds[i], 0) == 0) {
            *kind = kinds[i];
            return 1;
        }
    return 0;
}

static uint8_t probe_buf[32768] __attribute__((aligned(16)));

static uint32_t sum32(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i += 4) s = (s << 1 | s >> 31) ^ *(const uint32_t *)(const void *)(p + i);
    return s;
}

static void sd_tests(int *row, int kind)
{
    char t[40];
    movie_t mv;
    int e = movie_open(&mv, 0, kind);
    line(*row += 2, "FILE HEADER", e ? "BAD" : "OK", !e);
    if (e) return;
    /* raw reads: 32 KB pieces into cartridge memory */
    stv_index_entry last;
    movie_index((int)mv.chunks - 1, &last);
    uint32_t file_end = last.part ? 0x7FFFFFFF : last.offset + last.size;
    int pieces = 48;
    if ((uint32_t)pieces * SLOT_BYTES + 4096 > file_end) pieces = (int)((file_end - 4096) / SLOT_BYTES);
    if (pieces < 4) {
        line(*row += 2, "SD SPEED", "MOVIE TOO SHORT", 0);
        return;
    }
    md_cmd(CMD_OPEN, 0, (uint16_t)kind, 0);
    uint32_t t0 = vticks;
    for (int k = 0; k < pieces; k++)
        cmd_timed(CMD_READ, (uint16_t)((4096u + (uint32_t)k * SLOT_BYTES) >> 16),
                  (uint16_t)(4096u + (uint32_t)k * SLOT_BYTES), 0 << 8 | 0);
    uint32_t dt = vticks - t0;
    uint32_t kbs = dt ? (uint32_t)pieces * 32 * 60 / dt : 0;
    char *p = num(t, kbs);
    str(p, " KB/S");
    line(*row += 2, "SD READ", t, kbs >= 900);
    /* read + copy into SDRAM, as the player streams */
    t0 = vticks;
    for (int k = 0; k < pieces; k++) {
        cmd_timed(CMD_READ, (uint16_t)((4096u + (uint32_t)k * SLOT_BYTES) >> 16),
                  (uint16_t)(4096u + (uint32_t)k * SLOT_BYTES), 0 << 8 | 0);
        dma0_start(probe_buf, (const void *)(uintptr_t)(CART_CACHED + CART_RING), SLOT_BYTES);
        while (dma0_busy()) vpoll();
        dma0_wait();
    }
    dt = vticks - t0;
    kbs = dt ? (uint32_t)pieces * 32 * 60 / dt : 0;
    p = num(t, kbs);
    str(p, " KB/S");
    line(*row += 2, "SD TO SDRAM", t, kbs >= 700);
    /* the same piece twice must match */
    uint32_t s1, s2;
    io_read(4096, probe_buf, SLOT_BYTES);
    s1 = sum32(probe_buf, SLOT_BYTES);
    io_read(4096 + SLOT_BYTES, probe_buf, SLOT_BYTES);
    io_read(4096, probe_buf, SLOT_BYTES);
    s2 = sum32(probe_buf, SLOT_BYTES);
    line(*row += 2, "READ AGAIN", s1 == s2 ? "SAME DATA" : "DIFFERENT!", s1 == s2);
}

/* Decode the first picture of chunk 0 again and again: the time per picture */
typedef struct { const uint8_t *part; uint32_t len; stv_target t; } pjob;
static pjob pj_storage __attribute__((aligned(16)));
#define PJ ((pjob *)SDRAM_THROUGH(&pj_storage))
static void slave_part(int a)
{
    (void)a;
    stv_target t = PJ->t;
    stv_decode_part(PJ->part, PJ->len, &t);
}

static void decode_test(int *row, int kind)
{
    char t[40];
    movie_t mv;
    if (movie_open(&mv, 0, kind)) return;
    hw_video_mode(mv.depth, mv.pic_y, mv.pic_h);
    stream_start(0);
    while (stream_avail() < 32 + 4) { stream_poll(); if (stream_error()) return; }
    const uint8_t *c = stream_data() + 32;
    uint32_t rsize = stv_rd32(c);
    while (stream_avail() < 32 + rsize) { stream_poll(); if (stream_error()) return; }
    stream_stop();
    c = stream_data() + 32;
    int pre = stv_rd16(c + 6), mainn = stv_rd16(c + 4), flags = stv_rd16(c + 10);
    const uint8_t *q = c + STV_REC_HEADER + ((flags & STV_REC_ADPCM_STATE) ? 8 : 0) + (pre ? 8 + pre : 0) + mainn;
    q = (const uint8_t *)(((uintptr_t)q + 3) & ~(uintptr_t)3);
    const uint8_t *pic = q, *pq = pic + 8;
    if (pic[1] & STV_PIC_PALETTE) {
        hw_load_palette((const uint16_t *)(const void *)pq);
        pq += 512;
    }
    uint32_t psize = stv_rd32(pic + 4), o0 = stv_rd32(pq), o1 = pic[2] > 1 ? stv_rd32(pq + 4) : 0;
    stv_target tg = { hw_row(0) + mv.pic_x * mv.depth, hw_row_bytes(), mv.pic_w / 4, mv.pic_h / 4, mv.depth, mv.version, 0 };
    const int reps = 30;
    uint32_t t0 = vticks;
    for (int i = 0; i < reps; i++) {
        if (o1) {
            PJ->part = pic + o1; PJ->len = psize - o1; PJ->t = tg;
            hw_slave_start(slave_part, 0);
        }
        stv_decode_part(pic + o0, psize - o0, &tg);
        hw_slave_wait();
        vpoll();
    }
    uint32_t dt = vticks - t0;
    hw_flip();
    /* ms per picture = dt / 60 * 1000 / reps */
    uint32_t tenths = dt * 10000 / 60 / reps;
    char *p = num(t, tenths / 10);
    *p++ = '.';
    p = num(p, tenths % 10);
    str(p, " MS A PICTURE");
    uint32_t budget = (uint32_t)(1000ull * mv.fps_den / mv.fps_num);
    line(*row += 2, "DECODE", t, tenths / 10 < budget * 7 / 10);
}

static void sound_test(int *row)
{
    static int16_t tone[2 * 256];
    audio_init(22050);
    audio_reset();
    for (int i = 0; i < 256; i++) {
        /* 441 Hz: 50 samples a period; a triangle is enough to hear */
        int ph = i % 50, v = ph < 25 ? ph * 2600 - 32500 : (50 - ph) * 2600 - 32500;
        tone[2 * i] = tone[2 * i + 1] = (int16_t)(v / 3);
    }
    audio_pause(0);
    for (int k = 0; k < 86 * 2; k++) {          /* about 2 seconds */
        while (audio_space() < 256) ;
        audio_write(tone, 250);                  /* whole periods */
    }
    line(*row += 2, "SOUND", "A 2 S TONE PLAYED", 1);
    while (audio_queued() > 256) ;
    audio_pause(1);
}

/* ---- decode benchmark: pictures made of one kind of block only, both
 * SH-2s at once (as the player decodes), timed with the free-running timer.
 * The result: SH-2 cycles a block for each op, which the encoder's decode
 * cost model (tools/stvenc/stvenc.c cost_p8 / cost_dc) should match. ---- */
#define FRT_TCR     (*(volatile uint8_t *)0xFFFFFE16)
#define FRT_FTCSR   (*(volatile uint8_t *)0xFFFFFE11)
#define FRT_FRCH    (*(volatile uint8_t *)0xFFFFFE12)
#define FRT_FRCL    (*(volatile uint8_t *)0xFFFFFE13)
static uint16_t frt(void)
{
    uint8_t h = FRT_FRCH;
    return (uint16_t)(h << 8 | FRT_FRCL);
}

static uint32_t pad4u(uint32_t n) { return (n + 3) & ~3u; }

/* A part of block rows [row0, row1), every block op `mode`; returns its size */
static uint32_t bench_part(uint8_t *p, int mode, int depth, int row0, int row1, int bw)
{
    int n = (row1 - row0) * bw, nops = (n + 31) / 32, cs = depth == 2 ? 2 : 1;
    int ncol = 0, nmsk = 0, nraw = 0, nmv = 0;
    switch (mode) {
    case STV_OP_FILL:    ncol = n; break;
    case STV_OP_FILLRUN: ncol = nops; break;
    case STV_OP_PAIR:    ncol = 2 * n; nmsk = n; break;
    case STV_OP_QUAD:    ncol = 4 * n; nmsk = 2 * n; break;
    case STV_OP_RAW:     nraw = 16 * n; break;
    case STV_OP_MOVE:    nmv = n; break;
    }
    uint16_t *h = (uint16_t *)(void *)p;
    h[0] = (uint16_t)row0; h[1] = (uint16_t)row1; h[2] = (uint16_t)nops;
    h[3] = (uint16_t)(ncol * cs); h[4] = (uint16_t)(nmsk * 2); h[5] = (uint16_t)(nraw * cs);
    h[6] = (uint16_t)(nmv * 2); h[7] = 0;
    uint8_t *o = p + 16;
    for (int left = n; left > 0; left -= 32)
        *o++ = STV_OP(mode, left > 32 ? 32 : left);
    o = p + 16 + pad4u((uint32_t)nops);
    uint32_t seed = 12345;
#define RND() (seed = seed * 1103515245u + 12345u, seed >> 16)
    for (int i = 0; i < ncol; i++) {
        if (cs == 2) { *(uint16_t *)(void *)o = (uint16_t)(RND() & 0x7FFF); o += 2; }
        else *o++ = (uint8_t)RND();
    }
    o = (uint8_t *)(((uintptr_t)o + 3) & ~(uintptr_t)3);
    for (int i = 0; i < nmsk; i++) { *(uint16_t *)(void *)o = (uint16_t)RND(); o += 2; }
    o = (uint8_t *)(((uintptr_t)o + 3) & ~(uintptr_t)3);
    for (int i = 0; i < nraw; i++) {
        if (cs == 2) { *(uint16_t *)(void *)o = (uint16_t)(RND() & 0x7FFF); o += 2; }
        else *o++ = (uint8_t)RND();
    }
    o = (uint8_t *)(((uintptr_t)o + 3) & ~(uintptr_t)3);
    for (int i = 0; i < nmv; i++) {         /* the block beside it */
        int dx = (i % bw) ? -4 : 4;
        *(uint16_t *)(void *)o = (uint16_t)((uint8_t)dx << 8);
        o += 2;
    }
    o = (uint8_t *)(((uintptr_t)o + 3) & ~(uintptr_t)3);
#undef RND
    return (uint32_t)(o - p);
}

/* cycles a block per SH-2 for one op */
static uint32_t bench_op(int mode, int depth)
{
    const int bw = 80, rows = 12;
    uint8_t *buf = stream_scratch();
    uint8_t *pa = buf, *pb = buf + 65536;
    uint32_t la = bench_part(pa, mode, depth, 0, rows, bw);
    uint32_t lb = bench_part(pb, mode, depth, rows, 2 * rows, bw);
    hw_cache_purge();
    stv_target tg = { hw_row(0), hw_row_bytes(), bw, 50, depth, 1, 0 };
    const int reps = 4;
    int err = 0;
    uint16_t t0 = frt();
    for (int i = 0; i < reps; i++) {
        PJ->part = pb; PJ->len = lb; PJ->t = tg;
        hw_slave_start(slave_part, 0);
        if (stv_decode_part(pa, la, &tg)) err = 1;
        hw_slave_wait();
    }
    uint32_t ticks = (uint16_t)(frt() - t0);
    if (err) return 0xFFFFFFFFu;
    return ticks * 128 / (uint32_t)(reps * rows * bw);
}

static void bench_page(void)
{
    static const char *const names[7] = { "SKIP", "FILL", "FILLRUN", "PAIR", "QUAD", "RAW", "MOVE" };
    FRT_TCR = 0x02;                         /* clock / 128 */
    FRT_FTCSR = 0x00;                       /* count freely (crt0: clear at OCRA = 2) */
    md_cmd(CMD_CLEAR, 0, 0, 0);
    puts_at(1, 6, "DECODE SPEED (CYCLES A BLOCK)", 1);
    puts_at(4, 2, "BLOCK", 0);
    puts_at(4, 14, "256 COL", 0);
    puts_at(4, 26, "DIRECT", 0);
    uint32_t c[2][7];
    for (int d = 0; d < 2; d++) {
        hw_video_mode(d + 1, 0, 200);
        for (int m = 0; m < 7; m++)
            c[d][m] = bench_op(m, d + 1);
    }
    hw_video_mode(2, 0, 1);
    char t[12];
    for (int m = 0; m < 7; m++) {
        puts_at(6 + 2 * m, 2, names[m], 0);
        for (int d = 0; d < 2; d++) {
            if (c[d][m] == 0xFFFFFFFFu) str(t, "ERR");
            else num(t, c[d][m]);
            puts_at(6 + 2 * m, 14 + 12 * d, t, 1);
        }
    }
    /* a picture of RAW blocks: ms (both SH-2s) */
    for (int d = 0; d < 2; d++) if (c[d][5] == 0xFFFFFFFFu) c[d][5] = 0;
    uint32_t ms10p = c[0][5] * 80 * 28 * 10 / 23011, ms10d = c[1][5] * 80 * 25 * 10 / 23011;
    char u[40], *q = str(u, "ALL RAW: ");
    q = num(q, ms10p / 10); *q++ = '.'; q = num(q, ms10p % 10);
    q = str(q, " / ");
    q = num(q, ms10d / 10); *q++ = '.'; q = num(q, ms10d % 10);
    str(q, " MS");
    puts_at(21, 2, u, 0);
}

void probe_run(void)
{
    for (;;) {
        md_cmd(CMD_CLEAR, 0, 0, 0);
        hw_video_mode(2, 0, 1);
        puts_at(1, 8, "SEGATUBE HARDWARE TEST", 1);
        int row = 4;
        char t[40];
        vticks = 0;
        puts_at(3, 1, "EVERDRIVE I/O WITH RV=1:", 0);
        md_cmd(CMD_EDRV, 4, 0, 0);
        row = 8;
        int r = cmd_timed(CMD_SD_INIT, 0, 0, 0);
        if (r == 0xFE) line(row,  "EVERDRIVE", "NOT FOUND", 0);
        else if (r == 0xFF) line(row,  "EVERDRIVE", "SD CARD ERROR", 0);
        else {
            char *p = str(t, "FOUND, ");
            p = num(p, (uint32_t)r);
            str(p, " MOVIES");
            line(row,  "EVERDRIVE", t, r > 0);
        }
        row += 2;
        int kind = 0;
        if (r > 0 && r < 0xFE) {
            if (find_profile(&kind)) {
                sd_tests(&row, kind);
            } else {
                line(row,  "MOVIE FILES", "NOT FOUND", 0);
                row += 2;
            }
        }
        sound_test(&row);
        if (kind) decode_test(&row, kind);
        puts_at(26, 1, "PHOTO THIS SCREEN.  A: DECODE SPEED", 1);
        wait_button();
        bench_page();
        puts_at(26, 1, "PHOTO THIS SCREEN.  A: RUN AGAIN", 1);
        wait_button();
    }
}

#endif
