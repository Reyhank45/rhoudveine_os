# Root Makefile for Rhoudveine OS

OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC := /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS := /tmp/rhoudveine_OVMF_VARS.fd
ISO := ISO/rhoudveine.iso

.PHONY: all kernel system build-x86_64 clean run run-bios

all: kernel system build-x86_64

kernel:
	$(MAKE) -C kernel

system:
	$(MAKE) -C System

build-x86_64: kernel system
	$(MAKE) -C kernel build-x86_64

$(ISO): build-x86_64

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C System clean
	rm -rf ISO

# ── QEMU UEFI (OVMF) ────────────────────────────────────────────────────
run: $(ISO)
	@cp -f $(OVMF_VARS_SRC) $(OVMF_VARS)
	qemu-system-x86_64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(OVMF_VARS) \
	    -cdrom $(ISO) \
	    -m 512M \
	    -serial stdio \
	    -vga std \
	    -no-reboot

# ── QEMU Legacy BIOS ────────────────────────────────────────────────────
run-bios: $(ISO)
	qemu-system-x86_64 \
	    -cdrom $(ISO) \
	    -m 512M \
	    -serial stdio \
	    -vga std \
	    -no-reboot

# ── QEMU Laptop Emulation (Matches user hardware) ────────────────────────
run-host: $(ISO)
	@cp -f $(OVMF_VARS_SRC) $(OVMF_VARS)
	qemu-system-x86_64 \
	    -enable-kvm \
	    -cpu host \
	    -machine q35 \
	    -smp 4 \
	    -m 4G \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(OVMF_VARS) \
	    -cdrom $(ISO) \
	    -serial stdio \
	    -vga std \
	    -device qemu-xhci \
	    -no-reboot
