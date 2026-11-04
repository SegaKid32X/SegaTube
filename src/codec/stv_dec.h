/* stv_dec.h - SegaTube picture and audio decoding (32X player and PC encoder). */
#ifndef STV_DEC_H
#define STV_DEC_H

#include <stdint.h>
#include "stv_format.h"

/* Where a picture goes. On the 32X, base points into the frame buffer the
 * SH-2 can see (the one not on screen); in the encoder, into a simulation of
 * it. Pixels are native-endian: one byte (P8) or one 16-bit word (DC). */
typedef struct {
    uint8_t *base;      /* pixel (0,0) of the picture */
    int      pitch;     /* bytes from one row to the next */
    int      bw;        /* picture width in 4x4 blocks */
    int      bh;        /* picture height in blocks */
    int      depth;     /* STV_DEPTH_P8 or STV_DEPTH_DC */
    int      version;   /* the file's STV version (op 7: 1 SKIPLONG, 2 GRAD) */
} stv_target;

#define STV_OK          0
#define STV_ERR_HEADER  (-1)    /* part header inconsistent */
#define STV_ERR_OPS     (-2)    /* ops describe more blocks than the part has */
#define STV_ERR_DATA    (-3)    /* a stream ran out */

/* Decode one part (a band of block rows) of a picture into t. len is the
 * number of bytes available at part. Returns STV_OK or STV_ERR_*; on an
 * error the blocks decoded so far stay written and nothing outside the
 * part's rows is touched. */
int stv_decode_part(const uint8_t *part, uint32_t len, const stv_target *t);

/* GRAD blocks (the encoder uses these to know exactly what the decoder
 * draws). stv_grad_pattern[i]: which corner (0 TL, 1 TR, 2 BL, 3 BR) pixel i
 * of a 256-colour block takes. stv_grad_dc: the 16 BGR555 pixels blended
 * from 4 corners. */
extern const uint8_t stv_grad_pattern[16];
void stv_grad_dc(const uint16_t *corner, uint16_t *out);

/* Streams of a part, for tools: offsets from the part start */
typedef struct {
    uint32_t ops, col, msk, raw, mv, end;
} stv_part_layout;
int stv_part_layout_of(const uint8_t *part, uint32_t len, stv_part_layout *out);

/* IMA ADPCM, 4 bits a sample. One byte holds a stereo sample: the left
 * channel in the high nibble. */
typedef struct {
    int predictor;
    int index;
} stv_adpcm;

void stv_adpcm_load(stv_adpcm *s, const uint8_t *state4);  /* from stv_adpcm_state bytes */
/* Decode n stereo samples from src into out (interleaved L, R). */
void stv_adpcm_decode(stv_adpcm *l, stv_adpcm *r, const uint8_t *src, int n, int16_t *out);
/* One sample: the encoder uses it to track the decoder exactly. */
int  stv_adpcm_step(stv_adpcm *s, int nibble);
extern const int16_t stv_adpcm_steps[89];
extern const int8_t  stv_adpcm_index_adj[16];

#endif
