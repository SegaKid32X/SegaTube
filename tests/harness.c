/*
 * harness.c - headless libretro host to drive a Sega 32X ROM inside PicoDrive.
 *
 * Boots the ROM, runs a scripted controller sequence, and dumps frames as PPM
 * so run_tests.py can assert on them (black-screen guard, transitions, crops).
 *
 * Build (see tests/Makefile):
 *   cc -O2 -std=c11 -o harness harness.c -ldl
 *
 * Run:
 *   ./harness <core.so> <rom.32x> <script.txt> <outdir>
 *
 * Script language (one command per line, '#' = comment):
 *   run <n>            run n frames
 *   press <btn> <n>    hold btn for n frames, then release
 *   hold <btn>         press and keep held
 *   release <btn>      release a held button
 *   port <0|1>         choose which pad subsequent commands drive
 *   shot <name>        write <outdir>/<name>.ppm from the latest frame
 *   telem              print the game telemetry words
 *   expect <w> <v>     fail unless telemetry word w (1..5) == v (hex ok)
 *   expecthi <w> <v>   fail unless (word w >> 8) == v
 *   expectlo <w> <v>   fail unless (word w & 0xFF) == v
 *   belowlo <w> <v>    fail unless (word w & 0xFF) < v
 *   waithi <w> <v> <n> run up to n frames until (word w >> 8) == v
 *   waitlo <w> <v> <n> run up to n frames until (word w & 0xFF) == v
 *   audiomark          start a new audio measurement window
 *   loud <n>           fail unless audio RMS since audiomark >= n (16-bit units)
 *   quiet <n>          fail unless audio RMS since audiomark <  n
 *
 * HARNESS_PAD6=1 plugs in a 6-button pad (x, y, z, mode buttons).
 * SRAM_FILE=path keeps the cartridge's battery RAM: loaded (if it exists)
 * before the first frame, written back at the end of the script.
 *
 * Telemetry: the 68000 mirrors 6 words to MD work RAM 0xFFFF00:
 *   0 magic 0x5A53, 1 state, 2 health<<8|ammo, 3 boot stage, 4 pad, 5 vblanks
 * Buttons: up down left right a b c start x y z mode
 *
 * This is a minimal, self-contained libretro frontend: it declares only the
 * subset of the libretro ABI it needs, so no libretro.h is required.
 * NOTE: the Genesis/32X button->libretro-id mapping is core-dependent; adjust
 * the BTN table below if a press does not register with your PicoDrive build.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- minimal libretro ABI ------------------------------------------------ */
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_game_info_ext {
    const char *full_path, *archive_path, *archive_file, *dir, *name, *ext, *meta;
    const void *data;
    size_t size;
    bool file_in_archive, persistent_data;
};
/* HARNESS_INFO_EXT=1: hand the core its content the way RetroArch does,
 * as a loaded file through GET_GAME_INFO_EXT (cores that ask for it) */
static struct retro_game_info_ext game_ext;
struct retro_game_geometry { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; };
struct retro_system_timing { double fps; double sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };

typedef bool (*env_t)(unsigned cmd, void *data);
typedef void (*video_t)(const void *data, unsigned w, unsigned h, size_t pitch);
typedef void (*audio_t)(int16_t l, int16_t r);
typedef size_t (*audio_batch_t)(const int16_t *data, size_t frames);
typedef void (*poll_t)(void);
typedef int16_t (*input_t)(unsigned port, unsigned device, unsigned index, unsigned id);

#define ENV_GET_CAN_DUPE            3
#define ENV_GET_LOG_INTERFACE       27
#define ENV_GET_SYSTEM_DIRECTORY    9
#define ENV_SET_PIXEL_FORMAT        10
#define ENV_GET_VARIABLE            15
#define ENV_GET_VARIABLE_UPDATE     17
#define ENV_SET_SUPPORT_NO_GAME     18
#define ENV_GET_SAVE_DIRECTORY      31
#define ENV_GET_INPUT_BITMASKS      51
#define ENV_GET_CORE_OPTIONS_VERSION 52
#define ENV_GET_GAME_INFO_EXT       66

#define DEVICE_JOYPAD 1
/* libretro joypad ids */
#define ID_B 0
#define ID_Y 1
#define ID_START 3
#define ID_UP 4
#define ID_DOWN 5
#define ID_LEFT 6
#define ID_RIGHT 7
#define ID_A 8
#define ID_X 9
#define ID_L 10
#define ID_R 11

/* name -> libretro id. Genesis mapping is core-dependent; tweak if needed. */
static const struct { const char *name; unsigned id; } BTN[] = {
    {"up", ID_UP}, {"down", ID_DOWN}, {"left", ID_LEFT}, {"right", ID_RIGHT},
    {"a", ID_Y}, {"b", ID_B}, {"c", ID_A}, {"start", ID_START},   /* PicoDrive: Y=A, A=C */
    {"x", ID_L}, {"y", ID_X}, {"z", ID_R}, {"mode", 2 /*SELECT*/},   /* PicoDrive: L=X, X=Y, R=Z */
};

/* ---- core symbols -------------------------------------------------------- */
static void  (*retro_init)(void);
static void  (*retro_deinit)(void);
static void  (*retro_set_environment)(env_t);
static void  (*retro_set_video_refresh)(video_t);
static void  (*retro_set_audio_sample)(audio_t);
static void  (*retro_set_audio_sample_batch)(audio_batch_t);
static void  (*retro_set_input_poll)(poll_t);
static void  (*retro_set_input_state)(input_t);
static bool  (*retro_load_game)(const struct retro_game_info *);
static void  (*retro_run)(void);
static void  (*retro_unload_game)(void);
static void  (*retro_get_system_av_info)(struct retro_system_av_info *);
static void *(*retro_get_memory_data)(unsigned id);
static size_t (*retro_get_memory_size)(unsigned id);
#define MEMORY_SYSTEM_RAM 2
#define MEMORY_SAVE_RAM 0

/* ---- state --------------------------------------------------------------- */
static int      pixfmt = 0;            /* 0=0RGB1555, 1=XRGB8888, 2=RGB565 */
static uint32_t pad[2];                /* held button bitmask per port     */
static const char *sysdir = ".";

#define MAXW 512
#define MAXH 512
static uint8_t  rgb[MAXW * MAXH * 3];
static unsigned fw, fh;
static int      have_frame;

#include <stdarg.h>
static void cb_log(int level, const char *fmt, ...)
{
    if (level < 2 && !getenv("HARNESS_VERBOSE")) return;     /* debug/info */
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool cb_env(unsigned cmd, void *data)
{
    switch (cmd & 0xFFFF) {
    case ENV_SET_PIXEL_FORMAT:
        pixfmt = *(const int *)data;
        return pixfmt >= 0 && pixfmt <= 2;
    case ENV_GET_LOG_INTERFACE:
        *(void (**)(int, const char *, ...))data = cb_log; return true;
    case ENV_GET_CAN_DUPE:
        *(bool *)data = true; return true;
    case ENV_GET_SYSTEM_DIRECTORY:
    case ENV_GET_SAVE_DIRECTORY:
        *(const char **)data = sysdir; return true;
    case ENV_GET_VARIABLE: {          /* each option's default, except: */
        struct { const char *key, *value; } *v = data;
        if (getenv("HARNESS_PAD6") && !strcmp(v->key, "picodrive_input1")) {
            v->value = "6 button pad";
            return true;
        }
        return false;
    }
    case ENV_GET_VARIABLE_UPDATE:
        *(bool *)data = false; return true;
    case ENV_GET_GAME_INFO_EXT:
        if (!getenv("HARNESS_INFO_EXT") || !game_ext.full_path)
            return false;
        *(const struct retro_game_info_ext **)data = &game_ext;
        return true;
    case ENV_GET_INPUT_BITMASKS:
        return false;
    case ENV_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0; return true;
    case ENV_SET_SUPPORT_NO_GAME:
        return true;
    default:
        /* accept SET_* notifications, reject unknown GETs */
        return false;
    }
}

static void store_frame(const void *src, unsigned w, unsigned h, size_t pitch)
{
    if (!src || w > MAXW || h > MAXH) return;
    const uint8_t *base = src;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned r, g, b;
            if (pixfmt == 1) {                     /* XRGB8888 */
                uint32_t v = ((const uint32_t *)(base + y * pitch))[x];
                r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255;
            } else {
                uint16_t v = ((const uint16_t *)(base + y * pitch))[x];
                if (pixfmt == 2) {                 /* RGB565 */
                    r = ((v >> 11) & 31) * 255 / 31;
                    g = ((v >> 5) & 63) * 255 / 63;
                    b = (v & 31) * 255 / 31;
                } else {                           /* 0RGB1555 */
                    r = ((v >> 10) & 31) * 255 / 31;
                    g = ((v >> 5) & 31) * 255 / 31;
                    b = (v & 31) * 255 / 31;
                }
            }
            uint8_t *p = &rgb[(y * w + x) * 3];
            p[0] = r; p[1] = g; p[2] = b;
        }
    }
    fw = w; fh = h; have_frame = 1;
}

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (data && data != (const void *)-1) store_frame(data, w, h, pitch);
}
static double audio_sq;
static long   audio_n;
static FILE  *wav_out;                 /* HARNESS_WAV: raw capture of all audio */
static void cb_audio(int16_t l, int16_t r)
{
    audio_sq += (double)l * l + (double)r * r; audio_n += 2;
    if (wav_out) { int16_t s[2] = { l, r }; fwrite(s, 2, 2, wav_out); }
}
static size_t cb_audio_batch(const int16_t *d, size_t f)
{
    for (size_t i = 0; i < f * 2; i++) audio_sq += (double)d[i] * d[i];
    audio_n += (long)f * 2;
    if (wav_out) fwrite(d, 2, f * 2, wav_out);
    return f;
}
static double audio_rms(void) { return audio_n ? sqrt(audio_sq / audio_n) : 0; }
static void cb_poll(void) {}
static int16_t cb_input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (port > 1 || dev != DEVICE_JOYPAD || idx != 0) return 0;
    return (pad[port] >> id) & 1;
}

static void *load_sym(void *h, const char *n)
{
    void *p = dlsym(h, n);
    if (!p) { fprintf(stderr, "missing symbol %s\n", n); exit(2); }
    return p;
}

static void write_ppm(const char *dir, const char *name)
{
    if (!have_frame) { fprintf(stderr, "shot %s: no frame yet\n", name); exit(3); }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(3); }
    fprintf(f, "P6\n%u %u\n255\n", fw, fh);
    fwrite(rgb, 1, (size_t)fw * fh * 3, f);
    fclose(f);
}

static int telem_word(int w)
{
    uint8_t *ram = retro_get_memory_data(MEMORY_SYSTEM_RAM);
    size_t   sz  = retro_get_memory_size(MEMORY_SYSTEM_RAM);
    if (!ram || sz < 0x10000) return -1;
    /* PicoDrive keeps 68000 RAM as native 16-bit words */
    return *(uint16_t *)(ram + 0xFF00 + 2 * w);
}

static void print_telem(void)
{
    printf("telem magic=%04x state=%04x stats=%04x stage=%04x pad=%04x vbl=%04x hw=%04x music=%04x audio=%.0f\n",
           telem_word(0), telem_word(1), telem_word(2), telem_word(3),
           telem_word(4), telem_word(5), telem_word(6), telem_word(7), audio_rms());
    fflush(stdout);
}

static unsigned btn_id(const char *name)
{
    for (size_t i = 0; i < sizeof BTN / sizeof BTN[0]; i++)
        if (!strcmp(name, BTN[i].name)) return BTN[i].id;
    fprintf(stderr, "unknown button: %s\n", name); exit(4);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s core.so rom.32x script outdir\n", argv[0]);
        return 2;
    }
    const char *core_path = argv[1], *rom_path = argv[2];
    const char *script = argv[3], *outdir = argv[4];

    void *h = dlopen(core_path, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

    retro_init          = load_sym(h, "retro_init");
    retro_deinit        = load_sym(h, "retro_deinit");
    retro_set_environment = load_sym(h, "retro_set_environment");
    retro_set_video_refresh = load_sym(h, "retro_set_video_refresh");
    retro_set_audio_sample = load_sym(h, "retro_set_audio_sample");
    retro_set_audio_sample_batch = load_sym(h, "retro_set_audio_sample_batch");
    retro_set_input_poll = load_sym(h, "retro_set_input_poll");
    retro_set_input_state = load_sym(h, "retro_set_input_state");
    retro_load_game     = load_sym(h, "retro_load_game");
    retro_run           = load_sym(h, "retro_run");
    retro_unload_game   = load_sym(h, "retro_unload_game");
    retro_get_system_av_info = load_sym(h, "retro_get_system_av_info");
    retro_get_memory_data = load_sym(h, "retro_get_memory_data");
    retro_get_memory_size = load_sym(h, "retro_get_memory_size");
    if (getenv("HARNESS_SYSDIR")) sysdir = getenv("HARNESS_SYSDIR");
    if (getenv("HARNESS_WAV")) wav_out = fopen(getenv("HARNESS_WAV"), "wb");

    retro_set_environment(cb_env);
    retro_set_video_refresh(cb_video);
    retro_set_audio_sample(cb_audio);
    retro_set_audio_sample_batch(cb_audio_batch);
    retro_set_input_poll(cb_poll);
    retro_set_input_state(cb_input);
    retro_init();

    FILE *rf = fopen(rom_path, "rb");
    if (!rf) { perror(rom_path); return 2; }
    fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 0, SEEK_SET);
    void *rom = malloc(sz);
    if (fread(rom, 1, sz, rf) != (size_t)sz) { perror("read rom"); return 2; }
    fclose(rf);

    struct retro_game_info gi = { rom_path, rom, (size_t)sz, NULL };
    {
        static char dir[1024], name[256], ext[16];
        const char *slash = strrchr(rom_path, '/'), *base = slash ? slash + 1 : rom_path;
        snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - rom_path) : 1, slash ? rom_path : ".");
        snprintf(name, sizeof name, "%s", base);
        char *dot = strrchr(name, '.');
        if (dot) { snprintf(ext, sizeof ext, "%s", dot + 1); *dot = 0; }
        game_ext = (struct retro_game_info_ext){ rom_path, NULL, NULL, dir, name, ext, NULL,
                                                 rom, (size_t)sz, false, false };
    }
    if (!retro_load_game(&gi)) { fprintf(stderr, "core rejected ROM\n"); return 2; }
    struct retro_system_av_info av; retro_get_system_av_info(&av);
    const char *sram_file = getenv("SRAM_FILE");
    if (sram_file) {
        uint8_t *sram = retro_get_memory_data(MEMORY_SAVE_RAM);
        size_t n = retro_get_memory_size(MEMORY_SAVE_RAM);
        FILE *f = fopen(sram_file, "rb");
        if (f && sram && n) { size_t got = fread(sram, 1, n, f); (void)got; }
        if (f) fclose(f);
        fprintf(stderr, "sram: %zu bytes%s\n", n, f ? " (loaded)" : "");
    }

    FILE *sf = fopen(script, "r");
    if (!sf) { perror(script); return 2; }
    char line[256]; unsigned cur_port = 0;
    while (fgets(line, sizeof line, sf)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char cmd[32], arg[32]; int n; unsigned v2 = 0, v3 = 0;
        int k = sscanf(line, "%31s %31s %d", cmd, arg, &n);
        sscanf(line, "%*s %*s %x %u", &v2, &v3);
        if (k <= 0) continue;
        if (!strcmp(cmd, "run") && k >= 2) {
            int frames = atoi(arg);
            for (int i = 0; i < frames; i++) retro_run();
        } else if (!strcmp(cmd, "press") && k >= 3) {
            unsigned id = btn_id(arg);
            pad[cur_port] |= 1u << id;
            for (int i = 0; i < n; i++) retro_run();
            pad[cur_port] &= ~(1u << id);
        } else if (!strcmp(cmd, "hold") && k >= 2) {
            pad[cur_port] |= 1u << btn_id(arg);
        } else if (!strcmp(cmd, "release") && k >= 2) {
            pad[cur_port] &= ~(1u << btn_id(arg));
        } else if (!strcmp(cmd, "port") && k >= 2) {
            cur_port = (unsigned)atoi(arg) & 1;
        } else if (!strcmp(cmd, "shot") && k >= 2) {
            write_ppm(outdir, arg);
        } else if (!strcmp(cmd, "telem")) {
            print_telem();
        } else if (!strcmp(cmd, "mark") && k >= 2) {
            printf("mark %s %ld\n", arg, audio_n / 2);      /* stereo frames so far */
            fflush(stdout);
        } else if (!strcmp(cmd, "audiomark")) {
            audio_sq = 0; audio_n = 0;
        } else if ((!strcmp(cmd, "loud") || !strcmp(cmd, "quiet")) && k >= 2) {
            double rms = audio_rms(), lim = atof(arg);
            if (cmd[0] == 'l' ? rms < lim : rms >= lim) {
                fprintf(stderr, "AUDIO FAILED: %s %s but rms = %.1f\n", cmd, arg, rms);
                return 5;
            }
        } else if ((!strcmp(cmd, "expect") || !strcmp(cmd, "expecthi") ||
                    !strcmp(cmd, "expectlo") || !strcmp(cmd, "belowlo")) && k >= 2) {
            int w = atoi(arg), val = telem_word(w);
            if (cmd[6] == 'h') val >>= 8;
            if (cmd[6] == 'l' || cmd[0] == 'b') val &= 0xFF;
            if (cmd[0] == 'b' ? !(val < (int)v2) : val != (int)v2) {
                fprintf(stderr, "EXPECT FAILED: %s word %d = %x, want %x\n", cmd, w, val, v2);
                print_telem();
                return 5;
            }
        } else if ((!strcmp(cmd, "waithi") || !strcmp(cmd, "waitlo")) && k >= 2) {
            int w = atoi(arg), i;
            for (i = 0; i < (int)v3; i++) {
                int val = telem_word(w);
                val = cmd[4] == 'h' ? (val >> 8) & 0xFF : val & 0xFF;
                if (val == (int)v2) break;
                retro_run();
            }
            if (i == (int)v3) {
                fprintf(stderr, "WAIT TIMEOUT: %s word %d never reached %x\n", cmd, w, v2);
                print_telem();
                return 5;
            }
        } else {
            fprintf(stderr, "bad script line: %s", line); return 4;
        }
    }
    fclose(sf);

    if (sram_file) {
        uint8_t *sram = retro_get_memory_data(MEMORY_SAVE_RAM);
        size_t n = retro_get_memory_size(MEMORY_SAVE_RAM);
        FILE *f = fopen(sram_file, "wb");
        if (f && sram && n) fwrite(sram, 1, n, f);
        if (f) fclose(f);
    }
    retro_unload_game();
    retro_deinit();
    free(rom);
    return 0;
}
