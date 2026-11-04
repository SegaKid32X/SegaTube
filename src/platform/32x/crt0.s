!-----------------------------------------------------------------------
! SegaTube - 32X cartridge layout and SH-2 startup (from ZERO SECTOR).
!
! ROM layout (offsets from the start of the cartridge):
!   0x000  68000 reset vectors (MD mode: everything enters the init block)
!   0x100  Mega Drive header
!   0x200  32X 68000 exception jump table (adapter remaps vectors here)
!   0x3C0  MARS user header (SH-2 initial data load + entry points)
!   0x3F0  Sega-designated initial program (must be byte-identical)
!   0x800  our 68000 resident program (linked at 0x880800)
!
! SDRAM (.data, copied by the 32X boot ROM using the MARS header):
!   vector tables, master/slave entry code, then C .data
!
! COMM registers: see src/platform/md/md.h. During start-up the master
! posts its stage in COMM12 (COMM0-7 still hold the boot ROMs' M_OK/S_OK
! handshake then); a crash posts CMD_CRASH in COMM0, the
! faulting PC in COMM2:COMM4 and 0xD (slave) / 0xE (master) in COMM6.
!-----------------------------------------------------------------------

        .text
        .global _start
_start:

! 68000 vectors: in Mega Drive mode every vector enters the initial program.
        .long   0x01000000
        .rept   63
        .long   0x000003F0
        .endr

! Mega Drive header at 0x100 (.org pads each field with spaces and fails
! the build if a field overruns)
        .org    0x100
        .ascii  "SEGA SSF"              /* (not "SEGA 32X"): a Mega EverDrive turns on its
                                           extended SSF mapper and its I/O (md_ed.c) */
        .org    0x110, 0x20
        .ascii  "(C)STUB 2026.NOV"
        .org    0x120, 0x20
        .ascii  "SEGATUBE"
        .org    0x150, 0x20
        .ascii  "SEGATUBE"
        .org    0x180, 0x20
        .ascii  "GM STUB0001-00"
        .word   0x0000                  /* checksum, written by romfix */
        .ascii  "J6"                    /* 3/6-button pad */
        .org    0x1A0, 0x20
        .long   0x00000000,0x003FFFFF   /* ROM start, end (romfix updates) */
        .long   0x00FF0000,0x00FFFFFF   /* RAM start, end */
        .org    0x1C8, 0x20             /* notes: tools/mkvfs.py writes "STVFS@" + offset here */
        .org    0x1F0, 0x20
        .ascii  "JUE"
        .org    0x200, 0x20

! 32X 68000 exception jump table at 0x200: entry n-1 serves vector n.
        .macro  jump address
        .word   0x4EF9,\address>>16,\address&0xFFFF
        .endm

        .macro  call address
        .word   0x4EB9,\address>>16,\address&0xFFFF
        .endm

        jump    0x880800                /* reset -> resident program */
        .rept   46
        call    0x880840                /* the return address names the vector */
        .endr
        .org    0x3C0

! MARS user header at 0x3C0
        .ascii  "SEGATUBE        "
        .long   0x00000000              /* version */
        .long   __text_end-0x02000000   /* source: .data image in ROM */
        .long   0x00000000              /* destination: SDRAM offset 0 */
        .long   __data_size             /* size */
        .long   pri_start               /* master SH-2 entry */
        .long   sec_start               /* slave SH-2 entry */
        .long   pri_vbr                 /* master VBR */
        .long   sec_vbr                 /* slave VBR */

! 0x3F0: Sega-designated initial program (see tools/extract_icd.py)
        .incbin "src/platform/32x/icd_mars.bin"

! 0x800: our 68000 resident program
        .incbin "md.bin"                /* from the build directory (as -I) */
        .global __md_end
__md_end:

!-----------------------------------------------------------------------
! Everything below lives in SDRAM. "aw",@progbits keeps the section
! loadable so objcopy puts it in the ROM image the boot ROM copies.
!-----------------------------------------------------------------------
        .section .sdata,"aw",@progbits

        .equ    pri_stack, 0x0603E000   /* master: grows down towards .bss */
        .equ    sec_stack, 0x06040000   /* slave: 8 KiB above the master's */

! Sega requires the free-running timer set up on both SH-2s whether or not
! interrupts are used (32X Technical Bulletin #10; values from the 32X
! hardware manual), and any interrupt latched before we started cleared.
        .macro  sh2_hw_init frt, intclr
        mov.l   \frt,r1                 /* 0xFFFFFE10 FRT */
        mov     #0x01,r0
        mov.b   r0,@(0,r1)              /* TIER = 01h: no timer interrupts */
        mov     #0xE2,r0
        mov.b   r0,@(7,r1)              /* TOCR = E2h: select OCRA */
        mov     #0,r0
        mov.b   r0,@(4,r1)
        mov     #2,r0
        mov.b   r0,@(5,r1)              /* OCRA = 0002h */
        mov     #0,r0
        mov.b   r0,@(6,r1)              /* TCR = 0 */
        mov     #1,r0
        mov.b   r0,@(1,r1)              /* FTCSR = 01h */
        mov     #0,r0
        mov.b   r0,@(2,r1)              /* FRC = 0 (high byte first) */
        mov.b   r0,@(3,r1)
        mov.l   \intclr,r1              /* one past the PWM clear register */
        .rept   5                       /* PWM, CMD, H, V, VRES clear */
        mov.w   r0,@-r1
        mov.w   r0,@r1
        .endr
        .endm

        .macro  vbr_table start, stack, err, irq, dma
        .long   \start, \stack, \start, \stack
        .long   \err, 0, \err, 0, 0, \err, \err, \err, \err
        .space  76
        .rept   32
        .long   \err                    /* TRAPA #32..#63 */
        .endr
        .rept   8
        .long   \irq                    /* 64-71: 32X levels (unused) */
        .endr
        .long   \dma                    /* 72: DMA channel 1 end (VCRDMA1) */
        .endm

        .p2align 4
pri_vbr:
        vbr_table pri_start, pri_stack, pri_err, pri_irq, pri_irq
sec_vbr:
        vbr_table sec_start, sec_stack, sec_err, sec_irq, sec_dma_irq

!-----------------------------------------------------------------------
! Master SH-2
!-----------------------------------------------------------------------
        .p2align 2
pri_start:
        mov.l   p_stack,r15
        mov.l   p_sr_off,r0
        ldc     r0,sr                   /* mask all interrupts */
        mov.l   p_comm,r8               /* r8 = 0x20004020 COMM base */
        mov     #1,r0
        mov.w   r0,@(12,r8)             /* stage 1: entry */
        sh2_hw_init p_frt, p_intclr

        mov.l   p_ccr,r1
        mov     #0x10,r0                /* purge, cache off */
        mov.b   r0,@r1

        mov.l   p_bss_start,r1          /* clear .bss (16-byte aligned) */
        mov.l   p_bss_end,r2
        mov     #0,r0
1:      cmp/hs  r2,r1
        bt      2f
        mov.l   r0,@r1
        bra     1b
        add     #4,r1
2:
        mov     #4,r0
        mov.w   r0,@(12,r8)             /* stage 4: bss cleared */

        mov.l   p_mok,r1                /* wait for the 68000 to clear M_OK */
3:      mov.l   @r8,r0
        cmp/eq  r1,r0
        bt      3b

        mov     #0,r0
        mov.l   r0,@(4,r8)              /* clear S_OK: release the slave */
        mov     #5,r0
        mov.w   r0,@(12,r8)             /* stage 5: handshake done */

        mov.l   p_adapter,r1
        mov     #0x80,r0                /* FM = 1: SH-2 owns the 32X VDP */
        mov.b   r0,@r1
        mov     #0,r0
        mov.b   r0,@(1,r1)              /* no SH-2 interrupts */
        mov     #6,r0
        mov.w   r0,@(12,r8)             /* stage 6: FM set */

        mov.l   p_ccr,r1
        mov     #0x11,r0                /* purge, cache on */
        mov.b   r0,@r1

        mov.l   p_main,r0
        jmp     @r0
        nop

pri_err:
        mov.l   p_comm,r1
        mov     #0x0E,r4                /* 0xE: master exception */
        mov.l   @r15,r2                 /* the exception pushed PC, then SR */
        mov     r2,r0
        mov.w   r0,@(4,r1)              /* PC low -> COMM4 */
        shlr16  r2
        mov     r2,r0
        mov.w   r0,@(2,r1)              /* PC high -> COMM2 */
        mov     #0x1F,r0                /* whole palette red */
        bra     crash_palette
        nop

pri_irq:
        rte                             /* nothing interrupts the master */
        nop

        .p2align 2
p_frt:          .long   0xFFFFFE10
p_intclr:       .long   0x2000401E
p_stack:        .long   pri_stack
p_sr_off:       .long   0x000000F0
p_comm:         .long   0x20004020
p_ccr:          .long   0xFFFFFE92
p_bss_start:    .long   __bss_start
p_bss_end:      .long   __bss_end
p_mok:          .ascii  "M_OK"
p_adapter:      .long   0x20004000
p_main:         .long   _main

!-----------------------------------------------------------------------
! Slave SH-2: waits for release, then runs slave_main() (job dispatcher)
!-----------------------------------------------------------------------
        .p2align 2
sec_start:
        mov.l   s_stack,r15
        mov.l   s_sr_off,r0
        ldc     r0,sr
        sh2_hw_init s_frt, s_intclr
        mov.l   s_ccr,r1
        mov     #0x10,r0
        mov.b   r0,@r1

        mov.l   s_comm,r8
        mov.l   s_sok,r1
5:      mov.l   @(4,r8),r0              /* wait for the master to clear S_OK */
        cmp/eq  r1,r0
        bt      5b

        mov.l   s_ccr,r1
        mov     #0x11,r0
        mov.b   r0,@r1

        mov.l   s_main,r0
        jmp     @r0
        nop

sec_err:
        mov.l   s_comm,r1
        mov.l   @r15,r2
        mov     r2,r0
        mov.w   r0,@(4,r1)              /* PC low -> COMM4 */
        shlr16  r2
        mov     r2,r0
        mov.w   r0,@(2,r1)              /* PC high -> COMM2 */
        mov     #0x0D,r4                /* 0xD: slave exception */
        mov     #0x7C,r0
        shll8   r0                      /* whole palette blue */

! Stop for good, repeatedly posting r4 in COMM6 and CMD_CRASH in COMM0 (the
! 68000 shows the crash screen) and filling the 32X palette with
! r0 (ignored unless the SH-2s own the VDP), so a crash shows even without
! the 68000's report.
crash_palette:
        mov     r0,r5
6:      mov.l   s_comm,r1
        mov     r4,r0
        mov.w   r0,@(6,r1)              /* 0xD / 0xE -> COMM6 */
        mov     #0x0F,r0                /* CMD_CRASH -> COMM0 */
        mov.w   r0,@(0,r1)
        mov.l   s_cram,r1
        mov     #1,r3
        shll8   r3                      /* 256 entries */
7:      mov.w   r5,@r1
        dt      r3
        bf/s    7b
        add     #2,r1
        bra     6b
        nop

sec_irq:
        rte
        nop

! The slave's DMA channel 1 feeds the PWM; its transfer-end interrupt
! (vector 72, VCRDMA1) refills it (audio.c). Saves what C may clobber.
        .global sec_dma_irq
sec_dma_irq:
        sts.l   pr,@-r15
        mov.l   r0,@-r15
        mov.l   r1,@-r15
        mov.l   r2,@-r15
        mov.l   r3,@-r15
        mov.l   r4,@-r15
        mov.l   r5,@-r15
        mov.l   r6,@-r15
        mov.l   r7,@-r15
        sts.l   macl,@-r15
        sts.l   mach,@-r15
        mov.l   s_audio_irq,r0
        jsr     @r0
        nop
        lds.l   @r15+,mach
        lds.l   @r15+,macl
        mov.l   @r15+,r7
        mov.l   @r15+,r6
        mov.l   @r15+,r5
        mov.l   @r15+,r4
        mov.l   @r15+,r3
        mov.l   @r15+,r2
        mov.l   @r15+,r1
        mov.l   @r15+,r0
        lds.l   @r15+,pr
        rte
        nop

        .p2align 2
s_frt:          .long   0xFFFFFE10
s_intclr:       .long   0x2000401E
s_cram:         .long   0x20004200
s_stack:        .long   sec_stack
s_sr_off:       .long   0x000000F0
s_comm:         .long   0x20004020
s_ccr:          .long   0xFFFFFE92
s_sok:          .ascii  "S_OK"
s_main:         .long   _slave_main
s_audio_irq:    .long   _audio_irq

! A small initialised anchor: the boot ROM's .data copy loop underflows
! when __data_size is 0 (it never is here, but keep the guarantee local).
        .p2align 4
        .global _boot_anchor
_boot_anchor:
        .long   0x53545542,0,0,0        /* "STUB" */
