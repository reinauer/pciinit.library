# WinUAE guest fixture

Native validation runs the libraries and test programs inside an isolated
WinUAE guest: an A4000 Kickstart, a PCI bus board, an emulated S3 ViRGE PCI
card and a pinned `openpci.library`. The fixture copies its inputs and never
modifies them.

```sh
python3 tools/prepare_guest.py --rom /path/to/s3virge.bin
python3 tools/run_guest.py
python3 tools/collect_guest.py
```

`prepare_guest.py` builds `fixture/out/guest-test/` with the guest filesystem, a
`startup-sequence` for the selected `--suite`, the WinUAE configuration and
SHA-256 hashes of every input. `--bridge firestorm` (the default) emulates a
Prometheus FireStorm, whose window reaches PCI memory `0xA0000`; `--bridge
prometheus` emulates the original Matay board, on which a VGA ROM runs
against an empty window and the context reports `PI_WARN_NO_LEGACY_WINDOW`.
`--seed` names a guest directory holding `C/Assign`, `C/Execute`, `C/Wait`
and `Libs/mathieeedoubbas.library` (default `fixture/out/seed`); `--openpci`
names the provider (default `fixture/out/pinned-openpci.library`);
`--kickstart` and `--port` override the remaining inputs. `--cpu` selects a
68020 (the default), 68030, 68040 or 68060. `--master-abort` sets the
bridge's `masterabort` option, so a PCI cycle no card claims ends in a 68k
bus error instead of all-ones, and tells the backend suite to expect them;
it needs a WinUAE with that option. The supplied ROM
is copied as the emulator's card ROM and as `SYS:card.rom` for the tests
that compare both sources.

The guest configuration uses `ReserveMemSpace 0xC0000` so no BAR is
allocated into the legacy VGA area, and has no RTG driver. Suites:

| Suite | Runs |
| --- | --- |
| `foundation` | engine, ROM, library, context, allocation and VBE parser tests, `PCIoprom --no-post` |
| `backend` | `hardware-test`: owned-card policy, PIT, PCI BIOS, legacy window round trip |
| `display` | `post-test`: POST and the ROM's text service |
| `vbe` | `vbe-native-test`: mode queries, mode sets, framebuffer mapping rules |
| `post`, `vbe-cli`, `boing-cli` | `PCIoprom` POST, mode sequence, pattern or Boing Ball |
| `post-timing`, `record`, `profile`, `deadline` | measurement and recording runs |
| `bustrap` | `bustrap-test`: the bus-error trap handler with hand-built exception frames; needs no `--rom` |
| `abortprobe` | `abortprobe`: what the bridge does with a PCI cycle no device claims (all-ones, or a bus error with `--master-abort`) |

`run_guest.py` launches the emulator headless with a time limit and archives
the previous reports; `done.txt` only proves script completion. A leftover
`fixture/out/guest-test/STOP` makes the runner exit at once. `collect_guest.py`
verifies that the executed binaries match the build, that every report is
newer than the launch and contains its result line, and writes a JSON and
text summary. Inspect `guest/*.txt` for details. MCP/GDB on the configured
port is the guest-control interface for interactive investigation.

## Emulator

The emulator is the Unix port of WinUAE at the revision named in
`winuae/revision`. `post-profile.patch` adds an optional POST profiler for
this project: it counts guest cycles per 68k PC and x86 IP between the
marker NOPs of a profiling build and writes the report named by
`PCIINIT_POST_PROFILE`.

```sh
python3 tools/build_fixture_emulator.py --source /path/to/WinUAE
python3 tools/build_fixture_emulator.py --profile   # optional profiler build
```

The script exports the pinned revision from the given checkout with
`git archive`, applies the profiler patch for `--profile`, configures a Release build without the
Qt UI, Lua, archive, printer, CHD and FloppyBridge features, and writes
`fixture/out/winuae` (or `winuae-profile`) with a JSON record of the
revision, patch and binary hashes. `fixture/out/` is ignored by git and is
not touched by `make clean`; the guest runs live in
`fixture/out/guest-test/`. `run_guest.py` uses `fixture/out/winuae` unless
`--winuae` names another binary.

The emulator must forward PCI legacy VGA memory accesses at
`0xA0000-0xBFFFF` to the emulated card. BAR access alone is insufficient;
the backend suite checks the legacy window with a memory round trip.
