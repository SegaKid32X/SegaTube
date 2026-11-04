/* audio.h - stereo sound through the 32X PWM (audio.c). */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

void     audio_init(int rate);          /* once: starts the slave's PWM DMA */
void     audio_reset(void);             /* drop what is queued; paused */
void     audio_pause(int on);
uint32_t audio_position(void);          /* samples played since the reset */
uint32_t audio_space(void);             /* samples that can be written now */
uint32_t audio_queued(void);
uint32_t audio_underruns(void);
int      audio_cycle(void);
void     audio_write(const int16_t *pcm, int n);
void     audio_write_silence(int n);

#endif
