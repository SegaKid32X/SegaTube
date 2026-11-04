/* audio.c - stereo sound through the 32X PWM.
 *
 * The slave SH-2's DMA channel 1 moves one 32-bit word (left and right pulse
 * widths) into the PWM's L/R registers each time the PWM asks (its DREQ,
 * once a sample), from a ring in SDRAM. At the end of each block of
 * AUDIO_BLOCK samples the DMA interrupts the slave (vector 72, crt0.s), which
 * starts the next block: the ring's next block if the master has filled it,
 * else a block of silence (and the movie clock waits, since it counts the
 * ring's samples played). This is the arrangement DOOM 32X Resurrection uses
 * (CHCR1 = 0x18E5, PWM control 0x0185).
 *
 * The master decodes each frame's ADPCM straight into the ring
 * (audio_write); the samples played so far are the movie's clock
 * (audio_position).
 */
#include "hw.h"
#include "audio.h"

#define AUDIO_RING      8192            /* stereo samples (0.37 s at 22 kHz) */
#define AUDIO_BLOCK     256
#define SH2_CLOCK_NTSC  23011361
#define SH2_CLOCK_PAL   22801467

static uint32_t ring[AUDIO_RING] __attribute__((aligned(16)));
static uint32_t silence[AUDIO_BLOCK] __attribute__((aligned(16)));

/* shared between the CPUs: always through the cache-through mirror */
typedef struct {
    volatile uint32_t wpos;         /* samples written (master) */
    volatile uint32_t rpos;         /* samples whose block has finished (slave) */
    volatile uint32_t cur;          /* start of the block playing now, or ~0 for silence */
    volatile uint32_t paused;
    volatile uint32_t underruns;
    volatile uint32_t running;
    volatile uint32_t cycle;
} audio_state;
static audio_state st_storage __attribute__((aligned(16)));
#define ST ((audio_state *)SDRAM_THROUGH(&st_storage))

#define DMA_SAR1   (*(volatile uint32_t *)0xFFFFFF90)
#define DMA_DAR1   (*(volatile uint32_t *)0xFFFFFF94)
#define DMA_TCR1   (*(volatile uint32_t *)0xFFFFFF98)
#define DMA_CHCR1  (*(volatile uint32_t *)0xFFFFFF9C)
#define DMA_VCR1   (*(volatile uint32_t *)0xFFFFFFA8)
#define DMA_DRCR1  (*(volatile uint8_t *)0xFFFFFE72)
#define DMA_OR     (*(volatile uint32_t *)0xFFFFFFB0)
#define INTC_IPRA  (*(volatile uint16_t *)0xFFFFFEE2)

static void start_block(const uint32_t *src)
{
    (void)DMA_CHCR1;
    DMA_CHCR1 = 0;                          /* clears TE, stops */
    DMA_SAR1 = (uint32_t)(uintptr_t)src | 0x20000000u;
    DMA_DAR1 = 0x20004034;                  /* L and R pulse width in one long */
    DMA_TCR1 = AUDIO_BLOCK;
    DMA_CHCR1 = 0x18E5;                     /* dst fixed, src++, long, DREQ, interrupt, on */
}

/* The slave's DMA-end interrupt (crt0.s sec_dma_irq) */
void audio_irq(void)
{
    audio_state *s = ST;
    if (s->cur != 0xFFFFFFFFu)
        s->rpos = s->cur + AUDIO_BLOCK;
    uint32_t next = s->rpos;
    if (!s->paused && s->wpos - next >= AUDIO_BLOCK) {
        s->cur = next;
        start_block(&ring[next & (AUDIO_RING - 1)]);
    } else {
        if (!s->paused && s->running) s->underruns++;
        s->cur = 0xFFFFFFFFu;
        start_block(silence);
    }
}

static uint32_t cycle_for(int rate)
{
    int clock = (MARS_VDP_MODE & VDP_MODE_PAL) ? SH2_CLOCK_PAL : SH2_CLOCK_NTSC;
    return (uint32_t)(clock / rate) + 1;
}

/* Slave job, once: set up the PWM and its DMA, then let the interrupt run. */
static void audio_start_job(int rate)
{
    audio_state *s = ST;
    uint32_t cycle = cycle_for(rate);
    s->cycle = cycle;
    for (int i = 0; i < AUDIO_BLOCK; i++)
        silence[i] = (cycle / 2) << 16 | (cycle / 2);
    MARS_INTMASK = 0;                       /* no 32X interrupts on the slave */
    MARS_PWM_CTRL = 0x0185;                 /* L->L, R->R, DREQ each sample */
    MARS_PWM_CYCLE = (uint16_t)cycle;
    for (int i = 0; i < 3; i++)
        MARS_PWM_MONO = (uint16_t)(cycle / 2);
    DMA_OR = 1;
    DMA_DRCR1 = 0;                          /* DREQ1 from outside (the PWM) */
    DMA_VCR1 = 72;                          /* vector 72 */
    INTC_IPRA = (uint16_t)((INTC_IPRA & 0xF0FF) | 0x0E00);   /* DMAC: level 14 */
    s->cur = 0xFFFFFFFFu;
    start_block(silence);
    /* let level 14 in (SR mask 13) */
    uint32_t sr;
    __asm__ volatile ("stc sr,%0" : "=r"(sr));
    sr = (sr & ~0xF0u) | 0xD0u;
    __asm__ volatile ("ldc %0,sr" : : "r"(sr));
}

/* Slave job: another sample rate (the DMA keeps running; the PWM and its
 * FIFO are set up only once - redoing that while the DMA feeds it hangs). */
static void audio_rate_job(int rate)
{
    audio_state *s = ST;
    uint32_t cycle = cycle_for(rate);
    uint32_t sr, masked;
    __asm__ volatile ("stc sr,%0" : "=r"(sr));
    masked = sr | 0xF0u;
    __asm__ volatile ("ldc %0,sr" : : "r"(masked));
    s->cycle = cycle;
    for (int i = 0; i < AUDIO_BLOCK; i++)
        silence[i] = (cycle / 2) << 16 | (cycle / 2);
    MARS_PWM_CYCLE = (uint16_t)cycle;
    __asm__ volatile ("ldc %0,sr" : : "r"(sr));
}

static int started_rate;

void audio_init(int rate)
{
    audio_state *s = ST;
    if (started_rate) audio_reset();
    s->wpos = s->rpos = 0;
    s->paused = 1;
    s->running = 0;
    s->underruns = 0;
    if (!started_rate)
        hw_slave_start(audio_start_job, rate);
    else if (rate != started_rate)
        hw_slave_start(audio_rate_job, rate);
    hw_slave_wait();
    started_rate = rate;
}

int audio_cycle(void)
{
    return (int)ST->cycle;
}

/* Forget everything queued; the next sample written is sample 0. */
void audio_reset(void)
{
    audio_state *s = ST;
    s->paused = 1;
    /* wait for the current block to end so the interrupt sees the reset */
    for (int i = 0; i < 400000 && s->cur != 0xFFFFFFFFu; i++)
        ;
    s->wpos = 0;
    s->rpos = 0;
    s->underruns = 0;
}

void audio_pause(int on)
{
    audio_state *s = ST;
    s->paused = (uint32_t)on;
    s->running = 1;
}

uint32_t audio_space(void)
{
    audio_state *s = ST;
    return AUDIO_RING - AUDIO_BLOCK - (s->wpos - s->rpos);
}

uint32_t audio_queued(void)
{
    audio_state *s = ST;
    return s->wpos - s->rpos;
}

/* Samples of the movie played so far (since the last reset). */
uint32_t audio_position(void)
{
    audio_state *s = ST;
    uint32_t cur = s->cur;
    uint32_t rpos = s->rpos;
    if (cur == 0xFFFFFFFFu)
        return rpos;
    /* (TCR1 is the slave's register: the master estimates mid-block as the
     * block start; good to 256 samples, about 12 ms) */
    return cur;
}

uint32_t audio_underruns(void)
{
    return ST->underruns;
}

/* Append n stereo samples (int16 L, R) to the ring as pulse widths. */
void audio_write(const int16_t *pcm, int n)
{
    audio_state *s = ST;
    uint32_t w = s->wpos;
    uint32_t c = s->cycle - 1;
    for (int i = 0; i < n; i++) {
        uint32_t l = ((uint32_t)(pcm[2 * i] + 32768) * c >> 16) + 1;
        uint32_t r = ((uint32_t)(pcm[2 * i + 1] + 32768) * c >> 16) + 1;
        ring[(w + (uint32_t)i) & (AUDIO_RING - 1)] = l << 16 | r;
    }
    s->wpos = w + (uint32_t)n;
}

/* Append n samples of silence. */
void audio_write_silence(int n)
{
    audio_state *s = ST;
    uint32_t w = s->wpos, half = s->cycle / 2;
    for (int i = 0; i < n; i++)
        ring[(w + (uint32_t)i) & (AUDIO_RING - 1)] = half << 16 | half;
    s->wpos = w + (uint32_t)n;
}
