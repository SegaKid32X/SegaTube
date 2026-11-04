| md_halt.s - the EverDrive register test, run from work RAM (md_ed.c).
| (The driver itself is C in work RAM: md_ed.c, section .ramtext.)

        .text
| void ed_rv_code(uint16_t *out)    (run from work RAM)
| The EverDrive's I/O with RV = 1 (the 32X hands the cartridge bus to the
| 68000, as ZERO SECTOR's console-proven save code does before it touches the
| EverDrive). Reads the eight I/O words, the 1 ms timer before and after six
| vblanks, then sends the status command and reads up to two answer bytes:
|   out[0..7] registers, out[8], out[9] timer, out[10] bytes answered,
|   out[11], out[12] the answer
        .global ed_rv_code, ed_rv_code_end
        .align  2
ed_rv_code:
        movea.l 4(sp),a1
        movem.l d2-d3/a2,-(sp)
        move.w  sr,-(sp)
        move.w  #0x2700,sr
        bset    #0,0xA15107             | RV = 1
        lea     0xA130D0,a0
        moveq   #7,d1
1:      move.w  (a0)+,(a1)+
        dbra    d1,1b
        lea     0xA130D0,a0
        move.w  6(a0),(a1)+             | timer
        lea     0xC00004,a2
        moveq   #5,d1
2:      move.w  (a2),d0
        btst    #3,d0
        bne.b   2b
3:      move.w  (a2),d0
        btst    #3,d0
        beq.b   3b
        dbra    d1,2b
        move.w  6(a0),(a1)+             | timer, ~100 ms later
        | flush what the FIFO holds for us
        move.w  #4095,d1
4:      move.w  2(a0),d0
        andi.w  #0x7FF,d0
        beq.b   5f
        move.b  1(a0),d0
        dbra    d1,4b
5:      | the status command: '+', ~'+', 0x10, ~0x10
        move.b  #0x2B,1(a0)
        move.b  #0xD4,1(a0)
        move.b  #0x10,1(a0)
        move.b  #0xEF,1(a0)
        moveq   #0,d2                   | bytes read
        move.l  #400000,d3
6:      subq.l  #1,d3
        beq.b   8f
        move.w  2(a0),d0
        andi.w  #0x7FF,d0
        beq.b   6b
        moveq   #0,d0
        move.b  1(a0),d0
        move.w  d2,d1
        add.w   d1,d1
        move.w  d0,2(a1,d1.w)           | out[11 + n]
        addq.w  #1,d2
        cmpi.w  #2,d2
        bne.b   6b
8:      move.w  d2,(a1)                 | out[10]
        bclr    #0,0xA15107             | RV = 0
        move.w  (sp)+,sr
        movem.l (sp)+,d2-d3/a2
        rts
        .align  2
ed_rv_code_end:
