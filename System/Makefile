# System Programs Makefile

CC := /home/linuxbrew/.linuxbrew/Cellar/x86_64-elf-gcc/15.2.0/bin/x86_64-elf-gcc
AS := nasm
LD := /home/linuxbrew/.linuxbrew/Cellar/x86_64-elf-binutils/2.46.0/bin/x86_64-elf-ld

# Flags for user-mode
CFLAGS := -I userlib -I ../kernel/src/impl/kernel -I ../kernel/src/impl/kernel/fs -I ../kernel/src/impl/kernel/include -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -O0 -mno-red-zone -fPIE -mcmodel=large
ASFLAGS := -f elf64

# Output directory for binaries
DIST_DIR := ../kernel/dist/x86_64

# init files
INIT_SRCS := init/init.c
INIT_OBJS := $(patsubst %.c, build/%.o, $(INIT_SRCS))
CRT0_OBJ := build/userlib/crt0.o

.PHONY: all clean

all: $(DIST_DIR)/init

# Link init as a standalone ELF
$(DIST_DIR)/init: $(CRT0_OBJ) $(INIT_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(LD) -o $@ -Ttext 0x2000000 --static $(CRT0_OBJ) $(INIT_OBJS)

# Compile C source
build/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $@"
	@$(CC) -c $(CFLAGS) $< -o $@

# Compile Assembly source (crt0)
build/userlib/%.o: userlib/%.asm
	@mkdir -p $(dir $@)
	@echo "  NASM    $@"
	@$(AS) $(ASFLAGS) $< -o $@

clean:
	@echo "  CLEAN   System"
	@rm -rf build
