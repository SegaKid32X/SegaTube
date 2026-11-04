/* hw.h - SegaTube's 32X hardware layer (master and slave SH-2). */
#ifndef HW_H
#define HW_H

#include <stdint.h>

/* SH-2 side 32X registers (cache-through) */
#define MARS_ADAPTER    (*(volatile uint16_t *)0x20004000)
#define MARS_INTMASK    (*(volatile uint8_t *)0x20004001)
#define MARS_COMM(n)    (*(volatile uint16_t *)(0x20004020 + (n)))
#define MARS_PWM_CTRL   (*(volatile uint16_t *)0x20004030)
#define MARS_PWM_CYCLE  (*(volatile uint16_t *)0x20004032)
#define MARS_PWM_LEFT   (*(volatile uint16_t *)0x20004034)
#define MARS_PWM_RIGHT  (*(volatile uint16_t *)0x20004036)
#define MARS_PWM_MONO   (*(volatile uint16_t *)0x20004038)
#define MARS_VDP_MODE   (*(volatile uint16_t *)0x20004100)
#define MARS_VDP_SHIFT  (*(volatile uint16_t *)0x20004102)
#define MARS_VDP_FBCTL  (*(volatile uint16_t *)0x2000410A)
#define MARS_CRAM       ((volatile uint16_t *)0x20004200)
#define MARS_FRAMEBUF   ((volatile uint16_t *)0x24000000)
#define FB_BYTES        ((uint8_t *)0x24000000)

#define VDP_MODE_BLANK  0x0000
#define VDP_MODE_PACKED 0x0001
#define VDP_MODE_DIRECT 0x0002
#define VDP_MODE_PAL    0x8000  /* read: a PAL console */
#define FBCTL_VBLK      0x8000
#define FBCTL_FEN       0x0002  /* frame buffer busy */
#define FBCTL_FS        0x0001

/* Cartridge as the SH-2 sees it */
#define CART_CACHED     0x02000000u
#define CART_THROUGH    0x22000000u
#define CART_SHARED     0x300000u       /* keep in step with md.h */
#define CART_RING       0x380000u
#define SLOT_BYTES      0x8000u
#define SDRAM_THROUGH(p) ((void *)((uintptr_t)(p) | 0x20000000u))

/* ---- COMM protocol (see src/platform/md/md.h) ---- */
#define C_CMD       0x0
#define C_P1        0x2
#define C_P2        0x4
#define C_P3        0x6
#define C_REPLY     0x8
#define C_FLAGS     0xA
#define C_PAD       0xC
#define C_VBL       0xE

#define FL_ED       0x0002
#define FL_VFS      0x0004
#define FL_PAD3     0x0010
#define FL_BUSY     0x0020

#define CMD_NOP     0
#define CMD_SD_INIT 1
#define CMD_OPEN    2
#define CMD_READ    3
#define CMD_OSD     5
#define CMD_LIBRARY 6
#define CMD_MESSAGE 7
#define CMD_CLEAR   8
#define CMD_TITLE   9
#define CMD_PUTS    10
#define CMD_EDRV    13
#define CMD_HELP    14

#define KIND_INFO   0
#define KIND_FILM   1
#define KIND_FULL   2

#define OSD_BAR     0x0100
#define OSD_SCRUB   0x0200
#define OSD_HIDE    0x1000
#define OSD_HINTS   0x2000
#define OSD_ICON_PLAY  1
#define OSD_ICON_PAUSE 2
#define OSD_ICON_FF    3
#define OSD_ICON_REW   4
#define OSD_ICON_STOP  5

#define MSG_NONE    0
#define MSG_LOADING 1
#define MSG_NO_ED   2
#define MSG_NO_SD   3
#define MSG_NO_MOVIES 4
#define MSG_ERROR   5
#define MSG_END     6
#define MSG_NO_MODE 7

/* Pad bits */
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

/* ---- the frame buffers ---- */
#define FB_ROW_WORDS_P8 160
#define FB_ROW_WORDS_DC 320

void hw_init(void);
/* Set up the 32X VDP for pictures of the given depth (1: 256 colours,
 * 2: direct colour), pic_h rows shown from screen line pic_y, both buffers
 * cleared to black. */
void hw_video_mode(int depth, int pic_y, int pic_h);
/* A line table of our own: line L shows row rows[L] (256 entries, 224 used) */
void hw_line_table(const uint16_t *rows, int row_words);
/* First pixel of row r of the buffer the SH-2 can reach now */
uint8_t *hw_row(int r);
int  hw_row_bytes(void);
void hw_clear_back(void);
void hw_flip(void);                     /* show the other buffer at the next vblank, wait for it */
void hw_flip_to(int buffer);            /* show this buffer (0/1) from the next vblank, wait */
int  hw_back_buffer(void);              /* the buffer the SH-2 draws into (0/1) */
void hw_request_show(int buffer);       /* show this buffer from the next vblank (no wait) */
int  hw_shown(void);                    /* the buffer on screen (0/1) */
int  hw_in_vblank(void);
void hw_wait_vblank(void);
void hw_load_palette(const uint16_t *pal);

/* ---- the 68000 ---- */
int      md_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3);  /* blocking; the result */
void     md_cmd_start(int cmd, uint16_t p1, uint16_t p2, uint16_t p3);
int      md_cmd_done(void);             /* -1 while running, else the result */
uint16_t hw_pad(void);                  /* held or pressed since the last call */
uint16_t hw_flags(void);
uint16_t hw_vblanks(void);

/* ---- the slave SH-2 ---- */
typedef void (*slave_fn)(int arg);
void hw_slave_start(slave_fn fn, int arg);
int  hw_slave_wait(void);
void hw_cache_purge(void);
void hw_cache_purge_range(const void *p, uint32_t len);

/* ---- DMA channel 0 (master): bulk copies, e.g. cartridge -> SDRAM ---- */
void dma0_start(void *dst, const void *src, uint32_t len);  /* len multiple of 4 */
int  dma0_busy(void);
void dma0_wait(void);

/* ---- tracing: a step number the 68000 copies into the test telemetry
 * (work RAM 0xFFFF06) every frame; C_P2 is only read with a new command ---- */
#define TRACE(n) (MARS_COMM(C_P2) = (uint16_t)(n))

/* ---- timing ---- */
void hw_stage(int stage);

#endif
