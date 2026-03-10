# Build system for Rhoudveine OS Programs
# This Makefile compiles all user-space programs as truly native Rhoudveine binaries
# leveraging the kernel's userlib/libc.h

# Compiler configuration
CC = x86_64-elf-gcc
GCC_INC = /home/linuxbrew/.linuxbrew/Cellar/x86_64-elf-gcc/15.2.0/lib/gcc/x86_64-elf/15.2.0/include

# Native Rhoudveine flags: no standard includes, use kernel userlib
CFLAGS = -Wall -Wextra -O2 -ffreestanding -nostdinc \
         -isystem $(GCC_INC) -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
         -I ../../../kernel/userlib

# Truly native: no standard libraries, use kernel userlib
LDFLAGS = -T linker.ld -static -nostdlib

# Paths
ROOTFS_DIR ?= ../../../rootfs
BUILD_DIR ?= build
BOOTER_DIR = $(ROOTFS_DIR)/System/Rhoudveine/Booter
PROGRAMS_DIR = $(ROOTFS_DIR)/System/Rhoudveine/Programs

# All binaries
ALL_BINARIES := graviton clear reset cdl cdir echo acpi shutdown reboot rash safemode sys

# Default target
.PHONY: all install clean
all: $(BUILD_DIR) $(addprefix $(BUILD_DIR)/, $(ALL_BINARIES)) install

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/crt0.o: crt0.s
	@x86_64-elf-gcc -c crt0.s -o $(BUILD_DIR)/crt0.o

# Generic rule for coreutils
$(BUILD_DIR)/%: coreutils/%.c $(BUILD_DIR)/crt0.o
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -o $@ $(BUILD_DIR)/crt0.o $< $(LDFLAGS)

# Specific rule for graviton
$(BUILD_DIR)/graviton: graviton/graviton.c $(BUILD_DIR)/crt0.o
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -o $@ $(BUILD_DIR)/crt0.o $< $(LDFLAGS)

# Installation target - copy binaries to rootfs
install:
	@echo "Installing binaries to rootfs..."
	@mkdir -p $(BOOTER_DIR)
	@mkdir -p $(PROGRAMS_DIR)
	@cp $(BUILD_DIR)/graviton $(BOOTER_DIR)/init
	@echo "  ✓ GRAVITON installed as /System/Rhoudveine/Booter/init"
	@for b in $(filter-out graviton,$(ALL_BINARIES)); do \
		cp $(BUILD_DIR)/$$b $(PROGRAMS_DIR)/; \
	done
	@echo "  ✓ COREUTILS programs installed"
	@echo "All programs installed to rootfs!"

clean:
	@rm -rf $(BUILD_DIR)
	@echo "Build artifacts cleaned"
