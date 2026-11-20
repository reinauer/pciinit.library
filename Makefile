CROSS ?= m68k-amigaos-
CC = $(CROSS)gcc
HOSTCC ?= cc
PYTHON ?= python3
VAMOS ?= vamos
OPENPCI_SDK ?= $(HOME)/git/OpenPCI/Include
CPPFLAGS = -Iinclude -I$(OPENPCI_SDK)
CFLAGS = -m68020 -O3 -g -MMD -MP -Wall -Wextra -Werror -fno-strict-aliasing -mregparm=2

# The pciinit.library version and the release name come from the nearest
# v<version>.<revision> tag, e.g. v2.4, v2.4.1 or v2.4-rc1. AmigaOS only has
# version and revision, so a package number or suffix is appended to the ID
# string instead. Checkouts without such a tag build as
# PCIINIT_FALLBACK_VERSION. Off the tag, the git description is appended.
PCIINIT_FALLBACK_VERSION = 2.3
GIT_TAG := $(shell git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null)
GIT_DESCRIBE := $(shell git describe --tags --match 'v[0-9]*' --dirty --always 2>/dev/null)
RELEASE_VERSION ?= $(or $(patsubst v%,%,$(GIT_TAG)),$(PCIINIT_FALLBACK_VERSION))
PCIINIT_VERSION := $(shell printf '%s\n' '$(RELEASE_VERSION)' | sed -n 's/^\([0-9][0-9]*\)\.[0-9][0-9]*.*/\1/p')
PCIINIT_REVISION := $(shell printf '%s\n' '$(RELEASE_VERSION)' | sed -n 's/^[0-9][0-9]*\.\([0-9][0-9]*\).*/\1/p')
ifneq ($(words $(PCIINIT_VERSION) $(PCIINIT_REVISION)),2)
$(error RELEASE_VERSION '$(RELEASE_VERSION)' is not <version>.<revision>[.<package>][-suffix])
endif
PCIINIT_DATE := $(or $(shell git log -1 --format=%cd --date=format:%d.%m.%Y 2>/dev/null),$(shell date +%d.%m.%Y))
PCIINIT_BUILD := $(strip $(if $(filter v$(RELEASE_VERSION),$(GIT_DESCRIBE)),\
    $(if $(filter $(PCIINIT_VERSION).$(PCIINIT_REVISION),$(RELEASE_VERSION)),,$(RELEASE_VERSION)),$(GIT_DESCRIBE)))
PCIINIT_VERSION_DEFS = -DPCIINIT_VERSION=$(PCIINIT_VERSION) -DPCIINIT_REVISION=$(PCIINIT_REVISION) \
    -DPCIINIT_DATE='"$(PCIINIT_DATE)"' $(if $(PCIINIT_BUILD),-DPCIINIT_BUILD='" $(PCIINIT_BUILD)"')
X86_BACKEND ?= m68kjit
ifeq ($(filter $(X86_BACKEND),libx86emu m68kjit),)
$(error Unsupported X86_BACKEND '$(X86_BACKEND)'; available: m68kjit libx86emu)
endif
BACKEND_SRC = src/x86emu/backends/$(X86_BACKEND).c src/x86emu/backends/libx86emu_adapter.c
ENGINE = decode ops ops2 prim_ops
ENGINE_SRC = $(addprefix third_party/libx86emu/,$(addsuffix .c,$(ENGINE)))
INTERPRETER_FLAGS = -Ithird_party/libx86emu/include -DX86EMU_NO_HOST_IO -DX86EMU_EXTERNAL_CLOCK -DX86EMU_DIRECT_FETCH \
    -DX86EMU_BULK_STRINGS -DX86EMU_NO_DISASSEMBLY -DX86EMU_NO_TRACE_FORMATTING
ENGINE_FLAGS = $(INTERPRETER_FLAGS)
# Generated helper calls must use the same ABI as the compiled engine.
# The last calling-convention option wins, as it does for GCC.
ifeq ($(X86_BACKEND),m68kjit)
JIT_CALL_ABI := $(lastword $(filter -mregparm% -mno-regparm -msasregparm,$(CFLAGS)))
ifneq ($(filter -mregparm -mregparm=2,$(JIT_CALL_ABI)),)
ENGINE_FLAGS += -DX86EMU_JIT_REGPARM2
else ifneq ($(filter-out -mregparm=0 -mno-regparm,$(JIT_CALL_ABI)),)
$(error m68kjit supports stack arguments or -mregparm=2, not $(JIT_CALL_ABI))
endif
endif
ENGINE_OBJ = $(addprefix build/engine/,$(addsuffix .o,$(ENGINE)))
BACKEND_OBJ = build/engine/backend.o build/engine/libx86emu_adapter.o
WRAPPER_OBJ = build/engine/engine.o $(BACKEND_OBJ) build/runtime/memory.o

.PHONY: all clean
all: build/PCIoprom build/engine-test build/rom-test build/x86emu.library build/nojit/x86emu.library build/library-test build/pciinit.library build/pciinit-test build/allocation-test

build:
	mkdir -p $@

# Rebuild consumers when a compile-time engine option changes.
.PHONY: engine-options-force
build/engine-options: engine-options-force | build
	@printf '%s\n' '$(X86_BACKEND) $(ENGINE_FLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(ENGINE_OBJ) $(WRAPPER_OBJ) build/host-engine-test build/host-bulk-test: build/engine-options

build/engine/%.o: third_party/libx86emu/%.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) -Wno-error -Wno-unused-parameter -ffreestanding -fno-builtin -c $< -o $@

build/engine/engine.o: src/x86emu/engine.c include/private/engine.h include/libraries/x86emu.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/engine/backend.o: src/x86emu/backends/$(X86_BACKEND).c include/private/backend.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/engine/libx86emu_adapter.o: src/x86emu/backends/libx86emu_adapter.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/runtime/memory.o: src/runtime/memory.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/runtime/resident.o: src/runtime/resident.S
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

build/x86emu/library.o: src/x86emu/library.c build/engine-options
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(if $(filter libx86emu,$(X86_BACKEND)),-DX86EMU_INTERPRETER_ONLY) -ffreestanding -fno-builtin -c $< -o $@

build/x86emu/lifecycle.o: src/runtime/library.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/engine/backend-library.o: src/x86emu/backends/$(X86_BACKEND).c include/private/backend.h build/engine-options build/compiler-options
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) -DX86EMU_NO_JIT_STATS -ffreestanding -fno-builtin -c $< -o $@
build/x86emu.library: build/runtime/resident.o build/x86emu/library.o build/x86emu/lifecycle.o $(ENGINE_OBJ) $(filter-out build/engine/backend.o,$(WRAPPER_OBJ)) build/engine/backend-library.o
	$(CC) -nostdlib -m68020 -Wl,-e,_library_start $(filter %.o,$^) -lgcc -o $@

# The release also carries the interpreter build as x86emu.library_nojit,
# for systems where generated code is unwelcome.
NOJIT_OBJ = $(addprefix build/nojit/,$(addsuffix .o,$(ENGINE) libx86emu libx86emu_adapter library))
build/nojit/%.o: third_party/libx86emu/%.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(INTERPRETER_FLAGS) $(CFLAGS) -Wno-error -Wno-unused-parameter -ffreestanding -fno-builtin -c $< -o $@
build/nojit/%.o: src/x86emu/backends/%.c include/private/backend.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(INTERPRETER_FLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@
build/nojit/library.o: src/x86emu/library.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DX86EMU_INTERPRETER_ONLY -ffreestanding -fno-builtin -c $< -o $@
$(NOJIT_OBJ): build/compiler-options
build/nojit/x86emu.library: build/runtime/resident.o build/x86emu/lifecycle.o build/engine/engine.o build/runtime/memory.o $(NOJIT_OBJ)
	$(CC) -nostdlib -m68020 -Wl,-e,_library_start $(filter %.o,$^) -lgcc -o $@

build/library-test: tests/library.c build/x86emu.library
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) $< -o $@

# Vamos looks for libraries next to the program first.
build/nojit/library-test: build/library-test
	mkdir -p $(@D)
	cp $< $@

.PHONY: check-library
check-library: build/library-test build/nojit/library-test build/nojit/x86emu.library
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable -V testlibs:$(CURDIR)/build -a libs:testlibs: build/library-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable -V testlibs:$(CURDIR)/build/nojit -a libs:testlibs: build/nojit/library-test

build/engine-test: tests/engine.c $(ENGINE_OBJ) $(WRAPPER_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/host-engine-test: tests/engine.c src/x86emu/engine.c $(BACKEND_SRC) src/runtime/memory.c $(ENGINE_SRC) include/private/engine.h include/private/backend.h include/private/runtime.h include/libraries/x86emu.h
	mkdir -p $(@D)
	$(HOSTCC) -MMD -MP -Iinclude $(ENGINE_FLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing $(filter %.c %.o,$^) -o $@

.PHONY: check-engine
check-engine: build/host-engine-test build/engine-test
	build/host-engine-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable build/engine-test

# Prove that lifecycle/callback policy can link without an emulator core.
build/backend-contract-test: tests/backend_contract.c src/x86emu/engine.c include/private/backend.h include/private/engine.h include/libraries/x86emu.h | build
	$(HOSTCC) -Iinclude -O2 -g -Wall -Wextra -Werror $(filter %.c,$^) -o $@

CLANG_FORMAT ?= clang-format
# Project sources only; third_party/ keeps its upstream layout.
FORMAT_SOURCES = $(shell git ls-files '*.c' '*.h' | grep -v '^third_party/')
.PHONY: format check-format
format:
	$(CLANG_FORMAT) -i --style=file $(FORMAT_SOURCES)
check-format:
	$(CLANG_FORMAT) --dry-run --Werror --style=file $(FORMAT_SOURCES)

.PHONY: check-backend
check-backend: build/backend-contract-test
	build/backend-contract-test

check: check-backend

clean:
	rm -rf build

build/pciinit/rom.o: src/pciinit/rom.c include/private/rom.h include/libraries/pciinit.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/rom-test: tests/rom.c build/pciinit/rom.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/host-rom-test: tests/rom.c src/pciinit/rom.c
	$(HOSTCC) -MMD -MP -Iinclude -O2 -g -Wall -Wextra -Werror $(filter %.c %.o,$^) -o $@

.PHONY: check-rom
check-rom: build/host-rom-test build/rom-test
	build/host-rom-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable build/rom-test

build/pciinit/%.o: src/pciinit/%.c include/private/pciinit.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffreestanding -fno-builtin -c $< -o $@

build/pciinit/bus.o: src/pciinit/bus.S
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

build/pciinit/lifecycle.o: src/runtime/library.c include/private/version.h build/version-options
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PCIINIT_VERSION_DEFS) $(CFLAGS) -DPCIINIT_LIBRARY -ffreestanding -fno-builtin -c $< -o $@

build/pciinit.library: build/pciinit/resident.o build/pciinit/library.o build/pciinit/lifecycle.o build/pciinit/image.o build/pciinit/rom.o build/pciinit/context.o build/pciinit/hardware.o build/pciinit/vbe.o build/pciinit/vbe_parse.o build/pciinit/quirks.o build/pciinit/bus.o build/runtime/memory.o
	$(CC) -nostdlib -m68020 -Wl,-e,_library_start $(filter %.o,$^) -lgcc -o $@

build/pciinit-test: tests/pciinit.c build/pciinit.library build/x86emu.library
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

.PHONY: check-pciinit
check-pciinit: build/pciinit-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable -V testlibs:$(CURDIR)/build -a libs:testlibs: build/pciinit-test

# PCIoprom formats no floating-point values; newlib's integer-only printf
# family leaves out 33 KiB of conversion code.
PCIOPROM_STDIO = -Dprintf=iprintf -Dsnprintf=sniprintf
build/PCIoprom: src/pcioprom/main.c build/pcioprom-boing.o build/pcioprom-config.o build/pcioprom-accesslog.o include/proto/pciinit.h include/inline/pciinit.h include/private/pattern.h include/private/boing.h include/private/oprom_config.h include/private/accesslog.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PCIOPROM_STDIO) $(filter %.c %.o,$^) -lm -o $@

build/pcioprom-boing.o: src/pcioprom/boing.c include/private/boing.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/pcioprom-config.o: src/pcioprom/config.c include/private/oprom_config.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PCIOPROM_STDIO) -c $< -o $@

build/pcioprom-accesslog.o: src/pcioprom/accesslog.c include/private/accesslog.h include/libraries/pciinit.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PCIOPROM_STDIO) -c $< -o $@

build/host-accesslog-test: tests/accesslog.c src/pcioprom/accesslog.c include/private/accesslog.h include/libraries/pciinit.h
	$(HOSTCC) -Iinclude -O2 -Wall -Wextra -Werror $(filter %.c,$^) -o $@

build/accesslog-test: tests/accesslog.c build/pcioprom-accesslog.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

.PHONY: check-accesslog
check-accesslog: build/host-accesslog-test build/accesslog-test
	build/host-accesslog-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-accesslog -q -C 68020 -s 128 -m 16384 -H disable build/accesslog-test

check: check-accesslog

build/host-oprom-config-test: tests/oprom_config.c src/pcioprom/config.c include/private/oprom_config.h
	$(HOSTCC) -Iinclude -O2 -Wall -Wextra -Werror $(filter %.c,$^) -o $@

build/oprom-config-test: tests/oprom_config.c build/pcioprom-config.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

.PHONY: check-oprom-config
check-oprom-config: build/host-oprom-config-test build/oprom-config-test
	build/host-oprom-config-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-oprom-config -q -C 68020 -s 128 -m 16384 -H disable build/oprom-config-test

check: check-oprom-config

build/host-boing-test: tests/boing.c src/pcioprom/boing.c include/private/boing.h
	$(HOSTCC) -Iinclude -O2 -Wall -Wextra -Werror $(filter %.c,$^) -lm -o $@

build/boing-test: tests/boing.c build/pcioprom-boing.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -lm -o $@

.PHONY: check-boing
check-boing: build/host-boing-test build/boing-test
	build/host-boing-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-boing -q -C 68020 -s 128 -m 16384 -H disable build/boing-test

check: check-boing

build/allocation-test: tests/allocation.c build/pciinit/image.o build/pciinit/rom.o build/pciinit/context.o build/pciinit/hardware.o build/pciinit/quirks.o build/pciinit/bus.o build/engine/engine.o $(BACKEND_OBJ) $(ENGINE_OBJ) build/x86emu.library
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

.PHONY: check-allocation check
check-allocation: build/allocation-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable -V testlibs:$(CURDIR)/build -a libs:testlibs: build/allocation-test

check: check-engine check-rom check-library check-pciinit check-allocation

-include $(wildcard build/*.d build/*/*.d)

build/hardware-test: tests/hardware.c build/pciinit/hardware.o build/pciinit/context.o build/pciinit/image.o build/pciinit/rom.o build/pciinit/quirks.o build/pciinit/bus.o build/runtime/memory.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/hook-test: tests/hook.c build/pciinit/hardware.o build/pciinit/context.o build/pciinit/image.o build/pciinit/rom.o build/pciinit/quirks.o build/pciinit/bus.o build/runtime/memory.o build/compiler-options
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@
.PHONY: check-hook
check-hook: build/hook-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-hook -q -C 68020 -s 128 -m 16384 -H disable build/hook-test
check: check-hook

# Native only: enters the bus-error trap handler with hand-built exception
# frames from supervisor mode, which vamos cannot run. See tests/bustrap.c.
build/bustrap-test: tests/bustrap.c tests/bustrap_s.S build/pciinit/bus.o build/compiler-options
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.S %.o,$^) -o $@

# Native only, for real bus boards: what a bridge does with a PCI cycle no
# device claims. Puts its own handler on the bus error vector; see
# tests/abortprobe.c.
build/abortprobe: tests/abortprobe.c tests/abortprobe_s.S build/compiler-options
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.S %.o,$^) -o $@

build/post-test: tests/post.c build/pciinit/image.o build/pciinit/rom.o build/pciinit/context.o build/pciinit/hardware.o build/pciinit/vbe.o build/pciinit/vbe_parse.o build/pciinit/quirks.o build/pciinit/bus.o build/runtime/memory.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/pciinit/resident.o: src/runtime/resident.S build/version-options
	mkdir -p $(@D)
	$(CC) $(PCIINIT_VERSION_DEFS) $(CFLAGS) -DPCIINIT_LIBRARY -c $< -o $@

build/pciinit/library.o: include/private/version.h build/version-options
build/pciinit/library.o: CPPFLAGS += $(PCIINIT_VERSION_DEFS)

# Rebuild the objects carrying the version when the tag or commit changes.
.PHONY: version-options-force
build/version-options: version-options-force | build
	@printf '%s\n' '$(PCIINIT_VERSION).$(PCIINIT_REVISION) $(PCIINIT_DATE) $(PCIINIT_BUILD)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

.PHONY: version
version:
	@echo 'pciinit.library $(PCIINIT_VERSION).$(PCIINIT_REVISION) ($(PCIINIT_DATE))$(if $(PCIINIT_BUILD), $(PCIINIT_BUILD))'
	@echo 'release pciinit-$(RELEASE_VERSION)'

build/vbe-native-test: tests/vbe_native.c include/private/pattern.h build/pciinit.library
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

build/vbe-parse-test: tests/vbe_parse.c build/pciinit/vbe_parse.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/host-vbe-parse-test: tests/vbe_parse.c src/pciinit/vbe_parse.c include/private/vbe.h include/libraries/pciinit.h
	$(HOSTCC) -Iinclude -O2 -g -Wall -Wextra -Werror $(filter %.c,$^) -o $@

.PHONY: check-vbe check-vbe-api
check-vbe: build/host-vbe-parse-test build/vbe-parse-test
	build/host-vbe-parse-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-$@ -q -C 68020 -s 128 -m 16384 -H disable build/vbe-parse-test

all: build/vbe-api-test build/vbe-parse-test build/vbe-native-test

check: check-vbe check-vbe-api

build/vbe-api-test: tests/vbe_api.c build/pciinit.library build/x86emu.library
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

check-vbe-api: build/vbe-api-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-vbe-api -q -C 68020 -s 128 -m 16384 -H disable -V testlibs:$(CURDIR)/build -a libs:testlibs: build/vbe-api-test

LHA ?= lha
.PHONY: release
release: build/pciinit.library build/x86emu.library build/nojit/x86emu.library build/PCIoprom
	$(PYTHON) tools/make_release.py --lha "$(LHA)" --strip "$(CROSS)strip" --version "$(RELEASE_VERSION)"

# Native JIT coverage requires m68k execution; host builds use the fallback.
build/mapped-engine-test: tests/engine.c $(ENGINE_OBJ) $(WRAPPER_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DENGINE_MAPPED $(filter %.c %.o,$^) -o $@

ifeq ($(X86_BACKEND),m68kjit)
build/blockjit-test: tests/blockjit.c $(ENGINE_OBJ) $(WRAPPER_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@
.PHONY: check-jit
check-jit: build/blockjit-test build/mapped-engine-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-jit -q -C 68020 -s 128 -m 16384 -H disable build/blockjit-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-jit -q -C 68020 -s 128 -m 16384 -H disable build/mapped-engine-test
check: check-jit
endif

build/post-timing: tests/post_timing.c include/proto/pciinit.h include/private/openpci_calls.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

build/host-bulk-test: tests/bulk.c src/x86emu/engine.c $(BACKEND_SRC) src/runtime/memory.c $(ENGINE_SRC) include/private/backend.h include/libraries/x86emu.h
	$(HOSTCC) -Iinclude $(ENGINE_FLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing $(filter %.c,$^) -o $@

build/bulk-test: tests/bulk.c $(ENGINE_OBJ) $(WRAPPER_OBJ) Makefile
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

.PHONY: check-bulk
check-bulk: build/host-bulk-test build/bulk-test
	build/host-bulk-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-bulk -q -C 68020 -s 128 -m 16384 -H disable build/bulk-test
check: check-bulk

# Recorder hooks exist only in this diagnostic executable, never the library.
RECORD_OBJ = $(addprefix build/record/,$(addsuffix .o,image rom context hardware quirks))
build/record/%.o: src/pciinit/%.c include/private/record.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPCIINIT_RECORD -ffreestanding -fno-builtin -c $< -o $@
build/post-record: tests/post_record.c $(RECORD_OBJ) build/pciinit/bus.o build/runtime/memory.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPCIINIT_RECORD $(filter %.c %.o,$^) -o $@

REPLAY_FLAGS = -DREPLAY_JIT=$(if $(filter m68kjit,$(X86_BACKEND)),1,0)
build/host-replay: tests/replay.c src/x86emu/engine.c $(BACKEND_SRC) src/runtime/memory.c $(ENGINE_SRC) build/engine-options
	$(HOSTCC) -Iinclude $(ENGINE_FLAGS) $(REPLAY_FLAGS) -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing $(filter %.c,$^) -o $@
build/replay: tests/replay.c $(ENGINE_OBJ) $(WRAPPER_OBJ)
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(REPLAY_FLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/host-pit-test: tests/pit.c include/private/pit.h
	$(HOSTCC) -Iinclude -O2 -Wall -Wextra -Werror $< -o $@
build/pit-test: tests/pit.c include/private/pit.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@
.PHONY: check-pit
check-pit: build/host-pit-test build/pit-test
	build/host-pit-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-pit -q -C 68020 -s 128 -m 16384 -H disable build/pit-test
check: check-pit

build/host-quirks-test: tests/quirks.c src/pciinit/quirks.c include/private/quirks.h
	$(HOSTCC) -Iinclude -O2 -g -Wall -Wextra -Werror $(filter %.c,$^) -o $@
build/quirks-test: tests/quirks.c build/pciinit/quirks.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@
.PHONY: check-quirks
check-quirks: build/host-quirks-test build/quirks-test
	build/host-quirks-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-quirks -q -C 68020 -s 128 -m 16384 -H disable build/quirks-test
check: check-quirks

build/memory-test: tests/memory.c build/runtime/memory.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -fno-builtin $(filter %.c %.o,$^) -o $@
.PHONY: check-memory
check-memory: build/memory-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-memory -q -C 68020 -s 128 -m 16384 -H disable build/memory-test
check: check-memory

# Rebuild native objects and test executables when compiler settings change.
.PHONY: compiler-options-force
build/compiler-options: compiler-options-force | build
	@printf '%s\n' '$(CC) $(CPPFLAGS) $(CFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(ENGINE_OBJ) $(WRAPPER_OBJ) $(RECORD_OBJ): build/compiler-options
$(patsubst src/%.c,build/%.o,$(wildcard src/pciinit/*.c)) build/pciinit/bus.o build/pcioprom-boing.o build/pcioprom-config.o \
    build/pciinit/resident.o build/pciinit/library.o build/pciinit/lifecycle.o \
    build/x86emu/library.o build/x86emu/lifecycle.o build/runtime/resident.o: build/compiler-options

build/PCIoprom build/post-timing build/rom-test build/library-test \
    build/pciinit-test build/allocation-test build/hardware-test \
    build/vbe-native-test build/vbe-api-test build/vbe-parse-test \
    build/boing-test build/engine-test build/mapped-engine-test build/blockjit-test \
    build/bulk-test build/pit-test build/memory-test: build/compiler-options

build/deadline-native-test: tests/deadline_native.c $(ENGINE_OBJ) $(WRAPPER_OBJ) build/compiler-options
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@

build/scan-test: tests/scan.c $(ENGINE_OBJ) $(WRAPPER_OBJ) build/compiler-options
	$(CC) $(CPPFLAGS) $(ENGINE_FLAGS) $(CFLAGS) $(filter %.c %.o,$^) -o $@
build/host-scan-test: tests/scan.c src/x86emu/engine.c $(BACKEND_SRC) $(ENGINE_SRC) src/runtime/memory.c
	$(HOSTCC) $(CPPFLAGS) $(ENGINE_FLAGS) -O2 -fno-strict-aliasing $(filter %.c,$^) -o $@
check-scan: build/scan-test build/host-scan-test
	build/host-scan-test
	$(VAMOS) -S --vols-base-dir $(CURDIR)/build/vamos-scan -q -C 68020 -s 128 -m 16384 -H disable build/scan-test
check: check-scan

