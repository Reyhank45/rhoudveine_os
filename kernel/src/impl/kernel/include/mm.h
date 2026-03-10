#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include <stddef.h>
#include <stdint.h>

// Initialize the physical memory manager.
void mm_init(void);

// Maps a region of physical memory into the kernel's MMIO virtual address
// space. Returns the virtual address.
void *mmio_remap(uint64_t physical_addr, size_t size);

// Allocate a physical frame (4KB page). Returns physical address.
uint64_t pfa_alloc(void);

// Allocate a physical frame from low memory (<4GB, identity mapped)
uint64_t pfa_alloc_low(void);

// Free a physical frame
void pfa_free(uint64_t paddr);

// Convert physical address to virtual address
void *phys_to_virt(uint64_t paddr);

// Convert virtual address to physical address
uint64_t virt_to_phys(void *vaddr);

// Virtual Memory Manager flags
#define PAGE_PRESENT (1ULL << 0)
#define PAGE_RW (1ULL << 1)
#define PAGE_USER (1ULL << 2)
#define PAGE_PWT (1ULL << 3)
#define PAGE_PCD (1ULL << 4)
#define PAGE_NO_EXEC (1ULL << 63)

// Map a physical page to a virtual address.
void map_page(uint64_t phys_addr, uint64_t virt_addr, uint64_t flags);

#endif // KERNEL_MM_H