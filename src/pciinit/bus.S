/* Card accesses that survive a 68k bus error.
 *
 * On a PC, a cycle nobody completes is aborted: reads return all-ones and
 * writes vanish. On an Amiga, the same cycle through a PCI bridge can end
 * in a 68k bus error, which without a handler is a dead-end alert. A ROM
 * that probes memory behind a PLL that has not locked yet expects the PC
 * behaviour and retries.
 *
 * Card loads and stores for the ROM are made by routines between
 * pi_bus_window_start and pi_bus_window_end. While pi_hardware_start has
 * installed pi_bus_trap as the task's tc_TrapCode, an access fault whose
 * stacked PC lies in that window resumes at pi_bus_recover, which returns
 * -1 to the routine's caller.
 *
 * Exec decides which access faults get that far. On the 68020 and 68030
 * its vector 2 code completes a data fault itself (a read returns zero)
 * and never calls tc_TrapCode, so the handler only sees faults on the
 * 68040 and 68060, or whatever a CPU library passes on. hardware.c
 * therefore makes single accesses with plain loads and stores on the
 * 68020/030 and calls these routines for them only on the 68040/060;
 * string transfers and banked framebuffer copies through the legacy
 * window use them on every CPU.
 *
 * The routines use only D0/D1/A0/A1 and never the stack, so returning
 * from the recovery point is a normal return from the routine. Stores are
 * followed by NOP: on the 68040 and 68060 it waits for the store to
 * complete, so a buffered write faults inside the window.
 */

#define EXEC_THISTASK 276 /* ExecBase.ThisTask */
#define TC_TRAPDATA   46  /* Task.tc_TrapData */
#define TC_TRAPCODE   50  /* Task.tc_TrapCode */
#define TRAP_CODE     0   /* PIBusTrap.code */
#define TRAP_DATA     4   /* PIBusTrap.data */
#define TRAP_FAULTS   8   /* PIBusTrap.faults */
#define TRAP_ADDRESS  12  /* PIBusTrap.address */

    .text
    .align 2
    .globl _pi_bus_read8, _pi_bus_read16, _pi_bus_read32
    .globl _pi_bus_write8, _pi_bus_write16, _pi_bus_write32
    .globl _pi_bus_fill8, _pi_bus_fill16, _pi_bus_fill32
    .globl _pi_bus_copy8, _pi_bus_copy16, _pi_bus_copy32
    .globl _pi_bus_trap

pi_bus_window_start:

/* Each routine starts with NOP. On the 68040 it completes the caller's
 * pending stores, so an access error frame discarded below never holds a
 * write-back that belongs to code outside the window. */
_pi_bus_read8:
    nop
    moveq   #0,d0
    move.b  (a0),d0
    rts
_pi_bus_read16:
    nop
    moveq   #0,d0
    move.w  (a0),d0
    rts
_pi_bus_read32:
    nop
    move.l  (a0),d0
    rts

_pi_bus_write8:
    nop
    move.b  d0,(a0)
    nop
    rts
_pi_bus_write16:
    nop
    move.w  d0,(a0)
    nop
    rts
_pi_bus_write32:
    nop
    move.l  d0,(a0)
    nop
    rts

/* A0 destination, D0 value, D1 count >= 1. DBF counts 16 bits; the outer
 * step carries into the upper word, as compiled loops do. Each loop starts
 * on a longword: on the 68030 a loop at an odd word took 0.28 s longer for
 * the VRAM clear of a ViRGE POST (WinUAE, 25 MHz). */
_pi_bus_fill8:
    nop
    subq.l  #1,d1
    .balignw 4,0x4e71
1:  move.b  d0,(a0)+
    dbf     d1,1b
    clr.w   d1
    subq.l  #1,d1
    bcc.s   1b
    nop
    rts
_pi_bus_fill16:
    nop
    subq.l  #1,d1
    .balignw 4,0x4e71
1:  move.w  d0,(a0)+
    dbf     d1,1b
    clr.w   d1
    subq.l  #1,d1
    bcc.s   1b
    nop
    rts
_pi_bus_fill32:
    nop
    subq.l  #1,d1
    .balignw 4,0x4e71
1:  move.l  d0,(a0)+
    dbf     d1,1b
    clr.w   d1
    subq.l  #1,d1
    bcc.s   1b
    nop
    rts

/* A0 destination, A1 source, D0 count >= 1 */
_pi_bus_copy8:
    nop
    subq.l  #1,d0
    .balignw 4,0x4e71
1:  move.b  (a1)+,(a0)+
    dbf     d0,1b
    clr.w   d0
    subq.l  #1,d0
    bcc.s   1b
    nop
    rts
_pi_bus_copy16:
    nop
    subq.l  #1,d0
    .balignw 4,0x4e71
1:  move.w  (a1)+,(a0)+
    dbf     d0,1b
    clr.w   d0
    subq.l  #1,d0
    bcc.s   1b
    nop
    rts
_pi_bus_copy32:
    nop
    subq.l  #1,d0
    .balignw 4,0x4e71
1:  move.l  (a1)+,(a0)+
    dbf     d0,1b
    clr.w   d0
    subq.l  #1,d0
    bcc.s   1b
    nop
    rts

pi_bus_recover:
    moveq   #-1,d0
    rts

pi_bus_window_end:

/* Exec enters tc_TrapCode in supervisor mode with the trap number (a
 * longword) on top of the processor's exception frame. Only access faults
 * (trap 2) inside the window are handled here. Their frame is replaced by
 * a format 0 frame that returns to pi_bus_recover in the faulting mode.
 * Frame sizes and fault address offsets:
 *   $A 68020/030 short bus cycle   32 bytes, address at +16
 *   $B 68020/030 long bus cycle    92 bytes, address at +16
 *   $7 68040 access error          60 bytes, address at +20
 *   $4 68060 access error          16 bytes, address at +8
 * Any other trap gives the task its own handler back and passes the trap
 * on to it unchanged. */
_pi_bus_trap:
    movem.l d0/a0/a1,-(sp)          /* saved 0-11, trap 12, frame from 16 */
    move.l  12(sp),d0
    subq.l  #2,d0
    beq.s   .Lbus
    /* Exec's vector 2 code is written for 68020/030 frames. On a 68040 or
     * 68060 it passes the access fault on through the illegal instruction
     * vector, so it arrives as trap 4 with the bus error frame. */
    subq.l  #2,d0
    bne     .Lrestore
    move.w  22(sp),d0
    and.w   #0x0fff,d0
    cmp.w   #0x0008,d0
    bne     .Lrestore
.Lbus:
    move.l  18(sp),d0               /* stacked PC */
    lea     pi_bus_window_start(pc),a0
    cmp.l   a0,d0
    bcs     .Lrestore
    lea     pi_bus_window_end(pc),a0
    cmp.l   a0,d0
    bcc     .Lrestore
    move.w  22(sp),d0               /* format and vector offset */
    lsr.w   #8,d0
    lsr.w   #4,d0
    cmp.w   #0xa,d0
    beq.s   .Lformat_ab
    cmp.w   #0xb,d0
    beq.s   .Lformat_b
    cmp.w   #7,d0
    beq.s   .Lformat_7
    cmp.w   #4,d0
    bne     .Lrestore
    move.l  16+8(sp),a0
    moveq   #16,d0
    bra.s   .Lours
.Lformat_7:
    move.l  16+20(sp),a0
    moveq   #60,d0
    bra.s   .Lours
.Lformat_b:
    move.l  16+16(sp),a0
    moveq   #92,d0
    bra.s   .Lours
.Lformat_ab:
    move.l  16+16(sp),a0
    moveq   #32,d0
.Lours:
    move.l  4.w,a1
    move.l  EXEC_THISTASK(a1),a1
    move.l  TC_TRAPDATA(a1),a1
    tst.l   TRAP_FAULTS(a1)
    bne.s   1f
    move.l  a0,TRAP_ADDRESS(a1)
1:  addq.l  #1,TRAP_FAULTS(a1)
    /* The new 8-byte frame ends where the old one ended: SP+16+size. */
    lea     8(sp,d0.w),a0
    move.w  16(sp),(a0)             /* SR of the faulting code */
    lea     pi_bus_recover(pc),a1
    move.l  a1,2(a0)
    move.w  #0x0008,6(a0)           /* format 0, vector 2 */
    move.l  8(sp),-(a0)             /* saved registers below it */
    move.l  4(sp),-(a0)
    move.l  (sp),-(a0)
    move.l  a0,sp
    movem.l (sp)+,d0/a0/a1
    rte

.Lrestore:
    movem.l (sp)+,d0/a0/a1
.Lforeign:
    move.l  a0,-(sp)                /* saved a0 at 4, trap at 8 */
    move.l  a1,-(sp)                /* saved a1 at 0 */
    move.l  4.w,a0
    move.l  EXEC_THISTASK(a0),a0
    move.l  TC_TRAPDATA(a0),a1
    move.l  TRAP_CODE(a1),TC_TRAPCODE(a0)
    move.l  TRAP_DATA(a1),TC_TRAPDATA(a0)
    move.l  TRAP_CODE(a1),-(sp)     /* handler at 0, a1 at 4, a0 at 8 */
    move.l  4(sp),a1
    move.l  8(sp),a0
    move.l  (sp),8(sp)
    addq.l  #8,sp                   /* handler, then the trap number */
    rts
