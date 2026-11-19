# Library ABI

Both libraries need a 68020 and AmigaOS 3.0 (Exec/DOS V39). They are
Resident/AutoInit Hunk libraries; the public vectors are listed in `fd/`.
Amiga GCC clients use the `proto/` and `inline/` headers. Public structures
start with a `size` field that must equal `sizeof` the structure; unknown
sizes and versions are rejected. Do not compile clients with `-mshort`.

## x86emu.library

| Function | Operation |
| --- | --- |
| `X86Create(config)` | Create a CPU context from an `X86Config` |
| `X86Delete(context)` | Free it |
| `X86GetRegisters(context, regs)` | Copy the register state out |
| `X86SetRegisters(context, regs)` | Set a real-mode entry state |
| `X86Run(context, limits, result)` | Execute until a stop condition |
| `X86ContextBytes()` | Size of the private context |

Callbacks use the stack calling convention and must preserve D2-D7 and
A2-A6. Declare them with `X86_CALLBACK`. The library does not use
A4-relative data; pass caller state through the `user` pointer, or restore
A4 in a trampoline.

`X86Config` holds the memory and interrupt callbacks, up to eight
executable code regions, the optional bulk and scan callbacks, the largest
bulk request and the data-region mask. Set `size` to `sizeof(struct
X86Config)` and `version` to `X86EMU_ABI_VERSION`. Open the library with
`X86EMU_VERSION`.

`X86Run()` requires nonzero instruction and access budgets; the time limit
and the cancellation pointer are optional. `X86Result.reason` tells halt,
budget, access failure, CPU fault, cancellation, timeout, bad arguments and
a busy context apart. Protected-mode work inside a run stays private.

A context has one caller at a time. Calling get, set, run or delete on a
context from inside its own callback returns BUSY. Delete every context
before closing the library.

### Bulk and scan callbacks

The bulk callback receives a contiguous prefix of REP MOVS or STOS: element
width 1, 2 or 4, guest linear source and destination, and a stride of plus
or minus the width. FILL stores the low `width` bytes of `value`,
little-endian; COPY reads each source element before writing its
destination. The callback returns how many elements it completed, from 0
to `count`, and must not touch the rest; 0 declines. Side effects, aliasing
and VGA latch behavior must match the scalar callback, so decline anything
you cannot prove, and never use a RAM copy for MMIO or port I/O. The
request storage is only valid during the call.

`bulk_elements` sets the largest request, from 1 to `X86_SPAN_MAX`
(65,536); 0 means 256. A request never exceeds the remaining access budget; FILL counts
one access per element and COPY two. Cancellation and timeout are checked
before each request. Complete the longest prefix you can rather than
declining the whole request; after a decline the engine runs scalar
accesses up to the next 256-access boundary. The scan callback handles REP
SCAS the same way, limited to 256 elements.

`data_regions` marks code regions, one bit each, as plain RAM for data
accesses: the engine reads and writes them directly and the access callback
no longer sees those accesses. Other code regions stay fetch-only and
their data accesses still reach the callback.

Faults inside a REP leave the partial register state of the reference
interpreter; instructions are not rolled back. Protected mode and
single-stepping use the scalar path.

## pciinit.library

| Function | Operation |
| --- | --- |
| `LoadOptionROMFromFile(path, tags)` | Load and validate a ROM file |
| `LoadOptionROMFromCard(board, tags)` | Copy a card's ROM |
| `GetOptionROMInfo(image, info)` | Image offset, length, PCI identity, loaded bytes |
| `FreeOptionROM(image)` | Free the image |
| `CreatePCIContext(board, tags)` | Own the card, allocate CPU state and 1 MiB guest RAM |
| `RunOptionROM(ctx, image)` | Run POST once |
| `GetPCIContextInfo(ctx, info)` | State, execution result, warnings |
| `DeletePCIContext(ctx)` | Release everything |
| `GetVBEInfo(ctx, info)` | VBE 4F00h controller info |
| `GetVBEModeInfo(ctx, mode, info)` | VBE 4F01h mode info |
| `SetVBEMode(ctx, mode, flags)` | VBE 4F02h, verified with 4F03h |
| `GetCurrentVBEMode(ctx, mode)` | VBE 4F03h, raw BX |
| `MapFramebuffer(ctx, fb)` | Temporary CPU mapping of a linear mode |
| `UnmapFramebuffer(ctx)` | Release it |
| `WriteFramebuffer(ctx, offset, source, bytes)` | Copy pixels into a banked mode |
| `ReadFramebuffer(ctx, offset, destination, bytes)` | Read them back |

Open the library with `PCIINIT_LIBRARY_VERSION`. Calls return
`enum PCIInitStatus`. Calls returning a pointer report their
status through the optional `PII_Error` tag. Unknown tags fail; tag lists
support DONE, IGNORE, SKIP and MORE up to 256 items. Unsuccessful calls
leave output structures unchanged.

### ROM images

Images are at most 1 MiB with up to 64 chained PCIR images. Every image
must be well-formed and each x86 image must checksum to zero; the first x86
image is used. ROMs without a PCIR structure are rejected. Loading from a
card temporarily enables the ROM and memory decode, copies the bytes and
restores the command and ROM registers; it does not move resources.
`GetOptionROMInfo` returns a read-only pointer to the loaded bytes, valid
while the image exists.

With `PII_Quirks` on (the default), `RunOptionROM()` patches known images,
identified by CRC-32 and length, in its RAM copy before running them. The
table is in `src/pciinit/quirks.c`. The card and the image object are never
modified.

### Contexts

A card context owns its card until deletion; loading that card's ROM or
creating another context for it returns `PI_CARD_BUSY`. OpenPCI must open
at version 14 and provide vectors through `ReleasePCIRegion`. Running the
ROM on the card needs `PII_Hardware=1`. `RunOptionROM()` returns
`PI_UNSUPPORTED_MAPPING` before anything runs if that tag is missing, the
bus board has no legacy I/O window, or another display controller already
decodes VGA I/O. Bus mastering is turned off while the
ROM runs and the original command word is restored afterwards. POST changes
VGA registers and VRAM; a failure does not undo that.

`PII_Offline=1` with a NULL board creates a context with RAM and ROM shadow
only; every port and VGA access fails.

`PII_Limits` copies an `X86Limits`; all three budgets must be nonzero. The
defaults are 1,000,000 instructions, 10,000,000 accesses and 5 s offline,
and 10,000,000 instructions, 30,000,000 accesses and 300 s on hardware. A
cancellation pointer must stay valid until the context is deleted.

POST must return through the HLT trampoline; success moves the context to
INITIALIZED and keeps its shadow after the image is freed. A fault, a
limit, a missing BIOS service or a HLT elsewhere moves it to FAILED. POST
runs once per context. Callers serialize calls on a context.

`PCIInitContextInfo.warnings` collects `PI_WARN_*` bits from POST and the
BIOS calls after it:

- `PI_WARN_NO_LEGACY_WINDOW`: the bus board could not supply PCI memory
  `A0000-BFFFF`. The ROM still ran; reads there returned zero and writes
  were dropped.
- `PI_WARN_MISSING_SERVICE`: the ROM called a software interrupt that
  neither its own vectors nor the library answer. The run stops as a fault
  and `missing_service` holds the vector in the upper half and AX in the
  lower half, e.g. INT 15h AX=F965h.
- `PI_WARN_BUS_ERROR`: a card access ended in a 68k bus error. The library
  treats it as an aborted PCI cycle: the read returned all-ones, the write
  was dropped, and a bulk transfer stopped at that element. `bus_errors`
  counts them since POST started and `bus_error_address` holds the Amiga
  address of the first. With `PII_StopOnBusError=1` the access stops the
  run as an access fault instead, and string transfers to the legacy window
  run element by element so the fault names the instruction.

`GetPCIContextInfo()` also accepts the 2.3 structure size, which ends
before `bus_errors`, and fills only that much.

Bus errors are caught by installing a trap handler as the calling task's
`tc_TrapCode` while POST or a BIOS call runs. It handles access faults
raised by the library's own card loads and stores for the ROM, in frame
formats `$A`, `$B` (68020/030), `$7` (68040) and `$4` (68060). Any other trap restores the task's own handler
and data and is passed to it unchanged. A bridge that stalls the bus
instead of ending the cycle with a bus error cannot be recovered.

Exec's bus error vector (Kickstart 3.1 and 3.2) decides what reaches the
task. On the 68020 and 68030 it completes a data fault itself: the read
returns zero, nothing is counted and no warning is set. On the 68040 and
68060 it reads the frame as a 68020/030 one. When bit 8 of the fault
address is clear it passes the fault on as trap 4, which the handler
recognizes by the bus error vector in the frame; when that bit is set it
returns into the faulting instruction, which faults again and the machine
hangs before any task handler runs. The CPU libraries tested (MMULib
68040/68060.library 47.2) pass bus errors on to exec's vector unchanged.

`PII_ServiceLog` points to a caller-owned `PCIInitServiceLog` with
`capacity` 0..256 entries. Each POST and BIOS call resets `count`. A
missing service then fails as unsupported BIOS functions do, with AH=86h
and CF set, and the run continues; each distinct vector/AX pair is stored
once while there is room. `PI_WARN_MISSING_SERVICE` and `missing_service`
still name the first one. Answering a service with failure can send a ROM
down a path it never takes on a PC, so use the log to find missing
services, not to run ROMs.

`PII_AccessHook` points to a `struct Hook` that sees the card's own
accesses before they reach the bus: I/O ports the library forwards to the
card (VGA ports and the PCI I/O space, not the PC platform ports it
emulates), the legacy window, memory BARs and the card's configuration
space through CF8h/CFCh or the PCI BIOS. `h_Entry` is called with the hook
in A0, the context in A2 and a `PCIInitAccessMsg` in A1, in the calling
task while a library call runs. It must preserve D2-D7 and A2-A6, and its
time counts toward the context's limits. `space`, `bar` and `offset` locate
the access: a port number, an offset from A0000h, an offset into BAR
`bar`, or a configuration register. `value` is the guest's numeric value.

- `PI_PHASE_BEFORE` comes first for every access. `PI_HOOK_PASS` performs
  it, a write with the possibly changed `value`. `PI_HOOK_DONE` completes it
  without the card: a read returns `value` and a write is dropped.
- `PI_PHASE_AFTER` follows a passed read with the value the card returned,
  which the hook may change.
- `PI_HOOK_FAULT`, or any unknown return value, stops the run as an access
  fault.

A passed configuration write still goes through the write filter
described below, so the bus master bit stays clear. A hook that completes
a configuration write with `PI_HOOK_DONE` is responsible for what it writes
itself. A BAR the bridge hides is reachable only through `PI_HOOK_DONE`.
Accesses a hook changed or completed carry `PI_TRACE_HOOKED` in their
diagnostics trace entries.

`PII_AccessRanges` points to up to `PI_MAX_ACCESS_RANGES` ranges ended by
`space == PI_SPACE_END` and requires a hook. The hook is then called only
for accesses that overlap a range; without the tag it sees every card
access. String transfers to the legacy window that overlap a hooked range
run element by element.

`PII_Diagnostics` points to a caller-owned `PCIInitDiagnostics` with
`capacity` 0..4096 trace entries. Keep it valid until deletion and read it
only between calls. Each POST and BIOS call resets it; it records the final
registers, the INT 10h vector, access counts and the last `capacity` I/O,
VGA, BAR and interrupt operations. Entry `n` is at `n % capacity`.
Interrupt entries carry the vector in `address`, the fault flag in `width`
and EAX in `value`.

### The emulated PC

POST starts at C000:0003 with the card's bus/device/function in AX,
SS:SP=0000:6FF0 and a far return to the trampoline. The BIOS data area
reports 640 KiB. F0000-FFFFF is read-only and holds an IRET at each fixed
IBM BIOS entry point and the AT model byte. There is no A20 wrap, PnP or
PMM.

The ROM sees the PCI BIOS and CF8/CFC configuration access for its own card
only; other devices read as absent, and configuration writes other than
the command register's I/O and memory enables are refused. VGA ports
`3B0-3DF` are forwarded bytewise. Other ports go to the PCI bus at the
guest's width, except the ones a PC chipset answers itself below `100h` and
`CF8-CFF`. Memory accesses to the legacy window and the card's BARs keep
the guest's width and byte order. Anything else is refused. While the ROM
has turned off its card's I/O or memory decode, the library completes the
accesses that decode covers as a PC completes a master abort, without a
bus cycle: reads return all-ones and writes are dropped. The access hook
still sees them first.

The PIT is timed by the EClock. Channel 0 runs free as a system BIOS
leaves it, mode 3 with a reload of 65536, and answers latch commands,
reads and reprogramming through ports 40h and 43h; channel 2 runs mode 3
with count latching, and port 61h reflects its gate and output. Channel 1
and the read-back command are refused. The DMA page registers `80h-8Fh`
are read/write latches, so POST codes and delay writes there are absorbed.
Where the ROM has installed no vector, the system BIOS answers as a PC with nothing
attached: INT 10h, 42h and 6Dh keep the BDA mode, cursor and page and
discard output; INT 15h returns AH=86h with CF set; INT 16h reports no key
and returns a space to a blocking read; INT 1Ah returns the 18.2 Hz tick
count. INT 13h and everything else are missing services.

### VBE calls

Each call runs
the ROM's INT 10h handler with fresh registers and the context's limits;
guest RAM persists between calls. The library uses 0000:7200 for the return
trampoline and 0000:8000-81FF for exchange data. AX=004Fh is success. A
BIOS error returns `PI_VBE_FAILED` and leaves the context INITIALIZED; an
execution failure returns `PI_EXECUTION_FAILED` and leaves it FAILED.

Controller info holds the OEM string (127 characters at most), up to 256
mode numbers, the VBE version, capabilities and memory size. Malformed
signatures, lists and far pointers are rejected. A mode must be in the
controller list to be set.

`SetVBEMode` takes single-plane graphics modes with 8-bit indexed or 15-,
16-, 24- or 32-bit direct color. `flags` is 0 for the banked mode or
`PI_VBE_LINEAR` for a VBE 2.0+ linear mode, optionally with
`PI_VBE_KEEP_MEMORY`. Palette, display start, custom timings and the
protected-mode interface are not exposed.

### Framebuffer access

`MapFramebuffer` needs a linear mode set in this context. The pixels must
lie inside one of the card's memory BARs; otherwise, and for banked modes,
it returns `PI_UNSUPPORTED_MAPPING`. `address` is valid until the matching
`UnmapFramebuffer`; `mode.framebuffer_pci` is a PCI address, never an Amiga
pointer. `bytes` is `pitch * height`. With `PI_FB_LITTLE_ENDIAN`, swap
words and longs or write bytes; `PI_FB_INDEXED` means palette indices.

Only one mapping may exist at a time. While it does, VBE calls, card
access and deleting its context return `PI_BAD_STATE`. Unmap promptly: the
OpenPCI region may block other windows or interrupts, so do not call DOS,
wait or touch other PCI regions while mapped. A failed restore returns
`PI_IO_ERROR` but still releases the mapping.

`WriteFramebuffer` and `ReadFramebuffer` serve banked modes, which is all a
VBE 1.2 ROM has. They take a byte offset into the visible pixels and switch
banks with VBE function 05h. They need the legacy VGA window and return
`PI_UNSUPPORTED_MAPPING` without it or for a linear mode. A bus error the
trap handler catches (see `PI_WARN_BUS_ERROR`) ends the transfer at that
bank with `PI_IO_ERROR`; it is counted like one during a BIOS call.

Deleting a context leaves the last mode on screen.
