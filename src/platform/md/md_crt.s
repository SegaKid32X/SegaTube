| SegaTube - 68000 entry (linked at 0x880800).
| The Sega initial program at 0x3F0 falls through to here with the carry
| set if it found a problem (d0 = its error code), clear otherwise.

        .section .text.entry,"ax"
        .global _start
_start:
        bra.w   init

| 0x880840: every 68000 exception and interrupt arrives here through a
| "jsr 0x880840" in the jump table at 0x200, so the return address on the
| stack says which vector fired: vector = (return - 0x880200) / 6.
| Vectors 2-11 (bus error .. line F) are fatal and get an error screen;
| everything else returns.
        .org    0x40
exception:
        move.l  d0,-(sp)
        move.l  4(sp),d0
        sub.l   #0x880200,d0
        divu.w  #6,d0
        andi.l  #0xFFFF,d0
        cmpi.w  #12,d0
        bcs.b   1f
        move.l  (sp)+,d0
        addq.l  #4,sp                   | drop the jump table return address
        rte
1:      move.w  #0x2700,sr
        lea     __stack,sp
        move.l  d0,-(sp)
        jsr     md_fatal                | never returns
2:      bra.b   2b

init:
        bcs.b   0f                      | test the carry before anything clears it
        moveq   #0,d7                   | boot block verdict: 0 = OK
        bra.b   1f
0:      moveq   #0,d7
        move.b  d0,d7
        ori.w   #0x100,d7               | 0x100 | error code
1:      move.w  #0x2700,sr
        lea     0xC00004,a0             | boot probe: the screen turns solid
        move.w  #0x8144,(a0)            | blue (display on, backdrop = CRAM 0)
        move.w  #0x8700,(a0)            | at our very first instruction, so a
        move.l  #0xC0000000,(a0)        | black screen means this code never
        move.w  #0x0C00,-4(a0)          | ran
        lea     __stack,sp
        lea     __bss_start,a0          | clear .bss
        lea     __bss_end,a1
2:      cmpa.l  a1,a0
        bhs.b   3f
        clr.w   (a0)+
        bra.b   2b
3:      lea     __data_load,a0          | copy .data from ROM
        lea     __data_start,a1
        lea     __data_end,a2
4:      cmpa.l  a2,a1
        bhs.b   5f
        move.w  (a0)+,(a1)+
        bra.b   4b
5:      move.l  d7,-(sp)                | md_main(boot_err)
        jsr     md_main
6:      bra.b   6b

