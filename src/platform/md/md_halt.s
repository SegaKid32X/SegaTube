| md_halt.s - wait in work RAM while the Mega EverDrive owns the cartridge bus.
|
| A file read into cartridge memory (EverDrive command 0xCB) is a DMA by the
| EverDrive's own processor: the 68000 must not touch the cartridge until it
| ends, so this loop is copied to work RAM (ed_init_halt) and run there with
| interrupts off. It follows the EverDrive's ed_halt_app (mega-ed-pub,
| edio/everdrive.c, MIT): post the status to wait for, start the transfer,
| then poll SYSSTAT until it reads 0x55A0 with the strobe toggling and the
| requested status bits clear. The SH-2s stay off the cartridge meanwhile
| (they only ask for a read when they do not need the cartridge).
|
| int ed_halt_code(int stat_req)  -> 0 done, -1 timed out (about 2 s)

        .text
        .global ed_halt_code, ed_halt_code_end
        .align  2
ed_halt_code:
        move.l  4(sp),d0                | stat_req
        movem.l d2-d3,-(sp)
        move.w  sr,-(sp)
        move.w  #0x2700,sr
        lea     0xA130D4,a0             | SYSSTAT
        move.w  d0,(a0)
        move.b  #0,0xA130D1             | FIFODATA = 0: run
        move.l  #4000000,d3             | timeout
1:      subq.l  #1,d3
        beq.b   9f
        move.w  (a0),d1
        move.w  d1,d2
        andi.w  #0xFFF8,d2
        cmpi.w  #0x55A8,d2              | 0x55A0 | strobe
        bne.b   1b
        move.w  (a0),d1
        move.w  d1,d2
        andi.w  #0xFFF8,d2
        cmpi.w  #0x55A0,d2              | strobe toggled
        bne.b   1b
        and.w   d0,d1
        bne.b   1b
        moveq   #0,d0
        bra.b   8f
9:      moveq   #-1,d0
8:      move.w  (sp)+,sr
        movem.l (sp)+,d2-d3
        rts
        .align  2
ed_halt_code_end:

| void ed_blind_code(int frames)
| Start an EverDrive transfer without reading its status (for when its
| registers cannot be read): post STATUS_CMD_OK, run, then wait the given
| number of vblanks (watching the VDP, which is not on the cartridge bus).
        .global ed_blind_code, ed_blind_code_end
        .align  2
ed_blind_code:
        move.l  4(sp),d1                | frames
        move.w  sr,-(sp)
        move.w  #0x2700,sr
        move.w  #2,0xA130D4             | SYSSTAT = STATUS_CMD_OK
        move.b  #0,0xA130D1             | FIFODATA = 0: run
        lea     0xC00004,a0
        subq.l  #1,d1
        bmi.b   3f
1:      move.w  (a0),d0                 | wait for the end of vblank
        btst    #3,d0
        bne.b   1b
2:      move.w  (a0),d0                 | then for the next one
        btst    #3,d0
        beq.b   2b
        dbra    d1,1b
3:      move.w  (sp)+,sr
        rts
        .align  2
ed_blind_code_end:
