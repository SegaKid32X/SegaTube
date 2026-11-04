/* md_vfs.c - movies built into the ROM (test builds, and the emulator tests).
 *
 * tools/mkvfs.py appends a small directory and the movie files to the ROM and
 * records where in the ROM header (0x1C8: "STVFS@" and a 32-bit offset). The
 * SH-2s read the files themselves; the 68000 only needs the names.
 *
 * VFS layout (big-endian):
 *   0   "STVFS1\0\0"
 *   8   u32 count
 *   12  u32 reserved
 *   16  count entries of 80 bytes: name[48], then u32 offset[4] and u32 size[4]
 *       for INFO.STI, FILM.STV, FULL.STV, I480.STV (offsets from the VFS start,
 *       0 = absent)
 */
#include "md.h"

#define ROM_HDR     ((const volatile uint8_t *)0x880000)    /* cartridge 0 .. 512 KiB */
#define BANK_WIN    ((const volatile uint8_t *)0x900000)    /* the 1 MiB bank window */
#define MARS_BANK   MARS16(0x04)

static uint8_t cart_byte(uint32_t off)
{
    MARS_BANK = (uint16_t)(off >> 20);
    return BANK_WIN[off & 0xFFFFF];
}

static uint32_t cart_long(uint32_t off)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | cart_byte(off + (uint32_t)i);
    return v;
}

int vfs_list_movies(void)
{
    const char *tag = "STVFS@";
    for (int i = 0; i < 6; i++)
        if (ROM_HDR[0x1C8 + i] != (uint8_t)tag[i])
            return 0;
    uint32_t vfs = 0;
    for (int i = 0; i < 4; i++) vfs = vfs << 8 | ROM_HDR[0x1CE + i];
    if (vfs < 0x1000 || vfs >= 0x400000) return 0;
    const char *magic = "STVFS1";
    for (int i = 0; i < 6; i++)
        if (cart_byte(vfs + (uint32_t)i) != (uint8_t)magic[i])
            return 0;
    uint32_t n = cart_long(vfs + 8);
    if (n > MAX_MOVIES) n = MAX_MOVIES;
    for (uint32_t m = 0; m < n; m++) {
        uint32_t e = vfs + 16 + m * 80;
        for (int k = 0; k < NAME_LEN; k++)
            movie_name[m][k] = (char)cart_byte(e + (uint32_t)k);
        movie_name[m][NAME_LEN - 1] = 0;
    }
    MARS_BANK = 0;
    movie_count = (int)n;
    return (int)n;
}
