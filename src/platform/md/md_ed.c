/* md_ed.c - the Mega EverDrive PRO's SD card, from the 68000.
 *
 * The protocol is the EverDrive's documented I/O API (krikzz/mega-ed-pub,
 * edio/everdrive.c, MIT licence): commands go byte by byte through a FIFO at
 * 0xA130D1 as '+', ~'+', cmd, ~cmd and their arguments, most significant
 * byte first; answers come back through the same FIFO.
 *
 * With the 32X attached the EverDrive's I/O is only there with RV = 1 (the
 * 32X hands the cartridge bus to the 68000): with RV = 0 it reads as open bus
 * and writes are lost (console tests, 2026-11-04; ZERO SECTOR's EverDrive
 * save code sets RV = 1 the same way). With RV = 1 the 68000 cannot run code
 * from the cartridge, so this whole driver lives in work RAM (section
 * .ramtext, copied there with .data at start-up) and turns RV on only for
 * each operation. It must not call anything outside work RAM meanwhile (no
 * library calls: tools/check_ramtext.py checks the build). The SH-2s keep off
 * the cartridge while a command runs (they wait for the answer in SDRAM).
 *
 * File data goes straight into cartridge memory (command 0xCB): the
 * EverDrive's processor does the transfer while we wait here. The SH-2s then
 * read it from the cartridge.
 *
 * Every wait has a time limit, so a console without an EverDrive (or an
 * emulator) gets "no EverDrive" instead of hanging.
 */
#include "md.h"

#define RAM __attribute__((section(".ramtext"), noinline))

#define ED_FIFO     (*(volatile uint8_t *)0xA130D1)
#define ED_FSTAT    (*(volatile uint16_t *)0xA130D2)
#define ED_SYSSTAT  (*(volatile uint16_t *)0xA130D4)

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
#define STATUS_STROBE 0x08
#define FA_READ      0x01
#define DIR_OPT_SORTED 0x01
#define AT_DIR       0x10

#define ERR_TIMEOUT  0x50
#define ERR_STATUS   0x40

static int ed_ok;
static long spin_limit = 600000;    /* FIFO waits (about 1.5 s) */
/* (strings in work RAM too: string constants would be in the cartridge) */
static char path_root[] = "/";
static char path_movies[] = "/MOVIES";

/* ---- RV ---- */

static void RAM rv_on(void)
{
    __asm__ volatile ("bset #0,0xA15107" ::: "memory", "cc");
}

static void RAM rv_off(void)
{
    __asm__ volatile ("bclr #0,0xA15107" ::: "memory", "cc");
}

/* ---- the FIFO ---- */

static void RAM fifo_wr8(unsigned v) { ED_FIFO = (uint8_t)v; }
static void RAM fifo_wr16(unsigned v) { ED_FIFO = (uint8_t)(v >> 8); ED_FIFO = (uint8_t)v; }
static void RAM fifo_wr32(uint32_t v)
{
    ED_FIFO = (uint8_t)(v >> 24);
    ED_FIFO = (uint8_t)(v >> 16);
    ED_FIFO = (uint8_t)(v >> 8);
    ED_FIFO = (uint8_t)v;
}

/* Read len bytes; 0, or -1 if the EverDrive stopped answering. */
static int RAM fifo_rd(uint8_t *p, int len)
{
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

static void RAM fifo_flush(void)
{
    for (int i = 0; i < 4096 && (ED_FSTAT & FIFO_RXF_MSK); i++)
        (void)ED_FIFO;
}

static void RAM cmd_tx(unsigned cmd)
{
    fifo_wr8('+');
    fifo_wr8('+' ^ 0xFF);
    fifo_wr8(cmd);
    fifo_wr8(cmd ^ 0xFF);
}

static void RAM tx_string(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    fifo_wr16((unsigned)n);
    for (int i = 0; i < n; i++)
        fifo_wr8((uint8_t)s[i]);
}

/* The status command: 0 if the last command went well, else its error. */
static int RAM check_status(void)
{
    uint8_t b[2];
    cmd_tx(CMD_STATUS);
    if (fifo_rd(b, 2))
        return ERR_TIMEOUT;
    if (b[0] != 0xA5)
        return ERR_STATUS;
    return b[1];
}

/* Wait while the EverDrive's processor owns the cartridge bus (a file read
 * into cartridge memory). From ed_halt_app in the EverDrive's sample code:
 * post the status to wait for, start, then poll SYSSTAT until it reads
 * 0x55A0 with the strobe toggling and that status bit clear. */
static int RAM run_dma(void)
{
    ED_SYSSTAT = STATUS_CMD_OK;
    ED_FIFO = 0;                            /* run */
    for (long t = 0; t < 4000000; t++) {
        uint16_t s = ED_SYSSTAT;
        if ((s & 0xFFF8) != (0x55A0 | STATUS_STROBE)) continue;
        s = ED_SYSSTAT;
        if ((s & 0xFFF8) != 0x55A0) continue;
        if (s & STATUS_CMD_OK) continue;
        return 0;
    }
    return -1;
}

/* ---- the commands (each one RV = 1 only while it runs) ---- */

static int RAM detect_rv(void)
{
    fifo_flush();
    spin_limit = 100000;                    /* no EverDrive: give up quickly */
    uint8_t b[2];
    cmd_tx(CMD_STATUS);
    int r = fifo_rd(b, 2);
    spin_limit = 600000;
    /* (the second byte is the previous command's result, which can be an
     * error left over from the EverDrive's menu: only the key matters) */
    if (r || b[0] != 0xA5)
        return 0;
    /* the SD card: initialise it if the root does not list */
    cmd_tx(CMD_F_DIR_LD);
    fifo_wr8(0);
    tx_string(path_root);
    if (check_status()) {
        cmd_tx(CMD_DISK_INIT);
        if (check_status())
            return 0;
    }
    return 1;
}

int RAM ed_detect(void)
{
    rv_on();
    ed_ok = detect_rv();
    rv_off();
    return ed_ok;
}

static int RAM rx_name(char *out, int max)
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

static int RAM list_rv(void)
{
    cmd_tx(CMD_F_DIR_LD);
    fifo_wr8(DIR_OPT_SORTED);
    tx_string(path_movies);
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
    char *dst = movie_name[0];
    for (int i = 0; i < total; i++) {
        uint8_t resp, info[9];
        char name[NAME_LEN];
        if (fifo_rd(&resp, 1) || resp) return movie_count;
        if (fifo_rd(info, 9) || rx_name(name, NAME_LEN)) return -1;
        if (!(info[8] & AT_DIR) || name[0] == '.' || movie_count >= MAX_MOVIES)
            continue;
        for (int k = 0; k < NAME_LEN; k++) dst[k] = name[k];
        dst += NAME_LEN;
        movie_count++;
    }
    return movie_count;
}

/* The folders in /MOVIES, sorted. */
int RAM ed_list_movies(void)
{
    if (!ed_ok) return -1;
    rv_on();
    int r = list_rv();
    rv_off();
    return r;
}

int RAM ed_open(const char *path)
{
    if (!ed_ok) return ERR_STATUS;
    rv_on();
    cmd_tx(CMD_F_FCLOSE);
    (void)check_status();
    cmd_tx(CMD_F_FOPN);
    fifo_wr8(FA_READ);
    tx_string(path);
    int r = check_status();
    rv_off();
    return r;
}

int RAM ed_close(void)
{
    if (!ed_ok) return ERR_STATUS;
    rv_on();
    cmd_tx(CMD_F_FCLOSE);
    int r = check_status();
    rv_off();
    return r;
}

/* Read len bytes at offset of the open file into cartridge memory. */
int RAM ed_read_to_cart(uint32_t offset, uint32_t cart_addr, uint32_t len)
{
    if (!ed_ok) return ERR_STATUS;
    COMM16(C_FLAGS) |= FL_BUSY;
    rv_on();
    cmd_tx(CMD_F_FPTR);
    fifo_wr32(offset);
    int e = check_status();
    if (!e) {
        cmd_tx(CMD_F_FRD_MEM);
        fifo_wr32(cart_addr);
        fifo_wr32(len);
        e = run_dma() ? ERR_TIMEOUT : check_status();
    }
    rv_off();
    COMM16(C_FLAGS) &= (uint16_t)~FL_BUSY;
    return e;
}

/* ---- hardware test: the I/O registers with RV = 1 (md_halt.s) ---- */

extern const uint16_t ed_rv_code[], ed_rv_code_end[];
static uint16_t rv_ram[96];
static uint16_t rv_out[16];
typedef void (*rv_fn)(uint16_t *out);

/* Shows them on text rows row..row+2; returns 1 if the status command was
 * answered with 0xA5. */
int ed_rv_test(int row)
{
    int n = (int)(ed_rv_code_end - ed_rv_code);
    for (int i = 0; i < n && i < 96; i++) rv_ram[i] = ed_rv_code[i];
    for (int i = 0; i < 16; i++) rv_out[i] = 0xEEEE;
    ((rv_fn)(void *)rv_ram)(rv_out);
    char t[40], *p;
    for (int half = 0; half < 2; half++) {
        p = t;
        for (int i = 0; i < 4; i++) {
            hex_string(p, rv_out[half * 4 + i], 4);
            p += 4;
            *p++ = ' ';
        }
        *p = 0;
        osd_text(1, row + half, half ? "RV=1 D8:" : "RV=1 D0:", 0);
        osd_text(10, row + half, t, 0);
    }
    p = t;
    hex_string(p, rv_out[8], 4); p += 4; *p++ = ' ';
    hex_string(p, rv_out[9], 4); p += 4; *p++ = ' ';
    *p++ = 'A'; *p++ = 'N'; *p++ = 'S'; *p++ = ' ';
    hex_string(p, rv_out[10], 1); p += 1; *p++ = ':';
    hex_string(p, rv_out[11] & 0xFF, 2); p += 2;
    hex_string(p, rv_out[12] & 0xFF, 2); p += 2;
    *p = 0;
    int ok = rv_out[10] >= 1 && (rv_out[11] & 0xFF) == 0xA5;
    osd_text(1, row + 2, "RV=1 TIMER:", 0);
    osd_text(13, row + 2, t, ok);
    return ok;
}
