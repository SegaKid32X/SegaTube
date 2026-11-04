/* library.c - the movie library: pages of four posters, two by two.
 *
 * The 32X shows the posters in direct colour; the 68000 writes the titles,
 * the page number and the hints on its own layer (CMD_LIBRARY). The line
 * table gives each poster line a row of its own (direct colour mode only has
 * room for 204 rows): lines 22-98 and 118-194, the rest show a black row.
 */
#include <string.h>
#include "hw.h"
#include "io.h"
#include "player.h"

#define POSTER_W    STV_POSTER_W
#define POSTER_H    STV_POSTER_H
#define TOP0        22          /* first line of each band (2 lines of frame above the poster) */
#define TOP1        118
#define BAND        77
#define BLACK_ROW   (2 * BAND)

static const int px[4] = { 24, 168, 24, 168 };
static int count;
static info_t infos[4];
static int info_ok[4];
static uint16_t (*posters)[POSTER_W * POSTER_H];    /* in the stream buffer (idle here) */

static void library_mode(void)
{
    static uint16_t rows[256];
    for (int l = 0; l < 256; l++) {
        if (l >= TOP0 && l < TOP0 + BAND) rows[l] = (uint16_t)(l - TOP0);
        else if (l >= TOP1 && l < TOP1 + BAND) rows[l] = (uint16_t)(BAND + l - TOP1);
        else rows[l] = BLACK_ROW;
    }
    for (int k = 0; k < 2; k++) {
        hw_line_table(rows, FB_ROW_WORDS_DC);
        hw_clear_back();
        hw_flip();
    }
    MARS_VDP_MODE = VDP_MODE_DIRECT;
}

static uint16_t *row16(int band, int y)
{
    return (uint16_t *)(void *)(FB_BYTES + 0x200 + (band * BAND + y) * 640);
}

/* Draw the page into the buffer the SH-2 reaches now. */
static void draw_page(int first, int cursor)
{
    for (int i = 0; i < 4; i++) {
        int band = i >> 1, x = px[i];
        int m = first + i;
        uint16_t frame = m == cursor ? 0x03FF : 0x0000;    /* yellow (BGR555) */
        for (int y = 0; y < BAND; y++) {
            uint16_t *r = row16(band, y);
            if (m >= count) {
                for (int c = -2; c < POSTER_W + 2; c++) r[x + c] = 0;
                continue;
            }
            int py = y - 2;
            for (int c = -2; c < POSTER_W + 2; c++) {
                int edge = py < 0 || py >= POSTER_H || c < 0 || c >= POSTER_W;
                r[x + c] = edge ? (py < POSTER_H + 2 ? frame : 0)
                                : (info_ok[i] ? posters[i][py * POSTER_W + c] : 0x2108);
            }
        }
    }
}

static void show_page(int first, int cursor)
{
    draw_page(first, cursor);
    hw_flip();
    draw_page(first, cursor);
}

static void load_page(int first)
{
    posters = (uint16_t (*)[POSTER_W * POSTER_H])(void *)stream_scratch();
    for (int i = 0; i < 4; i++) {
        int m = first + i;
        info_ok[i] = 0;
        if (m >= count) continue;
        if (info_read(m, &infos[i]) == 0 && info_poster(m, &infos[i], posters[i]) == 0)
            info_ok[i] = 1;
    }
}

/* The controls pop-up (MODE, or B for a 3-button pad) until a button is pressed */
static void help(void)
{
    md_cmd(CMD_HELP, 1, 0, 0);
    do {
        hw_wait_vblank();
        pad_poll();
    } while (!pad_pressed);
    md_cmd(CMD_HELP, 0, 0, 0);
}

void library_run(void)
{
    int kind = KIND_FILM;
    for (;;) {
        md_cmd(CMD_CLEAR, 0, 0, 0);
        md_cmd(CMD_MESSAGE, MSG_LOADING, 0, 0);
        count = io_init();
        md_cmd(CMD_MESSAGE, MSG_NONE, 0, 0);
        if (count <= 0) {
            int msg = count == -1 ? MSG_NO_ED : count == -2 ? MSG_NO_SD : MSG_NO_MOVIES;
            md_cmd(CMD_MESSAGE, (uint16_t)msg, 0, 0);
            /* START: look again */
            for (;;) {
                hw_wait_vblank();
                pad_poll();
                if (pad_pressed & (PAD_START | PAD_A)) break;
            }
            continue;
        }
        library_mode();
        int cursor = 0, first = -1;
        for (;;) {
            int page = cursor / 4 * 4;
            if (page != first) {
                first = page;
                load_page(first);
            }
            show_page(first, cursor);
            int i = cursor - first;
            uint32_t dur = info_ok[i] ? infos[i].duration_ms / 1000 : 0;
            md_cmd(CMD_LIBRARY, (uint16_t)(first | (kind - KIND_FILM) << 12), (uint16_t)cursor,
                   (uint16_t)(dur > 65535 ? 65535 : dur));
            int go = 0;
            for (;;) {
                hw_wait_vblank();
                pad_poll();
                uint16_t p = pad_pressed;
                if (!p) continue;
                int c = cursor;
                if (p & PAD_RIGHT) c++;
                if (p & PAD_LEFT) c--;
                if (p & PAD_DOWN) c += 2;
                if (p & PAD_UP) c -= 2;
                if (c < 0) c = 0;
                if (c >= count) c = count - 1;
                if (p & PAD_C) {
                    kind = kind == KIND_FILM ? KIND_FULL : KIND_FILM;
                    break;
                }
                if (p & (PAD_MODE | PAD_B)) { help(); break; }
                if (p & (PAD_A | PAD_START)) { go = 1; break; }
                if (c != cursor) { cursor = c; break; }
            }
            if (go) {
                md_cmd(CMD_CLEAR, 0, 0, 0);
                play_movie(cursor, kind);
                pad_flush();
                md_cmd(CMD_CLEAR, 0, 0, 0);
                library_mode();
                first = -1;
            }
        }
    }
}
