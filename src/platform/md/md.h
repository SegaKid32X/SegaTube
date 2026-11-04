/* md.h - SegaTube's 68000 program: shared definitions.
 *
 * The 68000 owns the SD card (through the Mega EverDrive's command FIFO), the
 * pad, the Mega Drive VDP (on-screen display and text over the 32X picture)
 * and the TV's field parity in 480i. The master SH-2 asks for things through
 * the COMM registers (protocol below); everything about video and audio
 * happens on the SH-2s.
 */
#ifndef MD_H
#define MD_H

#include <stdint.h>

#define VDP_DATA    (*(volatile uint16_t *)0xC00000)
#define VDP_CTRL    (*(volatile uint16_t *)0xC00004)
#define VDP_CTRL32  (*(volatile uint32_t *)0xC00004)
#define PAD1_DATA   (*(volatile uint8_t *)0xA10003)
#define PAD1_CTRL   (*(volatile uint8_t *)0xA10009)

/* 32X system registers, 68000 side */
#define MARS8(o)    (*(volatile uint8_t *)(0xA15100 + (o)))
#define MARS16(o)   (*(volatile uint16_t *)(0xA15100 + (o)))
#define COMM16(o)   (*(volatile uint16_t *)(0xA15120 + (o)))
#define COMM32(o)   (*(volatile uint32_t *)(0xA15120 + (o)))

/* ---- COMM protocol (keep in step with src/platform/32x/hw.h) ----
 * SH-2 writes:
 *   C_CMD   ack << 8 | seq << 4 | cmd      a new seq = a new command;
 *                                          ack changes = pad latch consumed
 *   C_P1..3 parameters
 *   (during boot C_P3 holds the master's start-up stage; after a crash
 *    C_CMD = CMD_CRASH, C_P1:C_P2 = PC, C_P3 = 0xD slave / 0xE master)
 * 68000 writes:
 *   C_REPLY seq << 8 | result              result of the command with that seq
 *   C_FLAGS FL_* bits
 *   C_PAD   buttons held, or pressed since the last ack (PAD_*)
 *   C_VBL   vblank counter
 */
#define C_CMD       0x0
#define C_P1        0x2
#define C_P2        0x4
#define C_P3        0x6
#define C_REPLY     0x8
#define C_FLAGS     0xA
#define C_PAD       0xC
#define C_VBL       0xE

#define FL_ODD      0x0001  /* the field on screen now is odd (480i) */
#define FL_ED       0x0002  /* a Mega EverDrive answered */
#define FL_VFS      0x0004  /* movies are built into the ROM (test builds) */
#define FL_INTERLACE 0x0008 /* the VDP is in interlace mode */
#define FL_PAD3     0x0010  /* a 3-button pad */
#define FL_BUSY     0x0020  /* the cartridge bus is lent to the EverDrive */

#define CMD_NOP     0
#define CMD_SD_INIT 1   /* -> number of movies (0xFE: no EverDrive, 0xFF: error) */
#define CMD_OPEN    2   /* P1 movie, P2 kind | part << 8 (KIND_*) -> 0 or error */
#define CMD_READ    3   /* P1:P2 file offset, P3 slot << 8 | 256-byte units (0 = 128) */
#define CMD_VIDEO   4   /* P1 bit 0: interlace */
#define CMD_OSD     5   /* P1 OSD_* flags, P2 position s, P3 duration s */
#define CMD_LIBRARY 6   /* P1 first movie | mode << 12, P2 cursor, P3 duration s of the cursor's */
#define CMD_MESSAGE 7   /* P1 MSG_*, P2 value */
#define CMD_CLEAR   8   /* clear the text layer */
#define CMD_TITLE   9   /* P1 movie whose name the OSD shows */
#define CMD_PUTS    10  /* P1 row << 8 | col, P2 two characters, P3 1: highlighted */
#define CMD_EDDIAG  11  /* P1 text row: the EverDrive's registers on screen -> 1 if it answered */
#define CMD_EDBLIND 12  /* P1 text row: a transfer without status reads (PROBE.BIN) -> 1 if it worked */
#define CMD_CRASH   15

#define KIND_INFO   0
#define KIND_FILM   1
#define KIND_FULL   2
#define KIND_I480   3

/* Where CMD_READ puts data: cartridge memory beyond the ROM, which the
 * SH-2s read. Slots 0-15: the stream ring, 16-31: the shared area. */
#define CART_SHARED 0x300000
#define CART_RING   0x380000
#define SLOT_BYTES  0x8000

#define OSD_ICON    0x000F  /* 0 none, 1 play, 2 pause, 3 ff, 4 rew, 5 stop */
#define OSD_SPEED   0x00F0  /* x 2^n while scanning */
#define OSD_BAR     0x0100  /* show the time bar */
#define OSD_SCRUB   0x0200  /* the scrub bar: P2 is the cursor */
#define OSD_MODE    0x0C00  /* picture mode label: 0 film, 1 full, 2 480i */
#define OSD_HIDE    0x1000  /* nothing at all */
#define OSD_HINTS   0x2000  /* show the button hints */

#define MSG_NONE    0
#define MSG_LOADING 1
#define MSG_NO_ED   2
#define MSG_NO_SD   3
#define MSG_NO_MOVIES 4
#define MSG_ERROR   5   /* P2: code */
#define MSG_END     6
#define MSG_NO_MODE 7

/* Pad bits as published (1 = pressed) */
#define PAD_UP    0x01
#define PAD_DOWN  0x02
#define PAD_LEFT  0x04
#define PAD_RIGHT 0x08
#define PAD_B     0x10
#define PAD_C     0x20
#define PAD_A     0x40
#define PAD_START 0x80
#define PAD_Z     0x100
#define PAD_Y     0x200
#define PAD_X     0x400
#define PAD_MODE  0x800

/* The movie list (from the SD card, or built into the ROM) */
#define MAX_MOVIES  200
#define NAME_LEN    48
extern char movie_name[MAX_MOVIES][NAME_LEN];
extern int  movie_count;

/* md_main.c */
void vdp_wait_vblank(void);
void hex_string(char *out, uint32_t v, int digits);
extern volatile uint16_t vblanks;

/* md_ed.c: the Mega EverDrive */
int  ed_detect(void);                          /* 1 if it answers */
int  ed_list_movies(void);                     /* fills movie_name; count or -1 */
int  ed_open(const char *path);                /* 0 or error */
int  ed_close(void);
int  ed_read_to_cart(uint32_t offset, uint32_t cart_addr, uint32_t len);   /* 0 or error */
void ed_init_halt(void);
int  ed_diag(int row);
int  ed_blind_test(int row);

/* md_vfs.c: movies built into the ROM */
int  vfs_list_movies(void);                    /* count, 0 if none */

/* md_osd.c: text over the picture */
void osd_init(void);
void osd_clear(void);
void osd_text(int col, int row, const char *s, int high);
void osd_draw(uint16_t flags, uint16_t pos, uint16_t dur, int movie);
void osd_library(int first, int cursor, uint16_t dur, int mode);
void osd_message(int msg, int value);
void osd_boot(const char *line1, const char *line2);

#endif
