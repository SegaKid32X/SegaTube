/* io.h - movie files for the master SH-2: from the EverDrive's SD card (the
 * 68000 reads them into cartridge memory) or built into the ROM (VFS). */
#ifndef IO_H
#define IO_H

#include <stdint.h>
#include "stv_format.h"

int  io_init(void);                     /* -> number of movies, or -1 no EverDrive, -2 error */
int  io_is_vfs(void);
int  io_open(int movie, int kind, int part);    /* KIND_*; 0 or an error code */
uint32_t io_size(void);                 /* VFS: the open file's size (else ~0) */
/* Read len bytes at off of the open file into dst (SDRAM). Blocking. */
int  io_read(uint32_t off, void *dst, uint32_t len);

/* The open profile: header and chunk index */
typedef struct {
    int      movie, kind;
    int      version, depth, interlaced;
    int      pic_w, pic_h, pic_x, pic_y;
    uint32_t fps_num, fps_den, rate;
    uint32_t frames, chunks, max_record, duration_ms;
    int      parts, lead;
    char     title[64];
} movie_t;

int  movie_open(movie_t *m, int movie, int kind);
/* Index entry of chunk c (only while no stream read is in flight) */
void movie_index(int c, stv_index_entry *e);
int  movie_chunk_of_frame(uint32_t frame);

/* ---- streaming: the profile file, chunk after chunk, into an SDRAM buffer ---- */
#define STREAM_BYTES    (128 * 1024)
void     stream_start(int chunk);       /* drop everything, fetch from this chunk on */
void     stream_poll(void);             /* move the transfers along; call often */
uint32_t stream_avail(void);            /* bytes ready at stream_data() */
const uint8_t *stream_data(void);
void     stream_consume(uint32_t n);
void     stream_hold(int on);           /* 1: keep the unread data where it is */
int      stream_eof(void);              /* everything fetched and consumed */
int      stream_idle(void);             /* no transfer in flight */
void     stream_stop(void);             /* finish what is in flight, fetch no more */
int      stream_error(void);
void    *stream_scratch(void);          /* the buffer, for other uses while not streaming */

/* INFO.STI of a movie */
typedef struct {
    uint32_t duration_ms, thumb_count, thumb_interval_ms, thumb_offset, poster_offset;
    uint16_t profiles;
    char     title[64];
} info_t;
int  info_read(int movie, info_t *inf);
/* the poster (128x72 BGR555) or a thumbnail (96x54) into dst */
int  info_poster(int movie, const info_t *inf, uint16_t *dst);
int  info_thumb(int movie, const info_t *inf, int n, uint16_t *dst);

#endif
