/*
 * player.c - SegaTube's movie player (master SH-2).
 *
 * The stream (io.c) brings the profile file into SDRAM chunk by chunk; we
 * take one record (one frame) at a time: its audio goes to the PWM ring
 * (audio.c), its picture is decoded into the frame buffer the SH-2 can reach
 * (the master decodes the top part, the slave the bottom), and it goes on
 * screen when the audio clock reaches its time.
 *
 * Both profiles (FILM, FULL) double-buffer: frame f is decoded into the
 * buffer that showed frame f-2, which is exactly what its P pictures refer
 * to.
 *
 * Seeking, fast forward and rewind work in chunks (about a second): each
 * chunk starts with two intra pictures in a row and audio for its first
 * moments.
 */
#include <string.h>
#include "hw.h"
#include "io.h"
#include "audio.h"
#include "stv_dec.h"
#include "player.h"

static movie_t mv;
static info_t inf;
static int have_info;
static int cur_movie;

/* ------------------------------------------------------------ input */

uint16_t pad_pressed, pad_held;
static uint16_t pad_last;
static uint16_t pad_vbl;

void pad_poll(void)
{
    uint16_t v = hw_vblanks();
    if (v == pad_vbl) {
        pad_pressed = 0;
        return;
    }
    pad_vbl = v;
    uint16_t p = hw_pad();
    pad_pressed = p & (uint16_t)~pad_last;
    pad_held = p;
    pad_last = p;
}

/* Forget presses made while nobody was reading the pad (the 68000 keeps
 * them until we take them), e.g. during "THE END"; buttons still held are
 * not new presses. */
void pad_flush(void)
{
    hw_pad();                       /* taken: the 68000 drops them at its next vblank */
    hw_wait_vblank();
    hw_wait_vblank();
    pad_last = hw_pad();
    pad_vbl = hw_vblanks();
    pad_pressed = 0;
}

/* A command for the 68000 while streaming: let the read in flight finish. */
void ui_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    while (!stream_idle())
        stream_poll();
    md_cmd(cmd, p1, p2, p3);
}

/* ------------------------------------------------------------ timing */

static uint32_t sample_of(uint32_t f)
{
    return (uint32_t)((uint64_t)f * mv.rate * mv.fps_den / mv.fps_num);
}

static uint32_t frame_of_ms(uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * mv.fps_num / mv.fps_den / 1000);
}

static uint32_t ms_of_frame(uint32_t f)
{
    return (uint32_t)((uint64_t)f * 1000 * mv.fps_den / mv.fps_num);
}

static uint32_t audio_base;         /* movie sample at audio_position() 0 */
static uint32_t audio_next;         /* movie sample the ring's next sample is */

static uint32_t now_sample(void)
{
    return audio_base + audio_position();
}

/* ------------------------------------------------------------ records */

typedef struct {
    uint32_t frame, size;
    int main_n, pre_n, pics, flags;
    const uint8_t *state, *pre_state, *pre, *mainp;
    const uint8_t *pic[2];
    const uint8_t *palette;
} rec_t;

static int chunk_left;              /* records left in this chunk; 0: a chunk header next */
static int chunk_first;             /* the next record is a chunk's first (intra pictures) */
static uint32_t rec_frame;          /* frame of the next record */
static int bad_stream;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* The next record, if all of it is in the stream buffer (not consumed). */
static int peek_record(rec_t *r)
{
    if (!chunk_left) {
        if (stream_avail() < sizeof(stv_chunk_header))
            return 0;
        const uint8_t *c = stream_data();
        if (rd32(c) != STV_CHUNK_MAGIC) {
            bad_stream = 1;
            return 0;
        }
        chunk_left = rd16(c + 12);
        chunk_first = 1;
        rec_frame = rd32(c + 8);
        stream_consume(sizeof(stv_chunk_header));
        if (!chunk_left)
            return 0;
    }
    if (stream_avail() < 4)
        return 0;
    const uint8_t *p = stream_data();
    uint32_t size = rd32(p);
    if (size < STV_REC_HEADER || size > STREAM_BYTES / 2) {
        bad_stream = 1;
        return 0;
    }
    if (stream_avail() < size)
        return 0;
    r->frame = rec_frame;
    r->size = size;
    r->main_n = rd16(p + 4);
    r->pre_n = rd16(p + 6);
    r->pics = rd16(p + 8);
    r->flags = rd16(p + 10);
    const uint8_t *q = p + STV_REC_HEADER;
    r->state = 0;
    r->pre_state = 0;
    if (r->flags & STV_REC_ADPCM_STATE) { r->state = q; q += 8; }
    if (r->pre_n) { r->pre_state = q; q += 8; r->pre = q; q += r->pre_n; }
    r->mainp = q;
    q += r->main_n;
    q = (const uint8_t *)(((uintptr_t)q + 3) & ~(uintptr_t)3);
    r->palette = 0;
    for (int k = 0; k < r->pics && k < 2; k++) {
        r->pic[k] = q;
        if (q[1] & STV_PIC_PALETTE)
            r->palette = q + 8;
        q += rd32(q + 4);
    }
    stream_hold(1);
    return 1;
}

static void drop_record(rec_t *r)
{
    stream_hold(0);
    stream_consume(r->size);
    chunk_left--;
    chunk_first = 0;
    rec_frame++;
}

/* ------------------------------------------------------------ audio */

static stv_adpcm al, ar;
static int16_t pcm[2 * 512];

static void service(void);

/* ADPCM at movie sample s0 into the ring; samples before audio_next are
 * dropped, a gap is filled with silence. */
static void put_audio(const uint8_t *src, int n, uint32_t s0, stv_adpcm *l, stv_adpcm *r)
{
    while (n > 0) {
        int k = n > 512 ? 512 : n;
        stv_adpcm_decode(l, r, src, k, pcm);
        int skip = 0;
        if (s0 + (uint32_t)k <= audio_next) {
            skip = k;
        } else if (s0 < audio_next) {
            skip = (int)(audio_next - s0);
        } else if (s0 > audio_next) {
            uint32_t gap = s0 - audio_next;
            if (gap > 8192) gap = 8192;
            while (audio_space() < gap) service();
            audio_write_silence((int)gap);
            audio_next = s0;
        }
        int w = k - skip;
        if (w > 0) {
            while (audio_space() < (uint32_t)w) service();
            audio_write(pcm + 2 * skip, w);
            audio_next += (uint32_t)w;
        }
        src += k; n -= k; s0 += (uint32_t)k;
    }
}

static int fresh;                   /* the first record after a seek */
static int muted;

static void record_audio(const rec_t *r)
{
    if (!mv.rate) return;
    if (fresh && r->pre_n && r->pre_state) {
        stv_adpcm pl, pr;
        stv_adpcm_load(&pl, r->pre_state);
        stv_adpcm_load(&pr, r->pre_state + 4);
        if (!muted) put_audio(r->pre, r->pre_n, sample_of(r->frame), &pl, &pr);
    }
    if (r->state) {
        stv_adpcm_load(&al, r->state);
        stv_adpcm_load(&ar, r->state + 4);
    }
    if (r->main_n && !muted)
        put_audio(r->mainp, r->main_n, sample_of(r->frame + (uint32_t)mv.lead), &al, &ar);
    else if (r->main_n) {
        /* keep the ADPCM state right even when not playing it */
        for (int i = 0; i < r->main_n; i += 512) {
            int k = r->main_n - i > 512 ? 512 : r->main_n - i;
            stv_adpcm_decode(&al, &ar, r->mainp + i, k, pcm);
        }
    }
    fresh = 0;
}

/* ------------------------------------------------------------ pictures */

typedef struct {
    const uint8_t *part;
    uint32_t len;
    stv_target t;
    int result;
} job_t;
static job_t job_storage __attribute__((aligned(16)));
#define JOB ((job_t *)SDRAM_THROUGH(&job_storage))

static void slave_decode(int arg)
{
    (void)arg;
    job_t *j = JOB;
    stv_target t = j->t;
    j->result = stv_decode_part(j->part, j->len, &t);
}

static uint16_t palette[256];
static int decode_errors;

/* ---- stats (Y while watching): where the time goes ---- */
#define FRT_TCR     (*(volatile uint8_t *)0xFFFFFE16)
#define FRT_FTCSR   (*(volatile uint8_t *)0xFFFFFE11)
#define FRT_FRCH    (*(volatile uint8_t *)0xFFFFFE12)
#define FRT_FRCL    (*(volatile uint8_t *)0xFFFFFE13)
static uint16_t frt(void)              /* 5.56 us ticks (clock / 128) */
{
    uint8_t h = FRT_FRCH;
    return (uint16_t)(h << 8 | FRT_FRCL);
}
static int stats_on;
static uint32_t st_dec_sum, st_dec_n, st_dec_max, st_skipped, st_vbl, st_fetched0;
static uint32_t st_q_min = 0xFFFFFFFFu;

static void decode_picture_(const uint8_t *pic);

/* Decode a picture into the buffer the SH-2 reaches now (timed). */
static void decode_picture(const uint8_t *pic)
{
    uint16_t t0 = frt();
    decode_picture_(pic);
    uint32_t d = (uint16_t)(frt() - t0);
    st_dec_sum += d;
    st_dec_n++;
    if (d > st_dec_max) st_dec_max = d;
}

static void decode_picture_(const uint8_t *pic)
{
    uint32_t psize = rd32(pic + 4);
    int flags = pic[1], parts = pic[2];
    const uint8_t *q = pic + 8;
    if (flags & STV_PIC_PALETTE) {
        memcpy(palette, q, 512);
        q += 512;
    }
    stv_target t;
    t.base = hw_row(0) + mv.pic_x * mv.depth;
    t.pitch = hw_row_bytes();
    t.bw = mv.pic_w / 4;
    t.bh = mv.pic_h / 4;
    t.depth = mv.depth;
    t.version = mv.version;
    t.poll = 0;                         /* (the slave never touches the stream) */
    if (parts > 2) parts = 2;
    if (parts == 2) {
        uint32_t o1 = rd32(q + 4);
        job_t *j = JOB;
        j->part = pic + o1;
        j->len = psize - o1;
        j->t = t;
        j->result = 0;
        hw_slave_start(slave_decode, 0);
    }
    uint32_t o0 = rd32(q);
    t.poll = stream_poll;               /* keep the SD card busy meanwhile */
    if (stv_decode_part(pic + o0, psize - o0, &t)) decode_errors++;
    if (parts == 2) {
        while (hw_slave_busy())
            stream_poll();
        hw_slave_wait();
        if (JOB->result) decode_errors++;
    }
}

/* ------------------------------------------------------------ screen */

static void set_video(void)
{
    hw_video_mode(mv.depth, mv.pic_y, mv.pic_h);
    if (mv.depth == 1) {
        memset(palette, 0, sizeof palette);
        hw_load_palette(palette);
    }
}

/* Show the buffer the SH-2 just drew into, from the next vblank. */
static void show_back(void)
{
    int b = hw_back_buffer();
    hw_request_show(b);
    for (int i = 0; i < 2000000 && hw_shown() != b; i++)
        stream_poll();
}

/* ------------------------------------------------------------ OSD */

static int osd_timer;               /* vblanks left on screen; -1: stays */
static uint16_t osd_flags_shown = 0xFFFF;
static uint32_t osd_pos_shown = 0xFFFFFFFF;

static uint16_t mode_bits(void)
{
    return (uint16_t)((mv.kind - KIND_FILM) << 10);
}

static void osd(uint16_t flags, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (flags == osd_flags_shown && s == osd_pos_shown)
        return;
    osd_flags_shown = flags;
    osd_pos_shown = s;
    ui_cmd(CMD_OSD, flags, (uint16_t)(s > 65535 ? 65535 : s), (uint16_t)(mv.duration_ms / 1000));
}

static void osd_hide(void)
{
    if (osd_flags_shown != OSD_HIDE) {
        osd_flags_shown = OSD_HIDE;
        ui_cmd(CMD_OSD, OSD_HIDE, 0, 0);
    }
}

/* ------------------------------------------------------------ service */

static uint16_t svc_vbl;

/* Called in every wait: moves the stream along, counts the OSD down. */
static void service(void)
{
    stream_poll();
    uint16_t vb = hw_vblanks();
    if (vb != svc_vbl) {
        if (osd_timer > 0 && --osd_timer == 0)
            osd_timer = -2;     /* hide at the next chance */
        svc_vbl = vb;
    }
}

/* ------------------------------------------------------------ seeking */

static int have_rec;

/* Start reading at chunk c; the clock restarts at its first frame. */
static void seek_chunk(int c)
{
    if (c < 0) c = 0;
    if (c >= (int)mv.chunks) c = (int)mv.chunks - 1;
    if (have_rec) {
        stream_hold(0);
        have_rec = 0;
    }
    stream_start(c);
    audio_reset();
    stv_index_entry e;
    movie_index(c, &e);
    chunk_left = 0;
    rec_frame = e.first_frame;
    audio_base = audio_next = sample_of(e.first_frame);
    fresh = 1;
    bad_stream = 0;
}

/* Wait (servicing) until the next record is in; 0 at the end or an error. */
static int wait_record(rec_t *r)
{
    for (;;) {
        if (peek_record(r)) return 1;
        if (bad_stream || stream_error() || stream_eof()) return 0;
        service();
    }
}

/* ------------------------------------------------------------ playing */

#define ACT_NONE    0
#define ACT_BACK    1
#define ACT_END     2
#define ACT_MODE    3
#define ACT_ERROR   4

static int paused;
static uint32_t shown_frame;

static uint32_t now_ms(void)
{
    return ms_of_frame(shown_frame);
}

static void show_osd_now(int extra)
{
    int icon = paused ? OSD_ICON_PAUSE : OSD_ICON_PLAY;
    osd((uint16_t)(icon | OSD_BAR | mode_bits() | extra), now_ms());
}

/* Show the chunk's first picture(s) only (scanning). */
static int show_key(int c)
{
    seek_chunk(c);
    rec_t r;
    if (!wait_record(&r)) return 0;
    decode_picture(r.pic[0]);
    show_back();
    if (r.palette) hw_load_palette(palette);
    shown_frame = r.frame;
    stream_hold(0);
    return 1;
}

/* Fast forward (dir 1) or rewind (-1), VHS style: the chunks' first pictures
 * go by at 2x to 32x. Returns the chunk to play from. */
static int scan(int dir)
{
    int speed = 1;
    audio_pause(1);
    uint32_t pos = now_ms();
    int shown = movie_chunk_of_frame(shown_frame);
    uint16_t last = hw_vblanks();
    for (;;) {
        service();
        pad_poll();
        uint16_t p = pad_pressed;
        if (p & (PAD_START | PAD_A | PAD_B | PAD_C)) break;
        if (p & (dir > 0 ? PAD_RIGHT : PAD_LEFT)) { if (speed < 5) speed++; }
        else if (p & (dir > 0 ? PAD_LEFT : PAD_RIGHT)) {
            if (speed > 1) speed--;
            else dir = -dir;
        }
        uint16_t v = hw_vblanks();
        uint32_t step = (uint32_t)(uint16_t)(v - last) * (1000u << speed) / 60;
        last = v;
        if (dir > 0) {
            pos += step;
            if (pos >= mv.duration_ms) { pos = mv.duration_ms ? mv.duration_ms - 1 : 0; }
        } else {
            pos = pos > step ? pos - step : 0;
        }
        int c = movie_chunk_of_frame(frame_of_ms(pos));
        osd((uint16_t)((dir > 0 ? OSD_ICON_FF : OSD_ICON_REW) | speed << 4 | OSD_BAR | mode_bits()), pos);
        if (c != shown) {
            if (!show_key(c)) break;
            shown = c;
        } else if ((dir < 0 && pos == 0) || (dir > 0 && pos + 1 >= mv.duration_ms)) {
            break;
        }
    }
    return shown;
}

/* P8: BGR555 -> palette index, 9-bit (3 bits a channel: a thumbnail needs
 * no more, and 512 x 256 tries take a moment where 4096 x 256 took seconds) */
static uint8_t lut512[512];

static void build_lut(void)
{
    for (int c = 0; c < 512; c++) {
        int r = (c & 7) * 4 + 2, g = (c >> 3 & 7) * 4 + 2, b = (c >> 6) * 4 + 2;
        int best = 1 << 30, bi = 0;
        for (int i = 0; i < 256; i++) {
            int p = palette[i];
            int dr = (p & 31) - r, dg = (p >> 5 & 31) - g, db = (p >> 10 & 31) - b;
            int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (d < best) { best = d; bi = i; }
        }
        lut512[c] = (uint8_t)bi;
    }
}

static uint16_t thumb[STV_THUMB_W * STV_THUMB_H];

/* The thumbnail, framed, in the buffer the SH-2 reaches now: centred, just
 * above the bottom of the picture (the OSD covers the lines below). */
static void draw_thumb(void)
{
    int x0 = (320 - STV_THUMB_W) / 2;
    int bottom = mv.pic_y + mv.pic_h;
    if (bottom > 188) bottom = 188;
    int y0 = bottom - STV_THUMB_H - 2 - mv.pic_y;      /* picture row */
    if (y0 < 2) y0 = 2;
    int rows = STV_THUMB_H;
    if (y0 + rows > mv.pic_h - 2) rows = mv.pic_h - 2 - y0;
    int x = x0 - mv.pic_x;
    if (x < 2) x = 2;
    int cols = STV_THUMB_W;
    if (x + cols > mv.pic_w - 2) cols = mv.pic_w - 2 - x;
    if (rows <= 0 || cols <= 0) return;
    for (int y = -2; y < rows + 2; y++) {
        uint8_t *row = hw_row(y0 + y);
        for (int i = -2; i < cols + 2; i++) {
            int frame_px = y < 0 || y >= rows || i < 0 || i >= cols;
            unsigned c = frame_px ? 0x7FFF : thumb[y * STV_THUMB_W + i];
            if (mv.depth == 2) {
                ((uint16_t *)(void *)row)[mv.pic_x + x + i] = (uint16_t)c;
            } else {
                unsigned k = (c >> 2 & 7) | (c >> 7 & 7) << 3 | (c >> 12 & 7) << 6;
                row[mv.pic_x + x + i] = lut512[k];
            }
        }
    }
}

/* The scrubber: a cursor on the time bar with the thumbnail at its time.
 * Returns the chunk to play from. */
static int scrub(void)
{
    audio_pause(1);
    int start_chunk = movie_chunk_of_frame(shown_frame);
    if (!have_info) return start_chunk;
    stream_stop();
    if (mv.depth == 1) build_lut();
    uint32_t cur = now_ms(), drawn = 0xFFFFFFFF;
    int hold = 0;
    int result = start_chunk;
    for (;;) {
        pad_poll();
        uint16_t p = pad_pressed, h = pad_held;
        if (p & (PAD_A | PAD_START)) { result = movie_chunk_of_frame(frame_of_ms(cur)); break; }
        if (p & PAD_B) break;
        int d = (h & PAD_RIGHT) ? 1 : (h & PAD_LEFT) ? -1 : 0;
        if (d) {
            if ((p & (PAD_LEFT | PAD_RIGHT)) || ++hold > 12) {
                uint32_t step = hold > 120 ? 60000 : hold > 50 ? 20000 : 5000;
                if (p & (PAD_LEFT | PAD_RIGHT)) step = 5000;
                if (d > 0) cur = cur + step < mv.duration_ms ? cur + step : mv.duration_ms - 1;
                else cur = cur > step ? cur - step : 0;
                if (hold > 12) hold -= 6;          /* repeat */
            }
        } else {
            hold = 0;
        }
        osd((uint16_t)(OSD_ICON_PAUSE | OSD_BAR | OSD_SCRUB | mode_bits()), cur);
        uint32_t n = inf.thumb_interval_ms ? cur / inf.thumb_interval_ms : 0;
        if (n != drawn && info_thumb(cur_movie, &inf, (int)n, thumb) == 0) {
            drawn = n;
            /* both buffers, so it stays put whichever is on screen */
            draw_thumb();
            show_back();
            draw_thumb();
        }
        service();
    }
    return result;
}

static int has_kind(int k)
{
    return !have_info || (inf.profiles >> (k - KIND_FILM) & 1);
}

/* The other picture mode (FILM <-> FULL), if the movie has it */
static int next_kind(int kind)
{
    int k = kind == KIND_FILM ? KIND_FULL : KIND_FILM;
    return has_kind(k) ? k : kind;
}

/* The controls pop-up (MODE): the movie waits until a button is pressed. */
static void show_help(void)
{
    if (!paused) audio_pause(1);
    ui_cmd(CMD_HELP, 1, 0, 0);
    for (;;) {
        service();
        pad_poll();
        if (pad_pressed) break;
    }
    ui_cmd(CMD_HELP, 0, 0, 0);
    if (!paused) audio_pause(0);
}

/* Start playing from chunk c: its first picture goes up at once. */
static int start_at(int c)
{
    seek_chunk(c);
    have_rec = 0;
    paused = 0;
    muted = 0;
    return 1;
}

static int handle_keys(int *seek_to)
{
    pad_poll();
    uint16_t p = pad_pressed;
    if (!p) return ACT_NONE;
    if (p & PAD_B) return ACT_BACK;
    if (p & PAD_C) return ACT_MODE;
    if (p & PAD_Y) {
        stats_on = !stats_on;
        if (!stats_on) ui_cmd(CMD_STATS, 0x8000, 0, 0);     /* away */
    }
    if (p & PAD_START) {
        paused = !paused;
        audio_pause(paused);
        osd_timer = paused ? -1 : 180;
        show_osd_now(OSD_HINTS);
    }
    if (p & PAD_A) {
        osd_timer = 180;
        show_osd_now(OSD_HINTS);
    }
    if (p & PAD_MODE) {
        show_help();
        return ACT_NONE;
    }
    if (p & (PAD_RIGHT | PAD_LEFT)) {
        int c = scan((p & PAD_RIGHT) ? 1 : -1);
        *seek_to = c;
    }
    if (p & (PAD_DOWN | PAD_UP)) {
        int c = scrub();
        *seek_to = c;
    }
    if (p & (PAD_X | PAD_Z)) {
        uint32_t ms = now_ms();
        if (p & PAD_X) ms = ms > 10000 ? ms - 10000 : 0;
        else ms = ms + 30000 < mv.duration_ms ? ms + 30000 : ms;
        *seek_to = movie_chunk_of_frame(frame_of_ms(ms));
    }
    return ACT_NONE;
}

/* ---- playing ---- */

/* Once a second while the stats are on: two lines at the top */
static void stats_tick(void)
{
    uint32_t q = audio_queued();
    if (q < st_q_min) st_q_min = q;
    uint16_t v = hw_vblanks();
    if ((uint16_t)(v - st_vbl) < 60) return;
    uint32_t secs_vbl = (uint16_t)(v - st_vbl);
    st_vbl = v;
    uint32_t fetched = stream_fetched();
    uint32_t kbs = (fetched - st_fetched0) * 60 / secs_vbl / 1024;
    st_fetched0 = fetched;
    if (stats_on) {
        /* tick = 128 / 23.01 MHz: ms x 10 = ticks x 1280 / 23011 */
        uint32_t avg = st_dec_n ? st_dec_sum / st_dec_n : 0;
        uint32_t avg10 = avg * 1280 / 23011, max10 = st_dec_max * 1280 / 23011;
        uint32_t qms = st_q_min == 0xFFFFFFFFu ? 0 : st_q_min * 1000 / (mv.rate ? mv.rate : 1);
        uint32_t frame10 = mv.fps_num ? 10000u * mv.fps_den / mv.fps_num : 0;
        ui_cmd(CMD_STATS, (uint16_t)(avg10 > 0x7FFF ? 0x7FFF : avg10),
               (uint16_t)(max10 > 0xFFFF ? 0xFFFF : max10), (uint16_t)frame10);
        ui_cmd(CMD_STATS, (uint16_t)(0x4000 | (kbs > 0x3FFF ? 0x3FFF : kbs)),
               (uint16_t)(stream_avail() / 1024 | (qms / 2 > 255 ? 255 : qms / 2) << 8),
               (uint16_t)((audio_underruns() > 255 ? 255 : audio_underruns()) << 8 |
                          (st_skipped > 255 ? 255 : st_skipped)));
    }
    st_dec_sum = st_dec_n = st_dec_max = 0;
    st_q_min = 0xFFFFFFFFu;
}

static int play_from(int c)
{
    start_at(c);
    int have_next = 0, first = 1, skipping = 0;
    uint32_t next_frame = 0;
    const uint16_t *next_pal = 0;
    uint32_t half_vbl = mv.rate / 120;
    for (;;) {
        service();
        int seek_to = -1;
        int act = handle_keys(&seek_to);
        if (act != ACT_NONE) return act;
        if (seek_to >= 0) {
            start_at(seek_to);
            have_next = 0;
            first = 1;
            osd_timer = 120;
            continue;
        }
        if (osd_timer == -2) { osd_hide(); osd_timer = 0; }
        else if (osd_timer != 0) show_osd_now(osd_timer < 0 ? OSD_HINTS : 0);

        stats_tick();
        if (!have_next) {
            rec_t r;
            if (peek_record(&r)) {
                /* Falling behind (the sound is about to run dry): keep the
                 * sound going and skip pictures up to the next chunk's
                 * intra pictures (the picture holds for a moment). */
                if (chunk_first) skipping = 0;
                else if (!first && !paused && mv.rate && audio_queued() < mv.rate / 16)
                    skipping = 1;
                record_audio(&r);
                if (skipping) {
                    drop_record(&r);
                    st_skipped++;
                    continue;
                }
                decode_picture(r.pic[0]);
                next_pal = r.palette ? palette : 0;
                next_frame = r.frame;
                drop_record(&r);
                have_next = 1;
            } else if (bad_stream || stream_error()) {
                return ACT_ERROR;
            } else if (stream_eof()) {
                /* wait for the audio to run out, then the end */
                /* (less than a DMA block left never plays out) */
                if (audio_queued() < 256 || paused) return paused ? ACT_NONE : ACT_END;
            }
        }
        if (have_next && !paused) {
            uint32_t now = now_sample();
            if (first || now + half_vbl >= sample_of(next_frame)) {
                show_back();
                if (next_pal) hw_load_palette(next_pal);
                shown_frame = next_frame;
                have_next = 0;
                if (first) {
                    audio_pause(0);
                    first = 0;
                }
            } else if (now > sample_of(next_frame) + mv.rate / 2) {
                /* far behind (a slow card?): pick up at the next chunk */
                int nc = movie_chunk_of_frame(next_frame) + 1;
                if (nc < (int)mv.chunks) { start_at(nc); have_next = 0; first = 1; }
            }
        }
    }
}

/* ------------------------------------------------------------ entry */

int play_movie(int m, int kind)
{
    cur_movie = m;
    /* the free-running timer: clock / 128, counting freely (crt0 sets it
     * to clear on compare match A at 2; the output level stays the same) */
    FRT_TCR = 0x02;
    FRT_FTCSR = 0x00;
    st_skipped = 0;
    have_info = info_read(m, &inf) == 0;
    if (kind != KIND_FILM && kind != KIND_FULL) kind = KIND_FILM;
    if (!has_kind(kind)) kind = next_kind(kind);
    if (!has_kind(kind)) {
        /* neither FILM nor FULL (converted with an old version?) */
        ui_cmd(CMD_MESSAGE, MSG_NO_MODE, 0, 0);
        for (int i = 0; i < 150; i++) hw_wait_vblank();
        ui_cmd(CMD_MESSAGE, MSG_NONE, 0, 0);
        return PLAY_ERROR;
    }
    ui_cmd(CMD_TITLE, (uint16_t)m, 0, 0);
    uint32_t resume_ms = 0;
    for (;;) {
        ui_cmd(CMD_MESSAGE, MSG_LOADING, 0, 0);
        TRACE(0x7001);
        int e = movie_open(&mv, m, kind);
        TRACE(0x7002);
        if (e) {
            ui_cmd(CMD_MESSAGE, MSG_ERROR, (uint16_t)e, 0);
            for (int i = 0; i < 120; i++) hw_wait_vblank();
            ui_cmd(CMD_MESSAGE, MSG_NONE, 0, 0);
            return PLAY_ERROR;
        }
        if (mv.rate) audio_init((int)mv.rate);
        TRACE(0x7003);
#ifdef DEBUG_STEPS
        ui_cmd(CMD_MESSAGE, MSG_ERROR, 0x102, 0);
        for (int i = 0; i < 30; i++) hw_wait_vblank();
#endif
        ui_cmd(CMD_MESSAGE, MSG_NONE, 0, 0);
        set_video();
#ifdef DEBUG_STEPS
        ui_cmd(CMD_MESSAGE, MSG_ERROR, 0x103, 0);
        for (int i = 0; i < 30; i++) hw_wait_vblank();
#endif
        osd_flags_shown = 0xFFFF;
        osd_timer = 150;
        int c = resume_ms ? movie_chunk_of_frame(frame_of_ms(resume_ms)) : 0;
        int act = play_from(c);
        stream_stop();
        audio_reset();
        if (act == ACT_MODE) {
            resume_ms = now_ms();
            int k = next_kind(kind);
            if (k == kind) continue;
            kind = k;
            continue;
        }
        ui_cmd(CMD_OSD, OSD_HIDE, 0, 0);
        if (act == ACT_END) {
            ui_cmd(CMD_MESSAGE, MSG_END, 0, 0);
            for (int i = 0; i < 150; i++) hw_wait_vblank();
            ui_cmd(CMD_MESSAGE, MSG_NONE, 0, 0);
            return PLAY_END;
        }
        if (act == ACT_ERROR) {
            ui_cmd(CMD_MESSAGE, MSG_ERROR, (uint16_t)(stream_error() | decode_errors << 8), 0);
            for (int i = 0; i < 150; i++) hw_wait_vblank();
            ui_cmd(CMD_MESSAGE, MSG_NONE, 0, 0);
            return PLAY_ERROR;
        }
        return PLAY_BACK;
    }
}
