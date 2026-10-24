# Test ROM for platform/n64/render.  Run from the build directory so nothing is
# written into the repository:
#   make -C /mnt/d/dbz-n64/build/n64_render -f <repo>/tests/n64_render/Makefile
# (tests/n64_render/build.sh does this and prepares the DFS contents in $(BUILD_DIR)/fs).
REPO      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILD_DIR ?= /mnt/d/dbz-n64/build/n64_render
SOURCE_DIR := $(REPO)
ROMNAME   := rtest
LIBDRAGON_PREVIEW = 2
include $(N64_INST)/include/n64.mk

N64_MKDFS_ROOT := $(BUILD_DIR)/fs
SRCS := tests/n64_render/main.c tests/n64_render/stubs.c tests/n64_render/rdpbench.c \
        $(patsubst $(REPO)/%,%,$(wildcard $(REPO)/platform/n64/render/*.c))
OBJS := $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

CFLAGS += -I$(REPO)/runtime -I$(REPO)/platform/n64/render -DGBA_RENDER_HWCHECK $(EXTRA_CFLAGS)
N64_C_AND_CXX_FLAGS := $(filter-out -O2 -ftrivial-auto-var-init=pattern,$(N64_C_AND_CXX_FLAGS)) -O2

all: $(ROMNAME).z64

$(BUILD_DIR)/$(ROMNAME).dfs: $(wildcard $(BUILD_DIR)/fs/*)
$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)

$(ROMNAME).z64: N64_ROM_TITLE = "GBA RENDER TEST"
$(ROMNAME).z64: N64_ROM_EXPANSIONPAK = required
$(ROMNAME).z64: $(BUILD_DIR)/$(ROMNAME).dfs

clean:
	rm -rf $(BUILD_DIR)/tests $(BUILD_DIR)/platform $(BUILD_DIR)/$(ROMNAME).* $(ROMNAME).z64
.PHONY: all clean

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
