/* Supervisor half of tests/bustrap.c: enter pi_bus_trap the way exec
 * does, with the trap number on top of a hand-built exception frame.
 *
 * The frame's SR is the current supervisor SR, so the handler's RTE stays
 * in supervisor mode on this stack. A return address to .Lcont is pushed
 * before the frame. A recovered fault RTEs to pi_bus_recover, whose RTS
 * pops it. A passed-on trap reaches bt_old_handler, which drops the trap
 * number and RTEs to bt_rts, whose RTS pops it the same way. Either way
 * the registers and the stack pointer are recorded at .Lcont before
 * returning from Supervisor(). */

    .text
    .align 2
    .globl _bt_entry, _bt_old_handler, _bt_rts
    .globl _bt_size, _bt_pc, _bt_fmtvec, _bt_addr_off, _bt_addr, _bt_trap
    .globl _bt_regs, _bt_sp_before, _bt_sp_after, _bt_seen_trap

_bt_entry:
    movem.l d2-d7/a2-a6,-(sp)
    move.l  sp,_bt_sp_before
    pea     .Lcont(pc)
    move.l  _bt_size,d0
    sub.l   d0,sp
    move.l  sp,a0
    lsr.l   #1,d0
1:  clr.w   (a0)+
    subq.l  #1,d0
    bne.s   1b
    move.w  sr,(sp)
    move.l  _bt_pc,2(sp)
    move.w  _bt_fmtvec+2,6(sp)
    move.l  _bt_addr_off,d1
    move.l  _bt_addr,0(sp,d1.l)
    move.l  _bt_trap,-(sp)
    move.l  #0x11111111,d0
    move.l  #0x22222222,d1
    move.l  #0x33333333,a0
    move.l  #0x44444444,a1
    jmp     _pi_bus_trap
.Lcont:
    movem.l d0-d1/a0-a1,_bt_regs
    move.l  sp,_bt_sp_after
    movem.l (sp)+,d2-d7/a2-a6
    rte

/* Stands in for the task's own handler. Passed-on cases use format 0
 * frames, so it can resume at bt_rts whatever PC the frame held. */
_bt_old_handler:
    move.l  (sp)+,_bt_seen_trap
    move.l  #_bt_rts,2(sp)
    rte

_bt_rts:
    rts

    .bss
    .align 2
_bt_size:      .space 4
_bt_pc:        .space 4
_bt_fmtvec:    .space 4
_bt_addr_off:  .space 4
_bt_addr:      .space 4
_bt_trap:      .space 4
_bt_regs:      .space 16
_bt_sp_before: .space 4
_bt_sp_after:  .space 4
_bt_seen_trap: .space 4
