/* md_ed.c - the Mega EverDrive PRO's SD card, from the 68000.
 *
 * The protocol is the EverDrive's documented I/O API (krikzz/mega-ed-pub,
 * edio/everdrive.c, MIT licence): commands go byte by byte through a FIFO at
 * 0xA130D1 as '+', ~'+', cmd, ~cmd and their arguments, most significant
 * byte first; answers come back through the same FIFO. Every wait here has a
 * time limit, so a console without an EverDrive (or an emulator) just gets
 * "no EverDrive" instead of hanging.
 *
 * File data goes straight into cartridge memory (command 0xCB): the
 * EverDrive's processor does the transfer while the 68000 waits in work RAM
 * (md_halt.s). The SH-2s then read it from the cartridge.
 */
#include "md.h"

#define ED_FIFO     (*(volatile uint8_t *)0xA130D1)
#define ED_FSTAT    (*(volatile uint16_t *)0xA130D2)
#define ED_TIMER    (*(volatile uint16_t *)0xA130D6)

#define FIFO_CPU_RXF 0x8000     /* nothing for the CPU to read */
#define FIFO_RXF_MSK 0x07FF

#define CMD_STATUS   0x10
#define CMD_DISK_INIT 0xC0
#define CMD_F_DIR_LD 0xC5
#define CMD_F_DIR_SIZE 0xC6
#define CMD_F_DIR_GET 0xC8
#define CMD_F_FOPN   0xC9
#define CMD_F_FRD_MEM 0xCB
#define CMD_F_FCLOSE 0xCE
#define CMD_F_FPTR   0xCF

#define STATUS_CMD_OK 0x02
#define FA_READ      0x01
#define DIR_OPT_SORTED 0x01
#define AT_DIR       0x10

#define ERR_TIMEOUT  0x50
#define ERR_STATUS   0x40

static int ed_ok;
static long spin_limit = 600000;    /* FIFO waits (about 1.5 s) */

/* ---- the FIFO ---- */

static void fifo_wr(const void *data, int len)
{
    const uint8_t *p = data;
    while (len--)
        ED_FIFO = *p++;
}

static void fifo_wr8(unsigned v) { ED_FIFO = (uint8_t)v; }
static void fifo_wr16(unsigned v) { fifo_wr8(v >> 8); fifo_wr8(v); }
static void fifo_wr32(uint32_t v) { fifo_wr16(v >> 16); fifo_wr16(v & 0xFFFF); }

/* Read len bytes; 0, or -1 if the EverDrive stopped answering. */
static int fifo_rd(void *data, int len)
{
    uint8_t *p = data;
    long spins = 0;
    while (len) {
        int n = ED_FSTAT & FIFO_RXF_MSK;
        if (!n) {
            if (++spins > spin_limit)
                return -1;
            continue;
        }
        spins = 0;
        if (n > len) n = len;
        len -= n;
        while (n--)
            *p++ = ED_FIFO;
    }
    return 0;
}

static void fifo_flush(void)
{
    ED_FIFO = 0;
    ED_FIFO = 0;
    for (int i = 0; i < 4096 && (ED_FSTAT & FIFO_RXF_MSK); i++)
        (void)ED_FIFO;
}

static void cmd_tx(unsigned cmd)
{
    fifo_wr8('+');
    fifo_wr8('+' ^ 0xFF);
    fifo_wr8(cmd);
    fifo_wr8(cmd ^ 0xFF);
}

static void tx_string(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    fifo_wr16((unsigned)n);
    fifo_wr(s, n);
}

static int check_status(void)
{
    uint8_t b[2];
    cmd_tx(CMD_STATUS);
    if (fifo_rd(b, 2))
        return ERR_TIMEOUT;
    if (b[0] != 0xA5)
        return ERR_STATUS;
    return b[1];
}

/* ---- the RAM-resident wait ---- */

extern const uint16_t ed_halt_code[], ed_halt_code_end[];
static uint16_t halt_ram[48];
typedef int (*halt_fn)(int stat_req);

void ed_init_halt(void)
{
    int n = (int)(ed_halt_code_end - ed_halt_code);
    for (int i = 0; i < n && i < (int)(sizeof halt_ram / 2); i++)
        halt_ram[i] = ed_halt_code[i];
}

/* ---- commands ---- */

int ed_detect(void)
{
    ed_init_halt();
    /* an EverDrive answers the status command with 0xA5, 0x00 */
    fifo_flush();
    spin_limit = 100000;            /* no EverDrive: give up quickly */
    ed_ok = check_status() == 0;
    spin_limit = 600000;
    if (!ed_ok)
        return 0;
    /* the SD card: initialise it if the root does not list */
    cmd_tx(CMD_F_DIR_LD);
    fifo_wr8(0);
    tx_string("/");
    if (check_status()) {
        cmd_tx(CMD_DISK_INIT);
        if (check_status())
            return ed_ok = 0, 0;
    }
    return 1;
}

static int rx_name(char *out, int max)
{
    uint8_t b[2];
    if (fifo_rd(b, 2)) return -1;
    int n = b[0] << 8 | b[1];
    for (int i = 0; i < n; i++) {
        uint8_t c;
        if (fifo_rd(&c, 1)) return -1;
        if (i < max - 1) out[i] = (char)c;
    }
    out[n < max - 1 ? n : max - 1] = 0;
    return 0;
}

/* Hardware test: what the EverDrive's I/O registers show, on text rows
 * row..row+3. Returns 1 if a status command was answered. */
int ed_diag(int row)
{
    char t[48], *p;
    volatile uint16_t *io = (volatile uint16_t *)0xA130D0;
    /* the eight registers, as words, four to a line */
    for (int half = 0; half < 2; half++) {
        p = t;
        for (int i = 0; i < 4; i++) {
            hex_string(p, io[half * 4 + i], 4);
            p += 4;
            *p++ = ' ';
        }
        *p = 0;
        osd_text(1, row + half, half ? "A130D8:" : "A130D0:", 0);
        osd_text(9, row + half, t, 0);
    }
    row++;
    /* the 1 ms timer: does it count? */
    uint16_t t0 = ED_TIMER;
    for (int i = 0; i < 6; i++) vdp_wait_vblank();
    uint16_t t1 = ED_TIMER;
    p = t;
    hex_string(p, t0, 4); p += 4; *p++ = ' ';
    hex_string(p, t1, 4); p += 4; *p = 0;
    osd_text(1, row + 1, "TIMER (100MS):", 0);
    osd_text(16, row + 1, t, (uint16_t)(t1 - t0) > 50 && (uint16_t)(t1 - t0) < 200);
    /* the status command, waiting longer */
    ed_init_halt();
    fifo_flush();
    spin_limit = 300000;
    uint8_t b[4] = { 0, 0, 0, 0 };
    cmd_tx(CMD_STATUS);
    int e1 = fifo_rd(b, 2);
    cmd_tx(0x40);                       /* status2: key, protocol, device id, status */
    uint8_t c[4] = { 0, 0, 0, 0 };
    int e2 = fifo_rd(c, 4);
    spin_limit = 600000;
    p = t;
    p = e1 ? (hex_string(p, 0xDEAD, 4), p + 4) : (hex_string(p, (uint32_t)b[0] << 8 | b[1], 4), p + 4);
    *p++ = ' ';
    if (e2) { hex_string(p, 0xDEAD, 4); p += 4; }
    else { for (int i = 0; i < 4; i++) { hex_string(p, c[i], 2); p += 2; } }
    *p = 0;
    osd_text(1, row + 2, "STATUS:", 0);
    osd_text(16, row + 2, t, !e1 && b[0] == 0xA5);
    return !e1 && b[0] == 0xA5;
}

/* Hardware test: a transfer without reading anything back. Asks the
 * EverDrive to read /PROBE.BIN (a copy of this ROM the user put on the SD
 * card) at some offset into cartridge memory at 0x300000, then compares it
 * with the ROM itself. Shows the result on text rows row..row+2. */
extern const uint16_t ed_blind_code[], ed_blind_code_end[];
static uint16_t blind_ram[48];
typedef void (*blind_fn)(int frames);

#define CART_LO     ((const volatile uint8_t *)0x880000)    /* cartridge 0 .. 512 KiB */
#define BANK_WIN    ((const volatile uint8_t *)0x900000)

static int compare_blind(uint32_t off, uint32_t len)
{
    MARS16(0x04) = 3;                   /* 0x900000 = cartridge 0x300000 */
    int same = 0;
    for (uint32_t i = 0; i < len; i++)
        if (BANK_WIN[i] == CART_LO[off + i]) same++;
    MARS16(0x04) = 0;
    return same;
}

int ed_blind_test(int row)
{
    char t[40], *p;
    int n = (int)(ed_blind_code_end - ed_blind_code);
    for (int i = 0; i < n && i < 48; i++) blind_ram[i] = ed_blind_code[i];
    blind_fn wait_run = (blind_fn)(void *)blind_ram;
    /* the MARS id the 32X shows at A130EC: do reads here work at all? */
    volatile uint16_t *id = (volatile uint16_t *)0xA130EC;
    p = t;
    hex_string(p, id[0], 4); p += 4; *p++ = ' ';
    hex_string(p, id[1], 4); p += 4; *p = 0;
    osd_text(1, row, "A130EC (MARS):", 0);
    osd_text(16, row, t, id[0] == 0x4D41);
    /* a different offset each run, so old data cannot pass */
    uint32_t off = 0x1000 + (uint32_t)(vblanks & 0x3F) * 0x1000;
    uint32_t len = 0x1000;
    cmd_tx(CMD_F_FOPN);
    fifo_wr8(FA_READ);
    tx_string("/PROBE.BIN");
    for (int i = 0; i < 10; i++) vdp_wait_vblank();
    cmd_tx(CMD_F_FPTR);
    fifo_wr32(off);
    for (int i = 0; i < 3; i++) vdp_wait_vblank();
    cmd_tx(CMD_F_FRD_MEM);
    fifo_wr32(CART_SHARED);
    fifo_wr32(len);
    wait_run(30);                       /* half a second for 4 KiB */
    int same = compare_blind(off, len);
    p = t;
    hex_string(p, (uint32_t)same, 4);
    p += 4;
    *p++ = '/';
    hex_string(p, len, 4);
    osd_text(1, row + 1, "BLIND READ:", 0);
    osd_text(16, row + 1, same == (int)len ? "WORKS" : same > (int)len / 2 ? "PARTLY" : "NO DATA", same == (int)len);
    osd_text(25, row + 1, t, 0);
    if (same != (int)len) return 0;
    /* how fast: 32 KiB with shorter and shorter waits */
    int best = 0;
    for (int frames = 8; frames >= 1; frames /= 2) {
        off += 0x8000;
        cmd_tx(CMD_F_FPTR);
        fifo_wr32(off);
        for (int i = 0; i < 2; i++) vdp_wait_vblank();
        cmd_tx(CMD_F_FRD_MEM);
        fifo_wr32(CART_SHARED);
        fifo_wr32(0x8000);
        wait_run(frames);
        if (compare_blind(off, 0x8000) == 0x8000) best = frames;
        else break;
    }
    p = t;
    p = (char *)"32K IN ";
    char u[24], *q = u;
    while (*p) *q++ = *p++;
    *q++ = (char)('0' + best);
    const char *f = " FRAMES OR LESS";
    while (*f) *q++ = *f++;
    *q = 0;
    osd_text(1, row + 2, "BLIND SPEED:", 0);
    osd_text(16, row + 2, best ? u : "OVER 8 FRAMES", best > 0);
    return 1;
}

/* The folders in /MOVIES, sorted. */
int ed_list_movies(void)
{
    if (!ed_ok) return -1;
    cmd_tx(CMD_F_DIR_LD);
    fifo_wr8(DIR_OPT_SORTED);
    tx_string("/MOVIES");
    if (check_status())
        return 0;                       /* no MOVIES folder: no movies */
    uint8_t b[2];
    cmd_tx(CMD_F_DIR_SIZE);
    if (fifo_rd(b, 2)) return -1;
    int total = b[0] << 8 | b[1];
    if (total > 1000) total = 1000;
    cmd_tx(CMD_F_DIR_GET);
    fifo_wr16(0);
    fifo_wr16((unsigned)total);
    fifo_wr16(NAME_LEN - 1);
    movie_count = 0;
    for (int i = 0; i < total; i++) {
        uint8_t resp, info[9];
        char name[NAME_LEN];
        if (fifo_rd(&resp, 1) || resp) return movie_count;
        if (fifo_rd(info, 9) || rx_name(name, NAME_LEN)) return -1;
        if (!(info[8] & AT_DIR) || name[0] == '.' || movie_count >= MAX_MOVIES)
            continue;
        for (int k = 0; k < NAME_LEN; k++) movie_name[movie_count][k] = name[k];
        movie_count++;
    }
    return movie_count;
}

int ed_open(const char *path)
{
    if (!ed_ok) return ERR_STATUS;
    cmd_tx(CMD_F_FCLOSE);
    (void)check_status();
    cmd_tx(CMD_F_FOPN);
    fifo_wr8(FA_READ);
    tx_string(path);
    return check_status();
}

int ed_close(void)
{
    if (!ed_ok) return ERR_STATUS;
    cmd_tx(CMD_F_FCLOSE);
    return check_status();
}

/* Read len bytes at offset of the open file into cartridge memory. */
int ed_read_to_cart(uint32_t offset, uint32_t cart_addr, uint32_t len)
{
    if (!ed_ok) return ERR_STATUS;
    cmd_tx(CMD_F_FPTR);
    fifo_wr32(offset);
    int e = check_status();
    if (e) return e;
    COMM16(C_FLAGS) |= FL_BUSY;
    cmd_tx(CMD_F_FRD_MEM);
    fifo_wr32(cart_addr);
    fifo_wr32(len);
    int t = ((halt_fn)(void *)halt_ram)(STATUS_CMD_OK);
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    if (t)
        return ERR_TIMEOUT;
    return check_status();
}
