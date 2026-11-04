/* md_main.c - SegaTube's 68000 main loop.
 *
 * Boot: the screen turns blue at our first instruction (md_crt.s), we set up
 * the VDP and the text layer, look for movies (built into the ROM, or the
 * Mega EverDrive's SD card later, when the master asks), wait for the 32X
 * boot ROMs' "M_OK"/"S_OK" and release the master SH-2.
 *
 * Then, forever: at each vblank, read the pad (latching fresh presses until
 * the SH-2 acknowledges them), count, and publish the TV field's parity; in
 * between, carry out the master's commands (see md.h).
 */
#include "md.h"

char movie_name[MAX_MOVIES][NAME_LEN];
int  movie_count;
volatile uint16_t vblanks;

static uint16_t latched, last_pad, last_ack;
static int pad6;
static uint16_t flags;

#define VDP_STATUS_VB   0x0008
#define VDP_STATUS_ODD  0x0010

void vdp_wait_vblank(void)
{
    while (VDP_CTRL & VDP_STATUS_VB)
        ;
    while (!(VDP_CTRL & VDP_STATUS_VB))
        ;
}

void hex_string(char *out, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) {
        int d = (int)(v & 15);
        out[i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    out[digits] = 0;
}

static void delay(int n)
{
    while (n--)
        __asm__ volatile ("nop");
}

/* Pad 1, 3- or 6-button (see ZERO SECTOR's md_main.c for the sequence).
 * Bits: U1 D2 L4 R8 B10 C20 A40 START80, 6-button Z100 Y200 X400 MODE800. */
static uint16_t pad_read(void)
{
    PAD1_DATA = 0x40; delay(4);
    uint8_t hi = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x00; delay(4);
    uint8_t lo = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x40; delay(4);
    PAD1_DATA = 0x00; delay(4);
    PAD1_DATA = 0x40; delay(4);
    PAD1_DATA = 0x00; delay(4);
    uint8_t id = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x40; delay(4);
    uint8_t ext = (uint8_t)~PAD1_DATA;
    PAD1_DATA = 0x00; delay(4);
    PAD1_DATA = 0x40;
    uint16_t v = (uint16_t)((hi & 0x3F) | ((lo & 0x30) << 2));
    pad6 = (id & 0x0F) == 0x0F;
    if (pad6)
        v |= (uint16_t)((ext & 0x0F) << 8);
    return v;
}

static void vdp_init(void)
{
    VDP_CTRL = 0x8004;      /* no HINT */
    VDP_CTRL = 0x8144;      /* display on, mode 5, V28, no VINT */
    VDP_CTRL = 0x8230;      /* plane A at 0xC000 */
    VDP_CTRL = 0x8407;      /* plane B at 0xE000 */
    VDP_CTRL = 0x8578;      /* sprites at 0xF000 */
    VDP_CTRL = 0x8700;      /* backdrop: palette 0 colour 0 */
    VDP_CTRL = 0x8B00;      /* full-screen scroll */
    VDP_CTRL = 0x8C81;      /* H40, no interlace */
    VDP_CTRL = 0x8D2B;      /* H scroll table at 0xAC00 */
    VDP_CTRL = 0x8F02;      /* auto-increment 2 */
    VDP_CTRL = 0x9001;      /* 64x32 planes */
}

static void set_flag(uint16_t f, int on)
{
    flags = on ? (flags | f) : (flags & (uint16_t)~f);
    COMM16(C_FLAGS) = flags;
}

/* ------------------------------------------------------------ commands */

static const char *const kind_file[4] = { "INFO.STI", "FILM.STV", "FULL.STV", "I480.STV" };
static int osd_movie = -1;

static int build_path(char *out, int movie, int kind, int part)
{
    if (movie < 0 || movie >= movie_count || kind < 0 || kind > 3)
        return -1;
    const char *a = "/MOVIES/";
    char *p = out;
    while (*a) *p++ = *a++;
    for (const char *n = movie_name[movie]; *n; n++) *p++ = *n;
    *p++ = '/';
    for (const char *f = kind_file[kind]; *f; f++) *p++ = *f;
    if (part > 0 && part < 10)
        p[-1] = (char)('0' + part);        /* FILM.ST1, FILM.ST2, ... */
    *p = 0;
    return 0;
}

static unsigned run_command(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    char path[NAME_LEN + 32];
    switch (cmd) {
    case CMD_SD_INIT: {
        if (flags & FL_VFS)
            return (unsigned)movie_count;
        if (!(flags & FL_ED)) {
            set_flag(FL_ED, ed_detect());
            if (!(flags & FL_ED))
                return 0xFE;
        }
        int n = ed_list_movies();
        return n < 0 ? 0xFF : (unsigned)(n > 0xFD ? 0xFD : n);
    }
    case CMD_OPEN:
        if (build_path(path, p1, p2 & 0xFF, p2 >> 8))
            return 0xFF;
        return (unsigned)ed_open(path);
    case CMD_READ: {
        uint32_t off = (uint32_t)p1 << 16 | p2;
        int slot = p3 >> 8, units = p3 & 0xFF;
        uint32_t len = (uint32_t)(units ? units : 128) << 8;
        uint32_t dst = slot < 16 ? CART_RING + (uint32_t)slot * SLOT_BYTES
                                 : CART_SHARED + (uint32_t)(slot - 16) * SLOT_BYTES;
        if (slot > 31)
            return 0xFF;
        return (unsigned)ed_read_to_cart(off, dst, len);
    }
    case CMD_VIDEO:
        VDP_CTRL = (p1 & 1) ? 0x8C83 : 0x8C81;      /* H40, interlace mode 1 or none */
        set_flag(FL_INTERLACE, p1 & 1);
        return 0;
    case CMD_OSD:
        osd_draw(p1, p2, p3, osd_movie);
        return 0;
    case CMD_TITLE:
        osd_movie = (int16_t)p1;
        return 0;
    case CMD_LIBRARY:
        osd_library(p1 & 0xFFF, p2, p3, p1 >> 12);
        return 0;
    case CMD_MESSAGE:
        osd_message(p1, p2);
        return 0;
    case CMD_CLEAR:
        osd_clear();
        return 0;
    }
    return 0xFF;
}

/* ------------------------------------------------------------ crash screens */

void md_fatal(int vector)
{
    static const char *const names[12] = {
        "?", "?", "BUS ERROR", "ADDRESS ERROR", "ILLEGAL OPCODE", "DIVIDE BY ZERO",
        "CHK", "TRAPV", "PRIVILEGE", "TRACE", "LINE A", "LINE F" };
    vdp_init();
    osd_init();
    osd_boot("68000 CRASH", names[vector < 12 ? vector : 0]);
    for (;;)
        ;
}

static void sh2_crash(void)
{
    char t[24] = "PC ";
    hex_string(t + 3, (uint32_t)COMM16(C_P1) << 16 | COMM16(C_P2), 8);
    osd_boot(COMM16(C_P3) == 0xD ? "SLAVE SH-2 CRASH" : "MASTER SH-2 CRASH", t);
    for (;;)
        ;
}

/* ------------------------------------------------------------ main */

#define M_OK 0x4D5F4F4Bu
#define S_OK 0x535F4F4Bu

void md_main(int boot_err)
{
    __asm__ volatile ("move.w #0x2700,%sr");
    PAD1_CTRL = 0x40;
    PAD1_DATA = 0x40;
    vdp_init();
    osd_init();
    if (boot_err) {
        char t[12] = "ERROR ";
        hex_string(t + 6, (uint32_t)boot_err & 0xFF, 2);
        osd_boot("32X BOOT BLOCK", t);
    } else {
        osd_boot("STARTING THE 32X", 0);
    }
    MARS16(0x04) = 0;           /* cartridge bank 0 in the 0x900000 window */
    MARS8(0x07) = 0;            /* RV = 0: the SH-2s may read the cartridge */

    flags = 0;
    if (vfs_list_movies() > 0)
        flags |= FL_VFS;
    COMM16(C_FLAGS) = flags;
    COMM16(C_REPLY) = 0;

    /* the 32X boot ROMs post M_OK / S_OK once they have loaded us */
    int waited = 0;
    while (COMM32(0x0) != M_OK || COMM32(0x4) != S_OK) {
        vdp_wait_vblank();
        if (++waited == 180)
            osd_boot("32X HAS NOT STARTED", "CHECK THE 32X AND THE CARTRIDGE");
    }
    COMM32(0x0) = 0;            /* release the master */
    osd_clear();

    /* COMM0 was 0 when we released the master; its first command is seq 1
     * (it may already be waiting: do not read the counters back from COMM) */
    last_ack = 0;
    unsigned last_seq = 0;
    int was_vb = 0;
    for (;;) {
        uint16_t st = VDP_CTRL;
        int vb = (st & VDP_STATUS_VB) != 0;
        if (vb && !was_vb) {
            uint16_t pad = pad_read();
            uint16_t ack = COMM16(C_CMD) >> 8;
            if (ack != last_ack) {      /* the SH-2 took the latched presses */
                last_ack = ack;
                latched = 0;
            }
            latched |= pad & ~last_pad;
            last_pad = pad;
            COMM16(C_PAD) = pad | latched;
            set_flag(FL_ODD, (st & VDP_STATUS_ODD) != 0);
            set_flag(FL_PAD3, !pad6);
            COMM16(C_VBL) = ++vblanks;
            /* test telemetry (tests/harness.c reads work RAM 0xFFFF00) */
            volatile uint16_t *t = (volatile uint16_t *)0xFFFF00;
            t[0] = 0x5354;              /* "ST" */
            t[1] = COMM16(C_CMD);
            t[2] = COMM16(C_P1);
            t[3] = COMM16(C_P2);
            t[4] = pad;
            t[5] = vblanks;
            t[6] = flags;
            t[7] = COMM16(C_P3);
        }
        was_vb = vb;

        uint16_t c = COMM16(C_CMD);
        unsigned seq = (c >> 4) & 15, cmd = c & 15;
        if (cmd == CMD_CRASH)
            sh2_crash();
        if (seq != last_seq) {
            last_seq = seq;
            unsigned r = run_command((int)cmd, COMM16(C_P1), COMM16(C_P2), COMM16(C_P3));
            COMM16(C_REPLY) = (uint16_t)(seq << 8 | (r & 0xFF));
        }
    }
}
