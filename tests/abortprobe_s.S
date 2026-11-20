/* Supervisor half of tests/abortprobe.c.
 *
 * ap_handler goes on the bus error vector itself (VBR+8) for a single
 * probe access, so exec's vector 2 code cannot complete the fault (68020/
 * 030) or misread the frame (68040/060). A fault whose stacked PC lies in
 * the probe routines is copied to ap_frame, counted in ap_faults and
 * resumed at ap_recover through a format 0 frame, as src/pciinit/bus.S
 * does. Every other fault goes to the previous vector, ap_old.
 *
 * Frame sizes: $A 32, $B 92 (68020/030), $7 60 (68040), $4 16 (68060).
 */

    .text
    .align 2
    .globl _ap_read8, _ap_read16, _ap_read32
    .globl _ap_write8, _ap_write16, _ap_write32
    .globl _ap_handler, _ap_vbr
    .globl _ap_old, _ap_faults, _ap_frame_size, _ap_frame

ap_window_start:

/* A0 address, D0 value. The leading NOP completes earlier stores, the
 * trailing one makes a 68040/060 buffered store fault in here. */
_ap_read8:
    nop
    moveq   #0,d0
    move.b  (a0),d0
    nop
    rts
_ap_read16:
    nop
    moveq   #0,d0
    move.w  (a0),d0
    nop
    rts
_ap_read32:
    nop
    move.l  (a0),d0
    nop
    rts
_ap_write8:
    nop
    move.b  d0,(a0)
    nop
    rts
_ap_write16:
    nop
    move.w  d0,(a0)
    nop
    rts
_ap_write32:
    nop
    move.l  d0,(a0)
    nop
    rts

ap_recover:
    move.l  #0xdeadbeef,d0
    rts

ap_window_end:

_ap_handler:
    movem.l d0-d1/a0-a1,-(sp)       /* saved 0-15, frame from 16 */
    move.l  16+2(sp),d0             /* stacked PC */
    lea     ap_window_start(pc),a0
    cmp.l   a0,d0
    bcs     .Lchain
    lea     ap_window_end(pc),a0
    cmp.l   a0,d0
    bcc     .Lchain
    move.w  16+6(sp),d0             /* format */
    lsr.w   #8,d0
    lsr.w   #4,d0
    moveq   #32,d1
    cmp.w   #0xa,d0
    beq.s   .Lsize
    moveq   #92,d1
    cmp.w   #0xb,d0
    beq.s   .Lsize
    moveq   #60,d1
    cmp.w   #7,d0
    beq.s   .Lsize
    moveq   #16,d1
    cmp.w   #4,d0
    bne     .Lchain
.Lsize:
    move.l  d1,_ap_frame_size
    addq.l  #1,_ap_faults
    lea     16(sp),a0
    lea     _ap_frame,a1
    move.l  d1,d0
    lsr.w   #1,d0
    subq.w  #1,d0
1:  move.w  (a0)+,(a1)+
    dbf     d0,1b
    /* The new 8-byte frame ends where the old one ended: SP+16+size. */
    lea     8(sp,d1.w),a0
    move.w  16(sp),(a0)             /* SR of the faulting code */
    lea     ap_recover(pc),a1
    move.l  a1,2(a0)
    move.w  #0x0008,6(a0)           /* format 0, vector 2 */
    move.l  12(sp),-(a0)            /* saved registers below it */
    move.l  8(sp),-(a0)
    move.l  4(sp),-(a0)
    move.l  (sp),-(a0)
    move.l  a0,sp
    movem.l (sp)+,d0-d1/a0-a1
    rte
.Lchain:
    movem.l (sp)+,d0-d1/a0-a1
    move.l  _ap_old,-(sp)
    rts

/* Supervisor() entry: the vector base register in D0. */
_ap_vbr:
    movec   vbr,d0
    rte

    .bss
    .align 2
_ap_old:        .space 4
_ap_faults:     .space 4
_ap_frame_size: .space 4
_ap_frame:      .space 92
