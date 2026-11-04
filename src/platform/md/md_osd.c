/* md_osd.c - SegaTube's text layer: the Mega Drive VDP's plane A, over the
 * 32X picture.
 *
 * Plane A is tile 0 (transparent) except where text is drawn, so the 32X
 * video shows everywhere else (the 32X priority bit is 0: the Mega Drive
 * layer is in front where it is not transparent). The SH-2s never draw text;
 * they ask for it (CMD_OSD, CMD_LIBRARY, CMD_MESSAGE) and the 68000 redraws.
 */
#include "md.h"
#include "osd_tiles.h"

extern const uint8_t osd_tiles[], osd_tiles_end[];
extern const uint16_t osd_palette[];

#define PLANE_A     0xC000
#define COLS        40
#define ROWS        28

static void vram_addr(uint16_t addr)
{
    VDP_CTRL32 = 0x40000000u | ((uint32_t)(addr & 0x3FFF) << 16) | (addr >> 14);
}

static void put(int col, int row, int tile)
{
    vram_addr((uint16_t)(PLANE_A + (row * 64 + col) * 2));
    VDP_DATA = (uint16_t)(0x8000 | tile);
}

void osd_init(void)
{
    /* clear all of VRAM, then the tiles and palette line 0 */
    vram_addr(0);
    for (int i = 0; i < 0x8000; i++) VDP_DATA = 0;
    vram_addr(0);
    const uint16_t *t = (const uint16_t *)(const void *)osd_tiles;
    int words = (int)(osd_tiles_end - osd_tiles) / 2;
    for (int i = 0; i < words; i++) VDP_DATA = t[i];
    VDP_CTRL32 = 0xC0000000u;
    for (int i = 0; i < 16; i++) VDP_DATA = osd_palette[i];
}

void osd_clear(void)
{
    vram_addr(PLANE_A);
    for (int i = 0; i < 64 * ROWS; i++) VDP_DATA = 0;
}

static void clear_rows(int r0, int r1)
{
    for (int r = r0; r < r1; r++) {
        vram_addr((uint16_t)(PLANE_A + r * 128));
        for (int c = 0; c < 64; c++) VDP_DATA = 0;
    }
}

void osd_text(int col, int row, const char *s, int high)
{
    if (row < 0 || row >= ROWS) return;
    vram_addr((uint16_t)(PLANE_A + (row * 64 + col) * 2));
    for (; *s && col < COLS; s++, col++) {
        int ch = (uint8_t)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch >= 96) ch = ' ';
        VDP_DATA = (uint16_t)(0x8000 | ((high ? T_HIGH : T_TEXT) + ch - 32));
    }
}

static void fill_row(int row, int tile)
{
    vram_addr((uint16_t)(PLANE_A + row * 128));
    for (int c = 0; c < COLS; c++) VDP_DATA = (uint16_t)(0x8000 | tile);
}

static void time_string(char *o, unsigned s)
{
    unsigned h = s / 3600, m = s / 60 % 60;
    s %= 60;
    o[0] = (char)('0' + h / 10 % 10); o[1] = (char)('0' + h % 10); o[2] = ':';
    o[3] = (char)('0' + m / 10);      o[4] = (char)('0' + m % 10); o[5] = ':';
    o[6] = (char)('0' + s / 10);      o[7] = (char)('0' + s % 10); o[8] = 0;
}

static void center(int row, const char *s, int high)
{
    int n = 0;
    while (s[n]) n++;
    osd_text((COLS - n) / 2, row, s, high);
}

/* The bar: columns BAR_COL .. BAR_COL + BAR_LEN - 1 of row 25 (the SH-2
 * places the scrub thumbnail above the cursor using the same numbers). */
#define BAR_ROW     25
#define BAR_COL     11
#define BAR_LEN     20

void osd_draw(uint16_t flags, uint16_t pos, uint16_t dur, int movie)
{
    static uint16_t last_flags = 0xFFFF;
    if (flags & OSD_HIDE) {
        if (last_flags != 0xFFFF && !(last_flags & OSD_HIDE))
            clear_rows(23, ROWS);
        last_flags = flags;
        return;
    }
    if ((last_flags ^ flags) & (OSD_HIDE | OSD_HINTS | OSD_SCRUB))
        clear_rows(23, ROWS);
    last_flags = flags;
    static const char *const mode_name[4] = { "FILM", "FULL", "480I", "" };
    char t[12];
    /* row 24: title and mode, on the panel */
    fill_row(24, T_PANEL);
    fill_row(25, T_PANEL);
    fill_row(26, T_PANEL);
    if (movie >= 0 && movie < movie_count) {
        char name[29];
        int i = 0;
        for (; i < 28 && movie_name[movie][i]; i++) name[i] = movie_name[movie][i];
        name[i] = 0;
        osd_text(1, 24, name, 0);
    }
    osd_text(35, 24, mode_name[(flags & OSD_MODE) >> 10], 1);
    /* row 25: icon, time, bar, duration */
    static const uint8_t icons[6] = { 0, T_PLAY, T_PAUSE, T_FF, T_REW, T_STOP };
    int ic = flags & OSD_ICON;
    put(1, BAR_ROW, ic && ic < 6 ? icons[ic] : T_PANEL);
    time_string(t, pos);
    osd_text(2, BAR_ROW, t, 0);
    int fill = dur ? (int)((uint32_t)pos * (BAR_LEN * 8) / dur) : 0;
    if (fill > BAR_LEN * 8) fill = BAR_LEN * 8;
    for (int i = 0; i < BAR_LEN; i++) {
        int tile = i * 8 + 4 <= fill ? T_BAR_FILL : T_BAR_EMPTY;
        if ((flags & OSD_SCRUB) && i == (fill >= BAR_LEN * 8 ? BAR_LEN - 1 : fill / 8)) tile = T_BAR_CURSOR;
        put(BAR_COL + i, BAR_ROW, tile);
    }
    time_string(t, dur);
    osd_text(32, BAR_ROW, t, 0);
    /* row 26: speed or hints */
    int sp = (flags & OSD_SPEED) >> 4;
    if (sp && (ic == 3 || ic == 4)) {
        t[0] = 'X';
        int v = 1 << sp, k = 1;
        if (v >= 10) t[k++] = (char)('0' + v / 10);
        t[k++] = (char)('0' + v % 10);
        t[k] = 0;
        osd_text(2, 26, ic == 3 ? "FAST FORWARD" : "REWIND", 1);
        osd_text(15, 26, t, 1);
    } else if (flags & OSD_SCRUB) {
        osd_text(2, 26, "LEFT/RIGHT SEEK  A GO  B CANCEL", 0);
    } else if (flags & OSD_HINTS) {
        osd_text(1, 26, "START PAUSE <> SCAN DOWN SEEK C MODE", 0);
    }
}

/* The library: a 2 x 2 page of posters (drawn by the 32X), titles under them */
void osd_library(int first, int cursor, uint16_t dur, int mode)
{
    char t[24];
    osd_clear();
    center(1, "SEGATUBE", 1);
    for (int i = 0; i < 4; i++) {
        int m = first + i;
        if (m >= movie_count) break;
        char name[17];
        int k = 0;
        for (; k < 16 && movie_name[m][k]; k++) name[k] = movie_name[m][k];
        name[k] = 0;
        int col = (i & 1) ? 21 : 3, row = (i & 2) ? 25 : 13;
        osd_text(col, row, name, m == cursor);
    }
    /* row 26: page and the cursor's length */
    int pages = (movie_count + 3) / 4, page = first / 4 + 1;
    char *p = t;
    *p++ = 'P'; *p++ = 'A'; *p++ = 'G'; *p++ = 'E'; *p++ = ' ';
    if (page >= 10) *p++ = (char)('0' + page / 10 % 10);
    *p++ = (char)('0' + page % 10);
    *p++ = '/';
    if (pages >= 10) *p++ = (char)('0' + pages / 10 % 10);
    *p++ = (char)('0' + pages % 10);
    *p = 0;
    osd_text(1, 26, t, 0);
    if (dur) {
        time_string(t, dur);
        osd_text(31, 26, t, 0);
    }
    static const char *const modes[3] = { "C MODE: FILM", "C MODE: FULL", "C MODE: 480I" };
    osd_text(12, 26, modes[mode < 3 ? mode : 0], 0);
}

void osd_message(int msg, int value)
{
    static const char *const text[8] = {
        "", "LOADING...", "NO EVERDRIVE FOUND", "NO SD CARD", "NO MOVIES IN /MOVIES",
        "READ ERROR", "THE END", "MODE NOT IN THIS MOVIE"
    };
    clear_rows(12, 16);
    if (msg <= 0 || msg >= 8) return;
    center(13, text[msg], 1);
    if (msg == MSG_ERROR) {
        char h[8];
        hex_string(h, (uint32_t)value, 4);
        center(14, h, 0);
    }
}

void osd_boot(const char *line1, const char *line2)
{
    clear_rows(10, 16);
    center(10, "SEGATUBE", 1);
    if (line1) center(12, line1, 0);
    if (line2) center(14, line2, 0);
}
