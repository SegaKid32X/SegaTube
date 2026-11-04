/*
 * stv_format.h - the SegaTube movie format (".STV"), shared by the 32X
 * player and the PC encoder. All multi-byte fields are big-endian (the SH-2
 * and the 68000 are big-endian; the encoder writes them that way).
 *
 * A movie is a folder on the SD card:
 *
 *   MOVIES/<name>/INFO.STI    title, poster, scrub thumbnails, the profiles
 *   MOVIES/<name>/FILM.STV    one file per picture profile (any subset)
 *   MOVIES/<name>/FULL.STV
 *   MOVIES/<name>/I480.STV
 *
 * The EverDrive's file pointer is 32-bit, so a profile file stays under
 * 4 GB: a longer one continues in FILM.ST1, FILM.ST2 ... (the index says which
 * part holds each chunk).
 *
 * Profile file:
 *   [stv_file_header, 4 KiB]
 *   [chunk index: chunk_count x stv_index_entry, padded to 512 bytes]
 *   [chunks]
 * A chunk is about one second (a GOP): it starts with intra pictures, so
 * playback, seeking, fast forward and rewind can start at any chunk.
 *
 *   [stv_chunk_header, 32 bytes]
 *   [records: one per video frame]
 *
 * Record (4-byte aligned, one per frame):
 *   u32 size                   whole record, bytes (multiple of 4)
 *   u16 audio_samples          stereo samples of ADPCM in this record (main part)
 *   u16 preroll_samples        first record of a chunk only: audio played
 *                              only after a seek (see DESIGN.md), else 0
 *   u16 pic_count              pictures: 1 (progressive) or 2 (480i fields)
 *   u16 flags                  STV_REC_*
 *   stv_adpcm_state state[2]   (if STV_REC_ADPCM_STATE) L and R state to load
 *                              before the main audio part
 *   stv_adpcm_state pre[2]     (if preroll_samples) state for the preroll
 *   preroll ADPCM bytes        preroll_samples bytes (one byte = L,R nibbles)
 *   main ADPCM bytes           audio_samples bytes
 *   (pad to 4)
 *   pictures                   each: stv_pic_header, then its data
 *
 * Picture: 8-byte header, optional palette (256 x u16), then parts. A part is
 * a band of block rows decoded by one SH-2 (part 0: master, part 1: slave).
 *   stv_pic_header
 *   u16 palette[256]           if STV_PIC_PALETTE (32X CRAM format, BGR555)
 *   u32 part_offset[parts]     from the picture header start
 *   parts                      each: stv_part_header, then its streams
 */
#ifndef STV_FORMAT_H
#define STV_FORMAT_H

#include <stdint.h>

#define STV_MAGIC           0x53545631u     /* "STV1" */
#define STV_INFO_MAGIC      0x53544931u     /* "STI1" */
#define STV_CHUNK_MAGIC     0x43484E4Bu     /* "CHNK" */
#define STV_VERSION         1

#define STV_FILE_HEADER     4096
#define STV_INFO_HEADER     4096

/* Screen */
#define STV_SCREEN_W        320
#define STV_SCREEN_H        224
#define STV_BLOCK           4               /* 4x4 pixel blocks */

/* Profiles */
#define STV_PROF_FILM       0   /* direct colour (RGB555), up to 320x204, 24 fps */
#define STV_PROF_FULL       1   /* 256 colours a scene, 320x224, up to 30 fps */
#define STV_PROF_I480       2   /* 256 colours, 320x448 interlaced (two fields) */
#define STV_PROF_COUNT      3

/* Pixel depth of a profile's pictures */
#define STV_DEPTH_P8        1   /* one byte per pixel, a palette index */
#define STV_DEPTH_DC        2   /* one 16-bit BGR555 word per pixel */

/* Direct colour mode: the 32X frame buffer holds a 256-word line table and
 * 65280 words of pixels, so at most 204 rows of 320 pixels. */
#define STV_DC_MAX_ROWS     203     /* + one black row for the letterbox */

typedef struct {
    uint32_t magic;             /* STV_MAGIC */
    uint16_t version;
    uint16_t profile;           /* STV_PROF_* */
    uint16_t depth;             /* STV_DEPTH_* */
    uint16_t interlaced;        /* 1: each record holds an even and an odd field */
    uint16_t pic_w, pic_h;      /* picture (field) size in pixels, multiples of 4 */
    uint16_t pic_x, pic_y;      /* top-left on the 320x224 screen (x multiple of 4) */
    uint32_t fps_num, fps_den;  /* frames per second = num / den */
    uint32_t audio_rate;        /* stereo samples per second (IMA ADPCM, 4-bit) */
    uint32_t frame_count;
    uint32_t chunk_count;
    uint32_t index_offset;      /* bytes from the start of part 0 */
    uint32_t data_offset;       /* first chunk */
    uint32_t max_record;        /* largest record, bytes (the player's buffer) */
    uint32_t max_chunk;         /* largest chunk, bytes */
    uint32_t duration_ms;
    uint16_t part_count;        /* files: .STV, .ST1, ... */
    uint16_t audio_lead;        /* records carry audio this many frames ahead */
    uint32_t bitrate;           /* average bytes per second */
    char     title[64];         /* ASCII, NUL padded */
    uint8_t  reserved[STV_FILE_HEADER - 132];
} stv_file_header;

typedef struct {
    uint32_t offset;            /* within its part file */
    uint32_t size;              /* bytes, multiple of 4 */
    uint32_t first_frame;
    uint16_t part;              /* 0: .STV, 1: .ST1, ... */
    uint16_t frames;
} stv_index_entry;

typedef struct {
    uint32_t magic;             /* STV_CHUNK_MAGIC */
    uint32_t number;
    uint32_t first_frame;
    uint16_t frames;
    uint16_t flags;
    uint32_t size;              /* whole chunk incl. this header */
    uint32_t first_sample;      /* audio sample number of the first main sample */
    uint32_t reserved[2];
} stv_chunk_header;

typedef struct {
    int16_t  predictor;
    uint8_t  index;
    uint8_t  pad;
} stv_adpcm_state;

#define STV_REC_HEADER      12
#define STV_REC_ADPCM_STATE 0x0001  /* state[2] present */
#define STV_REC_KEY         0x0002  /* first record of a chunk */
#define STV_REC_LAST        0x0004  /* the last record of the movie */

/* Picture header */
typedef struct {
    uint8_t  type;              /* STV_PIC_I / STV_PIC_P */
    uint8_t  flags;             /* STV_PIC_PALETTE, STV_PIC_FIELD_ODD */
    uint8_t  parts;             /* 1 or 2 */
    uint8_t  reserved;
    uint32_t size;              /* whole picture incl. header, multiple of 4 */
} stv_pic_header;

#define STV_PIC_I           0   /* intra: no SKIP or MOVE */
#define STV_PIC_P           1   /* refers to what the target buffer already holds */
#define STV_PIC_PALETTE     0x01
#define STV_PIC_FIELD_ODD   0x02
#define STV_MAX_PARTS       2

/* Part header: 16 bytes, then the five streams, each padded to 4 bytes */
typedef struct {
    uint16_t row0, row1;        /* block rows [row0, row1) of the picture */
    uint16_t ops_len;           /* op bytes */
    uint16_t col_len;           /* colour stream bytes (P8: u8 each, DC: u16 each) */
    uint16_t msk_len;           /* mask stream bytes (u16 each) */
    uint16_t raw_len;           /* raw pixel bytes */
    uint16_t mv_len;            /* motion vector bytes (u16 each) */
    uint16_t cost;              /* the encoder's estimate of decode cycles / 64 */
} stv_part_header;

/*
 * Block ops. Blocks are visited in raster order over the part's block rows.
 * Each op byte is (mode << 5) | (n - 1), n = 1..32 blocks.
 *
 *   SKIP     n blocks keep what the buffer holds
 *   FILL     n blocks, each one colour (col)
 *   FILLRUN  n blocks, all one colour (one col)
 *   PAIR     n blocks, each 2 colours (col) and a 16-bit mask (msk);
 *            mask bit 15 is pixel (0,0), row-major; 1 selects the second colour
 *   QUAD     n blocks, each 4 colours (col) and 32 bits of 2-bit indices
 *            (two msk words, the first holds rows 0-1), pixel (0,0) in the top bits
 *   RAW      n blocks, each 16 pixels row-major (raw)
 *   MOVE     n blocks, each copied from the same buffer at (x+dx, y+dy):
 *            one mv word each: dx in the high byte, dy in the low byte
 *            (signed pixels). The source lies inside the part's rows; the 4x4
 *            source is read whole before the block is written.
 *   SKIPLONG n x 32 blocks skipped
 */
#define STV_OP_SKIP         0
#define STV_OP_FILL         1
#define STV_OP_FILLRUN      2
#define STV_OP_PAIR         3
#define STV_OP_QUAD         4
#define STV_OP_RAW          5
#define STV_OP_MOVE         6
#define STV_OP_SKIPLONG     7
#define STV_OP(mode, n)     ((uint8_t)(((mode) << 5) | ((n) - 1)))

/* INFO.STI: the library's view of a movie */
#define STV_POSTER_W        128     /* library poster, BGR555 */
#define STV_POSTER_H        72
#define STV_THUMB_W         96      /* scrub thumbnails, BGR555 */
#define STV_THUMB_H         54
#define STV_THUMB_BYTES     (STV_THUMB_W * STV_THUMB_H * 2)

typedef struct {
    uint32_t magic;             /* STV_INFO_MAGIC */
    uint16_t version;
    uint16_t profiles;          /* bit n: profile n present (STV_PROF_*) */
    uint32_t duration_ms;
    uint32_t thumb_count;
    uint32_t thumb_interval_ms; /* thumbnail n shows time n * interval */
    uint32_t thumb_offset;      /* bytes from the file start */
    uint32_t poster_offset;
    uint32_t reserved0[9];
    char     title[64];
    uint8_t  reserved[STV_INFO_HEADER - 128];
} stv_info_header;

_Static_assert(sizeof(stv_file_header) == STV_FILE_HEADER, "file header size");
_Static_assert(sizeof(stv_info_header) == STV_INFO_HEADER, "info header size");
_Static_assert(sizeof(stv_index_entry) == 16, "index entry size");
_Static_assert(sizeof(stv_chunk_header) == 32, "chunk header size");
_Static_assert(sizeof(stv_part_header) == 16, "part header size");
_Static_assert(sizeof(stv_pic_header) == 8, "picture header size");

/* Byte order helpers (the formats are big-endian) */
static inline uint16_t stv_rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t stv_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#endif
