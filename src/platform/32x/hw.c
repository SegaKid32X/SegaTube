/* hw.c - SegaTube's 32X hardware layer (from ZERO SECTOR's, reworked). */
#include "hw.h"

#define SPIN_LIMIT 4000000

/* ------------------------------------------------------------ frame buffers */

static int row_bytes = 320;     /* 320 (256 colours) or 640 (direct colour) */

void hw_stage(int stage)
{
    MARS_COMM(C_P3) = (uint16_t)stage;
}

static void wait_fb_ready(void)
{
    for (int i = 0; i < SPIN_LIMIT && (MARS_VDP_FBCTL & FBCTL_FEN); i++)
        ;
}

void hw_wait_vblank(void)
{
    int i = 0;
    while (i++ < SPIN_LIMIT && (MARS_VDP_FBCTL & FBCTL_VBLK))
        ;
    while (i++ < SPIN_LIMIT && !(MARS_VDP_FBCTL & FBCTL_VBLK))
        ;
}

int hw_back_buffer(void)
{
    return (MARS_VDP_FBCTL & FBCTL_FS) ? 0 : 1;
}

void hw_request_show(int b)
{
    MARS_VDP_FBCTL = (uint16_t)(b & 1);
}

int hw_shown(void)
{
    return MARS_VDP_FBCTL & FBCTL_FS;
}

int hw_in_vblank(void)
{
    return (MARS_VDP_FBCTL & FBCTL_VBLK) != 0;
}

/* Show buffer b from the next vblank; return at the start of that vblank. */
void hw_flip_to(int b)
{
    uint16_t want = (uint16_t)(b & 1);
    if ((MARS_VDP_FBCTL & FBCTL_FS) == want) {
        hw_wait_vblank();
        return;
    }
    MARS_VDP_FBCTL = want;
    for (int i = 0; i < SPIN_LIMIT && (MARS_VDP_FBCTL & FBCTL_FS) != want; i++)
        ;
    wait_fb_ready();
}

void hw_flip(void)
{
    hw_flip_to((MARS_VDP_FBCTL & FBCTL_FS) ^ 1);
}

uint8_t *hw_row(int r)
{
    return FB_BYTES + 0x200 + r * row_bytes;
}

int hw_row_bytes(void)
{
    return row_bytes;
}

void hw_line_table(const uint16_t *rows, int row_words)
{
    volatile uint16_t *fb = MARS_FRAMEBUF;
    wait_fb_ready();
    for (int line = 0; line < 256; line++)
        fb[line] = (uint16_t)(0x100 + rows[line < 224 ? line : 223] * row_words);
}

void hw_clear_back(void)
{
    volatile uint32_t *px = (volatile uint32_t *)(void *)(FB_BYTES + 0x200);
    wait_fb_ready();
    for (int i = 0; i < (0x10000 - 0x100) / 2; i++)
        px[i] = 0;
}

void hw_video_mode(int depth, int pic_y, int pic_h)
{
    static uint16_t rows[256];
    row_bytes = depth == 2 ? 640 : 320;
    for (int l = 0; l < 256; l++)
        rows[l] = (uint16_t)(l >= pic_y && l < pic_y + pic_h ? l - pic_y : pic_h);
    for (int k = 0; k < 2; k++) {
        hw_line_table(rows, row_bytes / 2);
        hw_clear_back();
        hw_flip();
    }
    MARS_VDP_MODE = depth == 2 ? VDP_MODE_DIRECT : VDP_MODE_PACKED;
    MARS_VDP_SHIFT = 0;
}

void hw_load_palette(const uint16_t *pal)
{
    for (int i = 0; i < 256; i++)
        MARS_CRAM[i] = pal[i];
}

void hw_init(void)
{
    hw_stage(8);
    MARS_VDP_MODE = VDP_MODE_BLANK;
    volatile uint32_t *dmaor = (volatile uint32_t *)0xFFFFFFB0;
    *dmaor = 1;                                 /* DMAC on (channel 0: copies) */
    hw_stage(9);
}

/* ------------------------------------------------------------ the 68000 */

static uint16_t cmd_seq, pad_ack;

void md_cmd_start(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    cmd_seq = (uint16_t)((cmd_seq + 1) & 15);
    MARS_COMM(C_P1) = p1;
    MARS_COMM(C_P2) = p2;
    MARS_COMM(C_P3) = p3;
    MARS_COMM(C_CMD) = (uint16_t)(pad_ack << 8 | cmd_seq << 4 | (cmd & 15));
}

int md_cmd_done(void)
{
    uint16_t r = MARS_COMM(C_REPLY);
    if (((r >> 8) & 15) != cmd_seq)
        return -1;
    return r & 0xFF;
}

int md_cmd(int cmd, uint16_t p1, uint16_t p2, uint16_t p3)
{
    md_cmd_start(cmd, p1, p2, p3);
    int r;
    while ((r = md_cmd_done()) < 0)
        ;
    return r;
}

uint16_t hw_pad(void)
{
    uint16_t v = MARS_COMM(C_PAD);
    pad_ack = (uint16_t)((pad_ack + 1) & 0xFF);
    MARS_COMM(C_CMD) = (uint16_t)(pad_ack << 8 | (MARS_COMM(C_CMD) & 0xFF));
    return v & 0x0FFF;
}

uint16_t hw_flags(void)
{
    return MARS_COMM(C_FLAGS);
}

uint16_t hw_vblanks(void)
{
    return MARS_COMM(C_VBL);
}

/* ------------------------------------------------------------ caches, DMA */

void hw_cache_purge(void)
{
    volatile uint8_t *ccr = (volatile uint8_t *)0xFFFFFE92;
    *ccr = 0x10;    /* purge + disable */
    *ccr = 0x11;    /* purge + enable */
}

/* Associative purge: writing to 0x40000000 | address drops that line. */
void hw_cache_purge_range(const void *p, uint32_t len)
{
    uintptr_t a = (uintptr_t)p & 0x1FFFFFF0u, end = ((uintptr_t)p & 0x1FFFFFFFu) + len;
    if (len >= 4096) {
        hw_cache_purge();
        return;
    }
    for (; a < end; a += 16)
        *(volatile uint32_t *)(0x40000000u | a) = 0;
}

#define DMA_SAR0   (*(volatile uint32_t *)0xFFFFFF80)
#define DMA_DAR0   (*(volatile uint32_t *)0xFFFFFF84)
#define DMA_TCR0   (*(volatile uint32_t *)0xFFFFFF88)
#define DMA_CHCR0  (*(volatile uint32_t *)0xFFFFFF8C)
#define DMA_DRCR0  (*(volatile uint8_t *)0xFFFFFE71)

/* src and dst through the cache-through mirrors; the caller purges the
 * destination from its cache before reading it. */
void dma0_start(void *dst, const void *src, uint32_t len)
{
    (void)DMA_CHCR0;
    DMA_CHCR0 = 0;
    DMA_DRCR0 = 0;
    DMA_SAR0 = (uint32_t)(uintptr_t)src | 0x20000000u;
    DMA_DAR0 = (uint32_t)(uintptr_t)dst | 0x20000000u;
    DMA_TCR0 = len >> 2;
    /* dst++, src++, longs, auto request, cycle steal, enabled */
    DMA_CHCR0 = 0x5A01;
}

int dma0_busy(void)
{
    uint32_t c = DMA_CHCR0;
    return (c & 1) && !(c & 2);
}

void dma0_wait(void)
{
    for (int i = 0; i < SPIN_LIMIT && dma0_busy(); i++)
        ;
    (void)DMA_CHCR0;
    DMA_CHCR0 = 0;
}

/* ------------------------------------------------------------ the slave */

typedef struct {
    volatile slave_fn fn;
    volatile int      arg;
    volatile int      busy;
} mailbox_t;

static mailbox_t mailbox_storage __attribute__((aligned(16)));
#define MAILBOX ((mailbox_t *)SDRAM_THROUGH(&mailbox_storage))

void hw_slave_start(slave_fn fn, int arg)
{
    mailbox_t *mb = MAILBOX;
    hw_slave_wait();
    mb->arg = arg;
    mb->busy = 1;
    mb->fn = fn;
}

int hw_slave_busy(void)
{
    return MAILBOX->busy != 0;
}

int hw_slave_wait(void)
{
    mailbox_t *mb = MAILBOX;
    int i = 0;
    for (; i < SPIN_LIMIT * 4 && mb->busy; i++)
        ;
    return i;
}

/* Slave SH-2 entry (crt0.s): run the jobs the master posts. */
void slave_main(void)
{
    mailbox_t *mb = MAILBOX;
    for (;;) {
        slave_fn fn = mb->fn;
        if (!fn) {
            for (int d = 0; d < 64; d++)    /* keep off the bus while idle */
                __asm__ volatile ("nop");
            continue;
        }
        mb->fn = 0;
        hw_cache_purge();       /* see the master's latest SDRAM writes */
        fn(mb->arg);
        mb->busy = 0;
    }
}
