.PHONY: all clean menuconfig build-x86_64 programs rootfs-tar help

# Paths
SYSTEM_PROGS_DIR := System/Rhoudveine/Programs
ROOTFS_DIR := rootfs
TAR_OUTPUT := rootfs.tar

# Default target - build kernel and programs
all: build-x86_64

# Kernel build targets - depends on rootfs.tar being created first
build-x86_64: rootfs-tar
	$(MAKE) -C kernel build-x86_64

# Build programs using Programs Makefile
programs:
	@echo "=========================================="
	@echo "[1/2] Building programs..."
	@echo "=========================================="
	$(MAKE) -C $(SYSTEM_PROGS_DIR) ROOTFS_DIR=../../../$(ROOTFS_DIR) all
	@echo ""

# Create rootfs.tar archive
rootfs-tar: programs
	@echo "=========================================="
	@echo "[2/2] Creating rootfs.tar..."
	@echo "=========================================="
	@if [ ! -d "$(ROOTFS_DIR)/System/Rhoudveine/Programs" ]; then \
		echo "Error: rootfs structure incomplete"; \
		exit 1; \
	fi
	@PROG_COUNT=$$(ls -1 $(ROOTFS_DIR)/System/Rhoudveine/Programs 2>/dev/null | wc -l); \
	echo "  ✓ Verified $$PROG_COUNT programs in rootfs"; \
	echo ""
	@echo "Creating tar archive..."
	@(cd $(ROOTFS_DIR) && tar --owner=0 --group=0 -cf ../$(TAR_OUTPUT) .)
	@if [ -f "$(TAR_OUTPUT)" ]; then \
		SIZE=$$(du -h $(TAR_OUTPUT) | cut -f1); \
		echo "  ✓ $(TAR_OUTPUT) created ($$SIZE)"; \
	else \
		echo "ERROR: $(TAR_OUTPUT) was not created"; \
		exit 1; \
	fi
	@echo ""
	@echo "=========================================="
	@echo "Build complete!"
	@echo "=========================================="
	@echo ""
	@echo "Output files:"
	@echo "  - Programs: $(ROOTFS_DIR)/System/Rhoudveine/Programs/"
	@echo "  - Archive:  $(TAR_OUTPUT)"
	@echo ""

# Clean all build artifacts
clean:
	@echo "Cleaning build artifacts..."
	$(MAKE) -C kernel clean
	$(MAKE) -C $(SYSTEM_PROGS_DIR) clean
	@rm -f $(TAR_OUTPUT)
	@echo "Clean complete"

# Menu configuration
menuconfig:
	$(MAKE) -C kernel menuconfig

# Help target
help:
	@echo "Rhoudveine OS Master Makefile"
	@echo "=============================="
	@echo ""
	@echo "Kernel Targets:"
	@echo "  build-x86_64   - Build kernel for x86_64"
	@echo "  menuconfig     - Configure kernel options"
	@echo ""
	@echo "Programs & Rootfs Targets:"
	@echo "  programs       - Build all user-space programs"
	@echo "  rootfs-tar     - Create rootfs.tar archive"
	@echo ""
	@echo "General Targets:"
	@echo "  all            - Build kernel, programs, and rootfs.tar (default)"
	@echo "  clean          - Remove all build artifacts"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make                # Build everything"
	@echo "  make programs       # Build programs only"
	@echo "  make rootfs-tar     # Create rootfs.tar"
	@echo "  make build-x86_64   # Build kernel only"
	@echo "  make clean          # Clean all artifacts"
	@echo ""
