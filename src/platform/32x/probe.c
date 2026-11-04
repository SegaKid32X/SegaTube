/*
 * probe.c - SegaTube's hardware test ROM (make probe -> rom/segatube-probe.32x).
 *
 * Measures what only a real console can tell: whether the Mega EverDrive
 * PRO answers with the 32X attached, how fast it reads the SD card into
 * cartridge memory, whether that data is stable, how long the SH-2s take to
 * decode a picture of your first movie, that the PWM makes sound, and whether
 * the 32X picture follows the Mega Drive's 480i interlace. Results go on
 * screen for you to read out (or photograph).
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
    static const int kinds[3] = { KIND_FULL, KIND_FILM, KIND_I480 };
    for (int i = 0; i < 3; i++)
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
    stv_target tg = { hw_row(0) + mv.pic_x * mv.depth, hw_row_bytes(), mv.pic_w / 4, mv.pic_h / 4, mv.depth, mv.version };
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

/* 480i calibration. The Mega Drive text ruler (columns every 32 pixels) is
 * the same in both fields; each 32X buffer draws bars on that grid (even
 * field: reds, odd field: blues), a fine diagonal and a block of colour.
 * LEFT/RIGHT shift the odd field's picture, UP/DOWN the even one's (through
 * the line table, 2 pixels a step) until both line up with the ruler. */
static int shift_px[2];

static void interlace_tables(int b)
{
    static uint16_t rows[256];
    for (int l = 0; l < 256; l++) rows[l] = (uint16_t)(l < 224 ? l : 223);
    volatile uint16_t *fb = MARS_FRAMEBUF;
    for (int l = 0; l < 256; l++)
        fb[l] = (uint16_t)(0x100 + rows[l] * 160 + shift_px[b] / 2);
}

static void interlace_draw(int b)
{
    for (int r = 0; r < 224; r++) {
        uint8_t *row = hw_row(r);
        int line = 2 * r + b;                       /* of 448 */
        memset(row, 0, 320);
        if (r >= 12 && r < 60)                      /* ruler bars */
            for (int x = 0; x < 320; x++) row[x] = (uint8_t)(b ? ((x >> 5) & 1 ? 3 : 5) : ((x >> 5) & 1 ? 2 : 4));
        if (r >= 70 && r < 200) {
            memset(row + 16, b ? 3 : 2, 96);        /* the colour block */
            int x = 128 + line * 160 / 448;         /* a fine diagonal */
            for (int k = 0; k < 3; k++) row[x + k] = 1;
        }
        row[0] = row[1] = row[159] = row[160] = row[318] = row[319] = 1;
    }
    interlace_tables(b);
}

/* Resync tricks: rewrite the 32X display mode at the start of a field, in
 * case that restarts the 32X's line timing (the late field is half a line
 * behind). 0 none, 1 blank+packed every field, 2 only before the blue
 * buffer's field, 3 only before the red one's, 4 toggle the 240-line bit. */
static int trick;

static void apply_trick(int next)
{
    switch (trick) {
    case 1: MARS_VDP_MODE = VDP_MODE_BLANK; MARS_VDP_MODE = VDP_MODE_PACKED; break;
    case 2: if (next == 1) { MARS_VDP_MODE = VDP_MODE_BLANK; MARS_VDP_MODE = VDP_MODE_PACKED; } break;
    case 3: if (next == 0) { MARS_VDP_MODE = VDP_MODE_BLANK; MARS_VDP_MODE = VDP_MODE_PACKED; } break;
    case 4: MARS_VDP_MODE = VDP_MODE_PACKED | 0x40; MARS_VDP_MODE = VDP_MODE_PACKED; break;
    /* 5-7: the 68000 (CMD_VIDEO) - 5 interlace mode 2, 6 rewrite VDP register
     * 12 each vblank, 7 pulse the display each vblank */
    case 8: MARS_ADAPTER = 0; MARS_ADAPTER = 0x8000; break;    /* FM off and on */
    }
}

static void trick_video(void)
{
    uint16_t p1 = 1;
    if (trick == 5) p1 |= 2;
    if (trick == 6) p1 |= 1 << 2;
    if (trick == 7) p1 |= 2 << 2;
    md_cmd(CMD_VIDEO, p1, 0, 0);
}

static void show_shift(void)
{
    char t[40], *p = str(t, "BLUE ");
    int v = shift_px[1];
    *p++ = v < 0 ? '-' : '+';
    p = num(p, (uint32_t)(v < 0 ? -v : v));
    p = str(p, "  RED ");
    v = shift_px[0];
    *p++ = v < 0 ? '-' : '+';
    p = num(p, (uint32_t)(v < 0 ? -v : v));
    p = str(p, " PX  TRICK ");
    *p++ = (char)('0' + trick);
    str(p, "   ");
    puts_at(23, 1, t, 1);
}

static void interlace_test(void)
{
    static uint16_t pal[256];
    memset(pal, 0, sizeof pal);
    pal[1] = 0x7FFF;            /* white */
    pal[2] = 0x001F;            /* red */
    pal[3] = 0x7C00;            /* blue */
    pal[4] = 0x000C;            /* dark red */
    pal[5] = 0x3000;            /* dark blue */
    hw_video_mode(1, 0, 224);
    for (int k = 0; k < 2; k++) {
        interlace_draw(hw_back_buffer());
        hw_flip();
    }
    hw_load_palette(pal);
    md_cmd(CMD_CLEAR, 0, 0, 0);
    for (int c = 0; c < 10; c++) {                   /* the ruler: 0 32 64 ... */
        char t[6];
        num(t, (uint32_t)c * 32);
        puts_at(0, c * 4, t, 0);
        puts_at(1, c * 4, "!", 0);
    }
    md_cmd(CMD_VIDEO, 1, 0, 0);
    puts_at(24, 1, "LINE UP BOTH FIELDS' BARS WITH THE", 0);
    puts_at(25, 1, "RULER: <> BLUE, UP/DN RED, B SWAP", 0);
    puts_at(26, 1, "A: NEXT TRICK  START: DONE", 1);
    show_shift();
    int swap = 0, last = -1, changes = 0, samples = 0, par = 0;
    int dirty[2] = { 0, 0 };
    for (;;) {
        while (hw_in_vblank()) ;
        uint32_t spins = 0;
        int sampled = 0;
        while (!hw_in_vblank()) {
            if (!sampled && ++spins == 200) {
                int odd = (hw_flags() & FL_ODD) != 0;
                if (last >= 0 && odd != last) changes++;
                last = odd;
                samples++;
                sampled = 1;
            }
        }
        par ^= 1;
        int next = par ^ swap;
        /* the buffer not on screen can take a new line table now */
        int hidden = hw_back_buffer();
        if (dirty[hidden]) { interlace_tables(hidden); dirty[hidden] = 0; }
        apply_trick(next);
        hw_request_show(next);
        pad_poll();
        uint16_t p = pad_pressed;
        if (p & PAD_B) swap ^= 1;
        if (p & (PAD_LEFT | PAD_RIGHT)) {
            shift_px[1] += (p & PAD_RIGHT) ? 2 : -2;
            dirty[1] = 1;
            show_shift();
        }
        if (p & (PAD_UP | PAD_DOWN)) {
            shift_px[0] += (p & PAD_DOWN) ? 2 : -2;
            dirty[0] = 1;
            show_shift();
        }
        if (p & (PAD_A | PAD_C)) {
            trick = (trick + 1) % 9;
            trick_video();
            show_shift();
        }
        if (p & PAD_START) break;
    }
    MARS_VDP_MODE = VDP_MODE_PACKED;
    md_cmd(CMD_VIDEO, 0, 0, 0);
    md_cmd(CMD_CLEAR, 0, 0, 0);
    char t[48], *q = str(t, "ODD FLAG CHANGED ");
    q = num(q, (uint32_t)changes);
    q = str(q, " OF ");
    q = num(q, (uint32_t)samples);
    str(q, " FIELDS");
    puts_at(10, 4, t, 1);
    show_shift();
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
        puts_at(26, 1, "A: 480I TEST   (PHOTO THIS SCREEN)", 1);
        wait_button();
        interlace_test();
        puts_at(14, 6, "START: RUN THE TESTS AGAIN", 0);
        puts_at(16, 6, "(PHOTO THIS SCREEN TOO)", 0);
        wait_button();
    }
}

#endif
