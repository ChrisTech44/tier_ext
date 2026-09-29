# SPDX-License-Identifier: GPL-2.0
#
# Builds the tier_ext loader and example policies against the tier_ext kernel
# in ../linux (which must already be built: vmlinux provides the BTF).
#
#   make            # build/tier_ext_load and build/*.bpf.o
#   make clean

LINUX     ?= $(abspath ../linux)
BUILD     := build
CLANG     ?= clang
BPFTOOL   ?= bpftool
CC        ?= gcc

LIBBPF_SRC := $(LINUX)/tools/lib/bpf
LIBBPF_OUT := $(abspath $(BUILD)/libbpf)
LIBBPF_A   := $(LIBBPF_OUT)/libbpf.a
LIBBPF_INC := $(LIBBPF_OUT)/include

POLICIES := $(patsubst %.bpf.c,%,$(wildcard *.bpf.c))
BPF_OBJS := $(addprefix $(BUILD)/,$(addsuffix .bpf.o,$(POLICIES)))

all: $(BUILD)/tier_ext_load $(BUILD)/te_resident $(BUILD)/te_touch $(BPF_OBJS)

$(BUILD):
	mkdir -p $@

$(LIBBPF_A): | $(BUILD)
	mkdir -p $(LIBBPF_OUT)
	$(MAKE) -C $(LIBBPF_SRC) OUTPUT=$(LIBBPF_OUT)/ $(LIBBPF_A)
	$(MAKE) -C $(LIBBPF_SRC) OUTPUT=$(LIBBPF_OUT)/ DESTDIR=$(LIBBPF_OUT) \
		prefix= install_headers

$(BUILD)/vmlinux.h: $(LINUX)/vmlinux | $(BUILD)
	$(BPFTOOL) btf dump file $< format c > $@

$(BUILD)/%.bpf.o: %.bpf.c tier_ext.bpf.h te_engine.bpf.h $(BUILD)/vmlinux.h $(LIBBPF_A)
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I$(BUILD) -I$(LIBBPF_INC) \
		-Wall -Wno-missing-declarations -c $< -o $@

$(BUILD)/tier_ext_load: tier_ext_load.c $(LIBBPF_A)
	$(CC) -g -O2 -Wall -I$(LIBBPF_INC) $< $(LIBBPF_A) -lelf -lz -static-libgcc -o $@

# guest-side test helper (static, so it runs in any VM image)
$(BUILD)/te_resident: ../tests/guest/te_resident.c | $(BUILD)
	$(CC) -O2 -Wall -static $< -o $@

$(BUILD)/te_touch: ../bench/guest/te_touch.c | $(BUILD)
	$(CC) -O2 -Wall -static $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
.DELETE_ON_ERROR:
