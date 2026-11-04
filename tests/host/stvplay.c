/*
 * stvplay - desktop reference player for SegaTube movie files.
 *
 * Parses a profile file the way the 32X player does (header, index, chunks,
 * records, pictures, audio), decodes with the same decoder into two models
 * of the 32X frame buffers, and writes what the screen would show.
 *
 *   stvplay FILE.STV --frames 0,10,100 --out DIR     PPM screens of those frames
 *   stvplay FILE.STV --wav out.wav                   the audio, as played
 *   stvplay FILE.STV --seek CHUNK --frames ...        start at a chunk (as a seek)
 *   stvplay FILE.STV --check                         decode everything, report
 *
 * Exit status 0 when every picture decoded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "stv_dec.h"

static uint8_t *file;
static size_t file_n;

static void fail(const char *m)
{
    fprintf(stderr, "stvplay: %s\n", m);
    exit(1);
}

typedef struct {
    int version, depth, interlaced, w, h, x, y;
    uint32_t fps_num, fps_den, rate, frames, chunks, index, data, lead;
} hdr;

static hdr H;
static uint8_t *fb[2];
static int pitch;
static uint16_t palette[256];

static void put_screen(const char *path, int field_buf)
{
    /* 320x224 for progressive, 320x448 woven for i480 */
    int sh = H.interlaced ? 448 : 224;
    FILE *f = fopen(path, "wb");
    if (!f) fail("cannot write screen");
    fprintf(f, "P6\n320 %d\n255\n", sh);
    for (int sy = 0; sy < sh; sy++) {
        for (int sx = 0; sx < 320; sx++) {
            int buf = H.interlaced ? (sy & 1) : field_buf;
            int ly = H.interlaced ? sy / 2 : sy;
            int px = sx - H.x, py = ly - H.y;
            unsigned c = 0;
            if (px >= 0 && px < H.w && py >= 0 && py < H.h) {
                uint8_t *row = fb[buf] + py * pitch;
                c = H.depth == STV_DEPTH_P8 ? palette[row[px]] : ((uint16_t *)(void *)row)[px];
            }
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            uint8_t o[3] = { (uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 3 | g >> 2), (uint8_t)(b << 3 | b >> 2) };
            fwrite(o, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *path = NULL, *outdir = ".", *wav = NULL, *frames_arg = NULL;
    int seek = 0, check = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_arg = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--seek") && i + 1 < argc) seek = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check")) check = 1;
        else path = argv[i];
    }
    if (!path) fail("usage: stvplay FILE.STV [--frames a,b,c] [--out DIR] [--wav F] [--seek N] [--check]");
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open file");
    fseek(f, 0, SEEK_END);
    file_n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(file_n);
    if (fread(file, 1, file_n, f) != file_n) fail("short read");
    fclose(f);
    if (file_n < STV_FILE_HEADER || stv_rd32(file) != STV_MAGIC) fail("not an STV file");
    const uint8_t *h = file;
    H.version = stv_rd16(h + 4);
    H.depth = stv_rd16(h + 8);
    H.interlaced = stv_rd16(h + 10);
    H.w = stv_rd16(h + 12); H.h = stv_rd16(h + 14);
    H.x = stv_rd16(h + 16); H.y = stv_rd16(h + 18);
    H.fps_num = stv_rd32(h + 20); H.fps_den = stv_rd32(h + 24);
    H.rate = stv_rd32(h + 28);
    H.frames = stv_rd32(h + 32);
    H.chunks = stv_rd32(h + 36);
    H.index = stv_rd32(h + 40);
    H.data = stv_rd32(h + 44);
    H.lead = stv_rd16(h + 62);
    if (stv_rd16(h + 60) != 1) fail("multi-part files: not in this tool yet");
    printf("%s: %dx%d at %d,%d depth %d%s, %u/%u fps, audio %u Hz, %u frames, %u chunks, title '%.64s'\n",
           path, H.w, H.h, H.x, H.y, H.depth, H.interlaced ? " interlaced" : "", H.fps_num, H.fps_den,
           H.rate, H.frames, H.chunks, (const char *)h + 68);

    pitch = H.w * H.depth;
    fb[0] = calloc((size_t)pitch * H.h + 16, 1);
    fb[1] = calloc((size_t)pitch * H.h + 16, 1);
    int want[4096], nwant = 0;
    if (frames_arg) {
        char *s = strdup(frames_arg), *t = strtok(s, ",");
        while (t && nwant < 4096) { want[nwant++] = atoi(t); t = strtok(NULL, ","); }
    }
    FILE *wf = NULL;
    long wav_samples = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        uint8_t z[44] = { 0 };
        fwrite(z, 1, 44, wf);
    }
    stv_adpcm al = { 0, 0 }, ar = { 0, 0 };
    int16_t *pcm = malloc(65536 * 4);
    int errors = 0;
    long pictures = 0;
    for (uint32_t c = (uint32_t)seek; c < H.chunks; c++) {
        const uint8_t *ie = file + H.index + c * 16;
        uint32_t off = stv_rd32(ie), size = stv_rd32(ie + 4), first = stv_rd32(ie + 8);
        int nfr = stv_rd16(ie + 14);
        if (off + size > file_n) fail("chunk past the end");
        const uint8_t *ch = file + off;
        if (stv_rd32(ch) != STV_CHUNK_MAGIC || stv_rd32(ch + 8) != first) fail("bad chunk header");
        const uint8_t *r = ch + 32;
        for (int i = 0; i < nfr; i++) {
            uint32_t rsize = stv_rd32(r);
            int main_n = stv_rd16(r + 4), pre_n = stv_rd16(r + 6), pics = stv_rd16(r + 8), flags = stv_rd16(r + 10);
            const uint8_t *p = r + STV_REC_HEADER;
            const uint8_t *state = NULL;
            if (flags & STV_REC_ADPCM_STATE) { state = p; p += 8; }
            if (pre_n) {
                /* played only when we started here (a seek, or the movie start) */
                stv_adpcm pl, pr;
                stv_adpcm_load(&pl, p);
                stv_adpcm_load(&pr, p + 4);
                p += 8;
                if (c == (uint32_t)seek && i == 0) {
                    stv_adpcm_decode(&pl, &pr, p, pre_n, pcm);
                    if (wf) fwrite(pcm, 4, (size_t)pre_n, wf);
                    wav_samples += pre_n;
                }
                p += pre_n;
            }
            if (state) {
                stv_adpcm_load(&al, state);
                stv_adpcm_load(&ar, state + 4);
            }
            stv_adpcm_decode(&al, &ar, p, main_n, pcm);
            if (wf) fwrite(pcm, 4, (size_t)main_n, wf);
            wav_samples += main_n;
            p += main_n;
            while ((p - file) & 3) p++;
            uint32_t frame = first + (uint32_t)i;
            for (int k = 0; k < pics; k++) {
                int psize = (int)stv_rd32(p + 4), pflags = p[1], parts = p[2];
                const uint8_t *q = p + 8;
                if (pflags & STV_PIC_PALETTE) {
                    for (int j = 0; j < 256; j++) palette[j] = stv_rd16(q + 2 * j);
                    q += 512;
                }
                int buf = H.interlaced ? k : (int)(frame & 1);
                stv_target t = { fb[buf], pitch, H.w / 4, H.h / 4, H.depth, H.version, 0 };
                for (int j = 0; j < parts; j++) {
                    uint32_t po = stv_rd32(q + 4 * j);
                    int e = stv_decode_part(p + po, (uint32_t)psize - po, &t);
                    if (e) { errors++; fprintf(stderr, "frame %u pic %d part %d: error %d\n", frame, k, j, e); }
                }
                pictures++;
                p += psize;
            }
            if ((uint32_t)(p - r) != rsize) { errors++; fprintf(stderr, "frame %u: record size mismatch\n", frame); }
            for (int j = 0; j < nwant; j++)
                if ((uint32_t)want[j] == frame) {
                    char out[1024];
                    snprintf(out, sizeof out, "%s/frame%05u.ppm", outdir, frame);
                    put_screen(out, (int)(frame & 1));
                }
            r += rsize;
        }
        if (!check && !wav && nwant && (uint32_t)want[nwant - 1] < first + (uint32_t)nfr) break;
    }
    if (wf) {
        uint8_t hd[44];
        uint32_t data = (uint32_t)wav_samples * 4;
        memcpy(hd, "RIFF", 4); uint32_t v = 36 + data; memcpy(hd + 4, &v, 4);
        memcpy(hd + 8, "WAVEfmt ", 8); v = 16; memcpy(hd + 16, &v, 4);
        uint16_t s = 1; memcpy(hd + 20, &s, 2); s = 2; memcpy(hd + 22, &s, 2);
        v = H.rate; memcpy(hd + 24, &v, 4); v = H.rate * 4; memcpy(hd + 28, &v, 4);
        s = 4; memcpy(hd + 32, &s, 2); s = 16; memcpy(hd + 34, &s, 2);
        memcpy(hd + 36, "data", 4); memcpy(hd + 40, &data, 4);
        fseek(wf, 0, SEEK_SET);
        fwrite(hd, 1, 44, wf);
        fclose(wf);
        printf("audio: %ld samples (%.2f s)\n", wav_samples, (double)wav_samples / H.rate);
    }
    printf("%ld pictures, %d errors\n", pictures, errors);
    return errors ? 1 : 0;
}
