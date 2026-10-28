    .text
    .align 2
    .globl _library_start
_library_start:
    moveq #-1,d0
    rts
_library_resident:
    .word 0x4afc
    .long _library_resident
    .long _library_resident_end
#ifdef PCIINIT_LIBRARY
    .byte 0x80,PCIINIT_VERSION,9,0
#else
    .byte 0x80,5,9,0 /* X86EMU_VERSION */
#endif
    .long _lib_name
    .long _lib_ident
    .long _lib_autoinit
_library_resident_end:
