/* io.c - movie files for the master SH-2.
 *
 * On the console the 68000 reads the SD card for us (CMD_OPEN / CMD_READ):
 * the EverDrive copies file data into cartridge memory beyond the ROM, and
 * we copy it from there into SDRAM with DMA channel 0. While a CMD_READ runs
 * the cartridge bus belongs to the EverDrive, so nothing here touches the
 * cartridge until the 68000 answers (the SH-2s' code and data are in SDRAM).
 *
 * Test builds carry their movies in the ROM (tools/mkvfs.py); then the same
 * calls read the ROM directly.
 */
#include <string.h>
#include "hw.h"
#include "io.h"

#define SLOT_SCRATCH    16          /* shared slot for io_read */
#define SLOT_INDEX      17          /* shared slots 17..31: the chunk index */
#define INDEX_MAX       ((32 - SLOT_INDEX) * SLOT_BYTES / 16)

static int vfs;                     /* movies in the ROM */
static uint32_t vfs_at;             /* cartridge offset of the VFS */
static int vfs_count;
static uint32_t vfs_file_off, vfs_file_size;    /* the open file */
static int open_movie = -1, open_kind = -1, open_part = -1;

static inline const uint8_t *cart(uint32_t off)
{
    return (const uint8_t *)(uintptr_t)(CART_THROUGH + off);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int vfs_find(void)
{
    const uint8_t *h = cart(0x1C8);
    if (memcmp((const void *)h, "STVFS@", 6))
        return 0;
    uint32_t at = rd32(h + 6);
    if (at < 0x1000 || at >= 0x400000 || memcmp((const void *)cart(at), "STVFS1", 6))
        return 0;
    vfs_at = at;
    vfs_count = (int)rd32(cart(at + 8));
    return vfs_count > 0;
}

int io_init(void)
{
    if ((hw_flags() & FL_VFS) && vfs_find()) {
        vfs = 1;
        md_cmd(CMD_SD_INIT, 0, 0, 0);
        return vfs_count;
    }
    int r = md_cmd(CMD_SD_INIT, 0, 0, 0);
    if (r == 0xFE) return -1;
    if (r == 0xFF) return -2;
    return r;
}

int io_is_vfs(void)
{
    return vfs;
}

int io_open(int movie, int kind, int part)
{
    if (vfs) {
        if (movie < 0 || movie >= vfs_count || part)
            return 0xFF;
        const uint8_t *e = cart(vfs_at + 16 + (uint32_t)movie * 80);
        uint32_t off = rd32(e + 48 + 4 * kind), size = rd32(e + 64 + 4 * kind);
        if (!off) return 0xFE;
        vfs_file_off = vfs_at + off;
        vfs_file_size = size;
        open_movie = movie; open_kind = kind; open_part = part;
        return 0;
    }
    if (movie == open_movie && kind == open_kind && part == open_part)
        return 0;
    int r = md_cmd(CMD_OPEN, (uint16_t)movie, (uint16_t)(kind | part << 8), 0);
    if (r == 0) {
        open_movie = movie; open_kind = kind; open_part = part;
    } else {
        open_movie = -1;
    }
    return r;
}

uint32_t io_size(void)
{
    return vfs ? vfs_file_size : 0xFFFFFFFFu;
}

static void copy_from_cart(void *dst, uint32_t cart_off, uint32_t len)
{
    if (!(((uintptr_t)dst | len) & 3) && len >= 64) {
        dma0_start(dst, (const void *)(uintptr_t)(CART_CACHED + cart_off), len);
        dma0_wait();
        hw_cache_purge_range(dst, len);
    } else {
        const uint8_t *s = cart(cart_off);
        uint8_t *d = dst;
        for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    }
}

int io_read(uint32_t off, void *dst, uint32_t len)
{
    if (vfs) {
        if (off + len > vfs_file_size) return 0xFD;
        copy_from_cart(dst, vfs_file_off + off, len);
        return 0;
    }
    uint8_t *d = dst;
    while (len) {
        uint32_t n = len > SLOT_BYTES ? SLOT_BYTES : len;
        uint32_t units = (n + 255) >> 8;
        int r = md_cmd(CMD_READ, (uint16_t)(off >> 16), (uint16_t)off,
                       (uint16_t)(SLOT_SCRATCH << 8 | (units & 0x7F)));
        if (r) return r;
        copy_from_cart(d, CART_SHARED + (SLOT_SCRATCH - 16) * SLOT_BYTES, n);
        d += n; off += n; len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------ profile files */

static movie_t *cur;
static uint32_t index_cart;         /* cartridge offset of the chunk index */
static uint8_t hdr_buf[256] __attribute__((aligned(4)));

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int movie_open(movie_t *m, int movie, int kind)
{
    int r = io_open(movie, kind, 0);
    if (r) return r;
    TRACE(0x7201);
    r = io_read(0, hdr_buf, 256);
    TRACE(0x7202);
    if (r) return r;
    const uint8_t *h = hdr_buf;
    if (rd32(h) != STV_MAGIC) return 0xFC;
    m->movie = movie;
    m->kind = kind;
    m->version = rd16(h + 4);
    m->depth = rd16(h + 8);
    m->interlaced = rd16(h + 10);
    m->pic_w = rd16(h + 12); m->pic_h = rd16(h + 14);
    m->pic_x = rd16(h + 16); m->pic_y = rd16(h + 18);
    m->fps_num = rd32(h + 20); m->fps_den = rd32(h + 24);
    m->rate = rd32(h + 28);
    m->frames = rd32(h + 32);
    m->chunks = rd32(h + 36);
    uint32_t index_off = rd32(h + 40);
    m->max_record = rd32(h + 48);
    m->duration_ms = rd32(h + 56);
    m->parts = rd16(h + 60);
    m->lead = rd16(h + 62);
    memcpy(m->title, h + 68, 64);
    m->title[63] = 0;
    if (!m->fps_num || !m->fps_den || !m->chunks || m->pic_w > 320 || m->pic_h > 224)
        return 0xFB;
    if (vfs) {
        index_cart = vfs_file_off + index_off;
    } else {
        if (m->chunks > INDEX_MAX) return 0xFA;
        uint32_t bytes = m->chunks * 16, done = 0;
        for (int slot = SLOT_INDEX; done < bytes; slot++) {
            uint32_t n = bytes - done > SLOT_BYTES ? SLOT_BYTES : bytes - done;
            r = md_cmd(CMD_READ, (uint16_t)((index_off + done) >> 16), (uint16_t)(index_off + done),
                       (uint16_t)(slot << 8 | (((n + 255) >> 8) & 0x7F)));
            if (r) return r;
            done += n;
        }
        index_cart = CART_SHARED + (SLOT_INDEX - 16) * SLOT_BYTES;
    }
    cur = m;
    return 0;
}

void movie_index(int c, stv_index_entry *e)
{
    const uint8_t *p = cart(index_cart + (uint32_t)c * 16);
    e->offset = rd32(p);
    e->size = rd32(p + 4);
    e->first_frame = rd32(p + 8);
    e->part = rd16(p + 12);
    e->frames = rd16(p + 14);
}

/* Binary search: the chunk holding frame f */
int movie_chunk_of_frame(uint32_t f)
{
    int lo = 0, hi = (int)cur->chunks - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        stv_index_entry e;
        movie_index(mid, &e);
        if (e.first_frame <= f) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* ------------------------------------------------------------ streaming */

static uint8_t sbuf[STREAM_BYTES] __attribute__((aligned(16)));
static uint32_t s_rd, s_wr;
static int s_chunk;                 /* next chunk to fetch */
static uint32_t s_cpos, s_csize, s_coff;
static int s_cpart;
static int s_state;                 /* 0 idle, 1 reading (68000), 2 copying (DMA) */
static uint32_t s_n;
static int s_stop, s_err, s_hold;

#define S_IDLE 0
#define S_READ 1
#define S_COPY 2

void stream_start(int chunk)
{
    stream_stop();
    s_rd = s_wr = 0;
    s_chunk = chunk;
    s_csize = 0;
    s_stop = 0;
    s_err = 0;
}

void stream_stop(void)
{
    s_stop = 1;
    while (s_state != S_IDLE)
        stream_poll();
}

int stream_idle(void)
{
    return s_state == S_IDLE;
}

int stream_error(void)
{
    return s_err;
}

void stream_poll(void)
{
    switch (s_state) {
    case S_IDLE: {
        if (s_stop || s_err || !cur || s_chunk >= (int)cur->chunks)
            return;
        if (!s_csize) {
            stv_index_entry e;
            movie_index(s_chunk, &e);
            s_coff = e.offset;
            s_csize = e.size;
            s_cpart = e.part;
            s_cpos = 0;
            if (io_open(cur->movie, cur->kind, s_cpart)) {
                s_err = 1;
                return;
            }
        }
        /* room at the end; slide the unread data down when it runs out
         * (not while the player holds pointers into it) */
        if (STREAM_BYTES - s_wr < SLOT_BYTES && s_rd > 0 && !s_hold) {
            memmove(sbuf, sbuf + s_rd, s_wr - s_rd);
            s_wr -= s_rd;
            s_rd = 0;
        }
        uint32_t n = s_csize - s_cpos;
        if (n > SLOT_BYTES) n = SLOT_BYTES;
        if (n > STREAM_BYTES - s_wr) n = (STREAM_BYTES - s_wr) & ~3u;
        if (!n)
            return;
        s_n = n;
        if (vfs) {
            if (s_coff + s_cpos + n > vfs_file_size) { s_err = 1; return; }
            dma0_start(sbuf + s_wr, (const void *)(uintptr_t)(CART_CACHED + vfs_file_off + s_coff + s_cpos), n);
            s_state = S_COPY;
        } else {
            uint32_t off = s_coff + s_cpos;
            md_cmd_start(CMD_READ, (uint16_t)(off >> 16), (uint16_t)off,
                         (uint16_t)(0 << 8 | (((n + 255) >> 8) & 0x7F)));
            s_state = S_READ;
        }
        return;
    }
    case S_READ: {
        int r = md_cmd_done();
        if (r < 0)
            return;
        if (r) {
            s_err = r;
            s_state = S_IDLE;
            return;
        }
        dma0_start(sbuf + s_wr, (const void *)(uintptr_t)(CART_CACHED + CART_RING), s_n);
        s_state = S_COPY;
        return;
    }
    case S_COPY:
        if (dma0_busy())
            return;
        dma0_wait();
        hw_cache_purge_range(sbuf + s_wr, s_n);
        s_wr += s_n;
        s_cpos += s_n;
        if (s_cpos >= s_csize) {
            s_chunk++;
            s_csize = 0;
        }
        s_state = S_IDLE;
        return;
    }
}

void *stream_scratch(void)
{
    stream_stop();
    return sbuf;
}

void stream_hold(int on)
{
    s_hold = on;
}

uint32_t stream_avail(void)
{
    return s_wr - s_rd;
}

const uint8_t *stream_data(void)
{
    return sbuf + s_rd;
}

void stream_consume(uint32_t n)
{
    s_rd += n;
    if (s_rd > s_wr) s_rd = s_wr;
}

int stream_eof(void)
{
    return cur && s_chunk >= (int)cur->chunks && s_state == S_IDLE && s_rd == s_wr;
}

/* ------------------------------------------------------------ INFO.STI */

int info_read(int movie, info_t *inf)
{
    int r = io_open(movie, KIND_INFO, 0);
    if (r) return r;
    r = io_read(0, hdr_buf, 128);
    if (r) return r;
    const uint8_t *h = hdr_buf;
    if (rd32(h) != STV_INFO_MAGIC) return 0xFC;
    inf->profiles = rd16(h + 6);
    inf->duration_ms = rd32(h + 8);
    inf->thumb_count = rd32(h + 12);
    inf->thumb_interval_ms = rd32(h + 16);
    inf->thumb_offset = rd32(h + 20);
    inf->poster_offset = rd32(h + 24);
    memcpy(inf->title, h + 64, 64);
    inf->title[63] = 0;
    return 0;
}

int info_poster(int movie, const info_t *inf, uint16_t *dst)
{
    int r = io_open(movie, KIND_INFO, 0);
    if (r) return r;
    return io_read(inf->poster_offset, dst, STV_POSTER_W * STV_POSTER_H * 2);
}

int info_thumb(int movie, const info_t *inf, int n, uint16_t *dst)
{
    if (n < 0) n = 0;
    if (inf->thumb_count && (uint32_t)n >= inf->thumb_count) n = (int)inf->thumb_count - 1;
    int r = io_open(movie, KIND_INFO, 0);
    if (r) return r;
    return io_read(inf->thumb_offset + (uint32_t)n * STV_THUMB_BYTES, dst, STV_THUMB_BYTES);
}
