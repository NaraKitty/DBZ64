# N64 build (run in WSL with N64_INST=/opt/libdragon), from the build
# directory, which receives dbz.z64:
#   cd BUILD_DIR && make -f ~/dbzn64/platform/n64/Makefile [GAME=log2|buu] \
#       BUILD_DIR=$PWD BASEROM=/mnt/d/dbz-n64/rom/baserom.gba
# GAME picks games/$(GAME) (game.h, game sources); default log2.
# Generated code comes from tools/recomp.py (GEN), build output goes to
# BUILD_DIR; neither is in the repository.  dbz.z64 embeds the GBA ROM:
# never build it inside the (synced) repository directory.
# make cannot handle spaces in paths; the repo lives under "DBZ LoG2 N64", so
# build through a space-free symlink (ln -s ".../DBZ LoG2 N64" ~/dbzn64)
REPO      ?= $(HOME)/dbzn64
GAME      ?= log2
ifeq ($(GAME),log2)
GEN       ?= /mnt/d/dbz-n64/gen/c
BASEROM   ?= /mnt/d/dbz-n64/rom/baserom.gba
BUILD_DIR ?= /mnt/d/dbz-n64/build/n64
else
GEN       ?= /mnt/d/dbz-n64/$(GAME)/gen/c
BASEROM   ?= /mnt/d/dbz-n64/rom/$(GAME)_baserom.gba
BUILD_DIR ?= /mnt/d/dbz-n64/$(GAME)/build/n64
endif
GAME_DIR  := $(REPO)/games/$(GAME)
ifeq ($(wildcard $(GAME_DIR)/game.h),)
$(error unknown GAME '$(GAME)': no $(GAME_DIR)/game.h)
endif
# dbz.z64 (with the GBA ROM inside) is written to the current directory
ifeq ($(shell case "$$(pwd -P)/" in ("$$(cd '$(REPO)' && pwd -P)/"*) echo inside;; esac),inside)
$(error run make from a build directory outside the repository (make -f .../platform/n64/Makefile): dbz.z64 contains the GBA ROM)
endif
SOURCE_DIR := $(REPO)
N64_MKDFS_ROOT := $(BUILD_DIR)/filesystem

include $(N64_INST)/include/n64.mk

RUNTIME_SRC := runtime/mem.c runtime/io.c runtime/bios.c runtime/dispatch.c \
               runtime/runtime.c runtime/interp.c runtime/ppu_soft.c runtime/native.c
GAME_SRC := $(patsubst $(REPO)/%,%,$(wildcard $(GAME_DIR)/*.c))
# USE_RDP=1 (default): draw with the RDP renderer (platform/n64/render);
# USE_RDP=0: the CPU reference renderer (runtime/ppu_soft.c, native view only)
USE_RDP ?= 1
PLATFORM_SRC := $(wildcard $(REPO)/platform/n64/*.c) $(if $(filter 1,$(USE_RDP)),$(wildcard $(REPO)/platform/n64/render/*.c))
PLATFORM_SRC := $(patsubst $(REPO)/%,%,$(PLATFORM_SRC))
GEN_SRC := $(wildcard $(GEN)/gen_*.c)

OBJS := $(addprefix $(BUILD_DIR)/,$(RUNTIME_SRC:.c=.o) $(GAME_SRC:.c=.o) $(PLATFORM_SRC:.c=.o)) \
        $(patsubst $(GEN)/%.c,$(BUILD_DIR)/gen/%.o,$(GEN_SRC))

# TLB=1: guest memory mapped through the TLB (platform/n64/tlb.c)
TLB ?= 1
ifeq ($(TLB),1)
MODE_FLAGS := -DGBA_TLB
endif
ifeq ($(USE_RDP),1)
MODE_FLAGS += -DUSE_RDP
endif

CFLAGS += $(MODE_FLAGS) -I$(REPO)/runtime -I$(GAME_DIR) -I$(REPO)/platform/n64 -I$(REPO)/platform/n64/render -I$(GEN) \
          -fno-strict-aliasing -Wno-error=format

# Generated code: no -Werror, no pattern-initialised locals (they would cost
# a store per local), and strict aliasing off like the runtime.
GEN_CFLAGS = $(filter-out -O2 -Werror -ftrivial-auto-var-init=pattern,$(N64_CFLAGS)) \
             -O2 -fno-strict-aliasing -w -MMD $(MODE_FLAGS) \
             -I$(REPO)/runtime -I$(GAME_DIR) -I$(GEN)

$(BUILD_DIR)/gen/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	@echo "    [GEN] $<"
	$(N64_CC) -c $(GEN_CFLAGS) -o $@ $<

all: dbz.z64

$(N64_MKDFS_ROOT)/rom.bin: $(BASEROM) $(REPO)/tools/mkrom.py $(GAME_DIR)/game.h
	@mkdir -p $(dir $@)
	python3 $(REPO)/tools/mkrom.py --game $(GAME) $(BASEROM) $@

$(BUILD_DIR)/dbz.dfs: $(N64_MKDFS_ROOT)/rom.bin $(wildcard $(N64_MKDFS_ROOT)/*.txt)
$(BUILD_DIR)/dbz.elf: $(OBJS)

# cartridge header title from game.h (GAME_N64_ROM_TITLE)
GAME_ROM_TITLE := $(shell sed -n 's/.*define GAME_N64_ROM_TITLE *\("[^"]*"\).*/\1/p' $(GAME_DIR)/game.h)

dbz.z64: N64_ROM_TITLE = $(GAME_ROM_TITLE)
dbz.z64: N64_ROM_SAVETYPE = sram256k
dbz.z64: N64_ROM_EXPANSIONPAK = required
dbz.z64: $(BUILD_DIR)/dbz.dfs

clean:
	rm -rf $(BUILD_DIR)/runtime $(BUILD_DIR)/games $(BUILD_DIR)/platform $(BUILD_DIR)/gen $(BUILD_DIR)/dbz.* dbz.z64

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

.PHONY: all clean
