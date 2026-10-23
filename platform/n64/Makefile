# N64 build (run in WSL with N64_INST=/opt/libdragon):
#   make -C platform/n64 BASEROM=/mnt/d/dbz-n64/rom/baserom.gba
# Generated code comes from tools/recomp.py (GEN), build output goes to
# BUILD_DIR; neither is in the repository.
# make cannot handle spaces in paths; the repo lives under "DBZ LoG2 N64", so
# build through a space-free symlink (ln -s ".../DBZ LoG2 N64" ~/dbzn64)
REPO      ?= $(HOME)/dbzn64
GEN       ?= /mnt/d/dbz-n64/gen/c
BASEROM   ?= /mnt/d/dbz-n64/rom/baserom.gba
BUILD_DIR ?= /mnt/d/dbz-n64/build/n64
SOURCE_DIR := $(REPO)
N64_MKDFS_ROOT := $(BUILD_DIR)/filesystem

include $(N64_INST)/include/n64.mk

RUNTIME_SRC := runtime/mem.c runtime/io.c runtime/bios.c runtime/dispatch.c \
               runtime/runtime.c runtime/interp.c runtime/ppu_soft.c runtime/native.c
PLATFORM_SRC := $(wildcard $(REPO)/platform/n64/*.c) $(if $(USE_RDP),$(wildcard $(REPO)/platform/n64/render/*.c))
PLATFORM_SRC := $(patsubst $(REPO)/%,%,$(PLATFORM_SRC))
GEN_SRC := $(wildcard $(GEN)/gen_*.c)

OBJS := $(addprefix $(BUILD_DIR)/,$(RUNTIME_SRC:.c=.o) $(PLATFORM_SRC:.c=.o)) \
        $(patsubst $(GEN)/%.c,$(BUILD_DIR)/gen/%.o,$(GEN_SRC))

# TLB=1: guest memory mapped through the TLB (platform/n64/tlb.c)
TLB ?= 1
ifeq ($(TLB),1)
MODE_FLAGS := -DGBA_TLB
endif

CFLAGS += $(MODE_FLAGS) -I$(REPO)/runtime -I$(REPO)/platform/n64 -I$(REPO)/platform/n64/render -I$(GEN) \
          -fno-strict-aliasing -Wno-error=format

# Generated code: no -Werror, no pattern-initialised locals (they would cost
# a store per local), and strict aliasing off like the runtime.
GEN_CFLAGS = $(filter-out -O2 -Werror -ftrivial-auto-var-init=pattern,$(N64_CFLAGS)) \
             -O2 -fno-strict-aliasing -w -MMD $(MODE_FLAGS) \
             -I$(REPO)/runtime -I$(GEN)

$(BUILD_DIR)/gen/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	@echo "    [GEN] $<"
	$(N64_CC) -c $(GEN_CFLAGS) -o $@ $<

all: dbz.z64

$(N64_MKDFS_ROOT)/rom.bin: $(BASEROM) $(REPO)/tools/mkrom.py
	@mkdir -p $(dir $@)
	python3 $(REPO)/tools/mkrom.py $(BASEROM) $@

$(BUILD_DIR)/dbz.dfs: $(N64_MKDFS_ROOT)/rom.bin $(wildcard $(N64_MKDFS_ROOT)/*.txt)
$(BUILD_DIR)/dbz.elf: $(OBJS)

dbz.z64: N64_ROM_TITLE = "DBZ LEGACY GOKU2"
dbz.z64: N64_ROM_SAVETYPE = sram256k
dbz.z64: N64_ROM_EXPANSIONPAK = required
dbz.z64: $(BUILD_DIR)/dbz.dfs

clean:
	rm -rf $(BUILD_DIR)/runtime $(BUILD_DIR)/platform $(BUILD_DIR)/gen $(BUILD_DIR)/dbz.* dbz.z64

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

.PHONY: all clean
