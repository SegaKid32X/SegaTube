/*
 * mars.ld - SH-2 linker script for a Sega 32X ROM.
 *
 * Memory map:
 *   0x02000000  ROM   boot code, 68000 program, big assets (cartridge)
 *   0x06000000  SDRAM .data (all SH-2 code and hot tables, copied from ROM by
 *                     the 32X boot ROM), then .bss
 *   0x0603E000  top of master SH-2 stack (grows down)
 *   0x06040000  top of slave SH-2 stack (grows down, 8 KiB)
 *
 * SegaTube uses both SH-2s (from ZERO SECTOR): the master stack grows down from 0x0603E000
 * and the slave stack from 0x06040000 (both set in crt0.s). RAM LENGTH stops
 * .data+.bss at 0x0603D000, leaving at least 4 KiB of master stack, so a RAM
 * overflow is a link error instead of a black screen.
 */

OUTPUT_ARCH(sh)
EXTERN (_start)
ENTRY (_start)
__DYNAMIC = 0;

MEMORY
{
    rom (rx) : ORIGIN = 0x02000000, LENGTH = 0x00400000   /* up to ~4 MiB   */
    ram (wx) : ORIGIN = 0x06000000, LENGTH = 0x0003D000    /* SDRAM below the stacks */
}

PROVIDE (__stack = 0x0603E000);

SECTIONS
{
  /* Cartridge ROM: the headers, boot code, the 68000 program and the big
   * assets. Everything the SH-2s touch every frame (all code and the other
   * read-only tables) goes in .data instead: the boot ROM copies it to
   * SDRAM, which is cached and far faster than the cartridge on hardware. */
  .text 0x02000000 : AT ( 0x00000000 )
  {
    __text_start = .;
    KEEP (*crt0.o(.text))

    . = ALIGN(16);
    __INIT_SECTION__ = .;
    KEEP (*(.init))
    SHORT (0x000B) SHORT (0x0009)   /* rts ; nop */
    . = ALIGN(16);
    __FINI_SECTION__ = .;
    KEEP (*(.fini))
    SHORT (0x000B) SHORT (0x0009)   /* rts ; nop */

    *(.eh_frame_hdr) KEEP (*(.eh_frame))
    *(.gcc_except_table) *(.gcc_except_table.*)
    KEEP (*(.jcr))

    . = ALIGN(16);
    __CTOR_LIST__ = .; ___CTOR_LIST__ = .;
    LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)
    KEEP (*(SORT(.ctors.*))) KEEP (*(.ctors))
    LONG(0) __CTOR_END__ = .;

    . = ALIGN(16);
    __DTOR_LIST__ = .; ___DTOR_LIST__ = .;
    LONG((__DTOR_END__ - __DTOR_LIST__) / 4 - 2)
    KEEP (*(SORT(.dtors.*))) KEEP (*(.dtors))
    LONG(0) __DTOR_END__ = .;

    /* nothing big lives in ROM: the movies are files (or the VFS appended
     * after the ROM image by tools/mkvfs.py) */
    . = ALIGN(16);
    __text_end = .;
  } > rom
  __text_size = __text_end - __text_start;

  .data 0x06000000 : AT ( LOADADDR (.text) + SIZEOF (.text) )
  {
    __data_start = .;
    *(.sdata) *(.sdata.*)               /* crt0: vector tables, SH-2 entry */
    *(.text) *(.text.*) *(.gnu.linkonce.t.*)
    *(.rdata) *(.rodata) *(.rodata.*) *(.gnu.linkonce.r.*)
    *(.data) *(.data.*) *(.gnu.linkonce.d.*)
    CONSTRUCTORS
    *(.lit8) *(.lit4) *(.gnu.linkonce.s.*)
    . = ALIGN(16);
    __data_end = .;
  } > ram
  __data_size = __data_end - __data_start;

  .bss :
  {
    __bss_start = .;
    *(.bss) *(.bss.*) *(.gnu.linkonce.b.*)
    *(.sbss) *(.sbss.*) *(.gnu.linkonce.sb.*)
    *(.scommon) *(COMMON)
    . = ALIGN(16);
    end = .; _end = end; __end = _end;
    __bss_end = .;
  } > ram
  __bss_size = __bss_end - __bss_start;
}
