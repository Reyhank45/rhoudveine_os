#include "include/mm.h"
#include "include/stdio.h"
#include <stddef.h>
#include <stdint.h>

#include "include/limine.h"

// Forward declaration for kprintf
extern void kprintf(const char *format, uint32_t color, ...);
extern struct limine_memmap_request memmap_request;
extern uint64_t hhdm_offset;
#define DIRECT_MAP_OFFSET hhdm_offset

// Symbol from the linker script, marks the end of the kernel image.
extern uint8_t kernel_end[];

#define PAGE_SIZE 4096

void *phys_to_virt(uint64_t paddr) { return (void *)(paddr + hhdm_offset); }

// Custom memset
static void *custom_memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  while (n-- > 0)
    *p++ = (unsigned char)c;
  return s;
}

// ---------------------------------------------------------------------------
// Physical Frame Allocator (PFA) — Bitmap Implementation
// ---------------------------------------------------------------------------

#define MAX_PAGES (1024ULL * 1024 * 2) // Support up to 8GB of RAM
#define BITMAP_SIZE (MAX_PAGES / 8)    // 256KB per bitmap

// Main bitmap — high memory (>=4GB)
static uint8_t *bitmap = NULL;
static uint64_t bitmap_base = 0;
static uint64_t free_pages_count = 0;
static uint64_t total_pages_count = 0;

// Low bitmap — low memory (<4GB, identity mapped)
static uint8_t *low_bitmap = NULL;
static uint64_t low_free_count = 0;
static uint64_t low_total_count = 0;

// ---------------------------------------------------------------------------
// Bitmap helper functions
// ---------------------------------------------------------------------------

static inline uint64_t addr_to_index(uint64_t paddr) {
  return (paddr - bitmap_base) / PAGE_SIZE;
}

static inline void bitmap_set_used(uint8_t *bmap, uint64_t index) {
  bmap[index / 8] |= (uint8_t)(1 << (index % 8));
}

static inline void bitmap_set_free(uint8_t *bmap, uint64_t index) {
  bmap[index / 8] &= (uint8_t)~(1 << (index % 8));
}

static inline int bitmap_is_free(uint8_t *bmap, uint64_t index) {
  return !(bmap[index / 8] & (uint8_t)(1 << (index % 8)));
}

// ---------------------------------------------------------------------------
// PFA public API
// ---------------------------------------------------------------------------

void pfa_free(uint64_t paddr) {
  if (paddr == 0)
    return;

  if (paddr < 0x100000000ULL) {
    uint64_t index = paddr / PAGE_SIZE;
    if (index >= MAX_PAGES)
      return;
    if (!bitmap_is_free(low_bitmap, index)) {
      bitmap_set_free(low_bitmap, index);
      low_free_count++;
      low_total_count++;
    }
    return;
  }

  if (paddr < bitmap_base)
    return;
  uint64_t index = addr_to_index(paddr);
  if (index >= MAX_PAGES)
    return;

  if (!bitmap_is_free(bitmap, index)) {
    bitmap_set_free(bitmap, index);
    free_pages_count++;
    total_pages_count++;
  }
}

uint64_t pfa_alloc_low(void) {
  for (uint64_t byte = 0; byte < BITMAP_SIZE; byte++) {
    if (low_bitmap[byte] == 0xFF)
      continue;

    for (uint64_t bit = 0; bit < 8; bit++) {
      uint64_t index = byte * 8 + bit;
      if (index >= MAX_PAGES)
        break;

      if (bitmap_is_free(low_bitmap, index)) {
        bitmap_set_used(low_bitmap, index);
        low_free_count--;
        return index * PAGE_SIZE;
      }
    }
  }

  kprintf("MM: pfa_alloc_low() out of low memory!\n", 0xFF0000);
  return 0;
}

uint64_t pfa_alloc(void) {
  for (uint64_t byte = 0; byte < BITMAP_SIZE; byte++) {
    if (bitmap[byte] == 0xFF)
      continue;

    for (uint64_t bit = 0; bit < 8; bit++) {
      uint64_t index = byte * 8 + bit;
      if (index >= MAX_PAGES)
        break;

      if (bitmap_is_free(bitmap, index)) {
        bitmap_set_used(bitmap, index);
        free_pages_count--;
        return bitmap_base + (index * PAGE_SIZE);
      }
    }
  }

  kprintf("MM: pfa_alloc() out of memory!\n", 0xFF0000);
  return 0;
}

// ---------------------------------------------------------------------------
// Memory statistics
// ---------------------------------------------------------------------------

uint64_t mm_get_total_memory(void) {
  return (total_pages_count + low_total_count) * PAGE_SIZE;
}

uint64_t mm_get_free_memory(void) {
  return (free_pages_count + low_free_count) * PAGE_SIZE;
}

// ---------------------------------------------------------------------------
// Virtual Memory Manager (VMM)
// ---------------------------------------------------------------------------

// MMIO virtual address region. Initialized in mm_init.
static uint64_t next_mmio_addr;

// ---------------------------------------------------------------------------
// map_page — map a single 4KB physical page to a virtual address with flags.
// All intermediate page table pages are allocated from low memory.
// ---------------------------------------------------------------------------
void map_page(uint64_t phys_addr, uint64_t virt_addr, uint64_t flags) {
  uint64_t pml4_index = (virt_addr >> 39) & 0x1FF;
  uint64_t pdpt_index = (virt_addr >> 30) & 0x1FF;
  uint64_t pdt_index = (virt_addr >> 21) & 0x1FF;
  uint64_t pt_index = (virt_addr >> 12) & 0x1FF;

  // Propagate caching bits into intermediate levels so the effective
  // memory type of the final PTE is not overridden by parent entries.
  uint64_t caching_flags = flags & (PAGE_PWT | PAGE_PCD);

  const uint64_t PADDR_MASK = 0x000FFFFFFFFFF000ULL;

  uint64_t pml4_phys;
  asm volatile("mov %%cr3, %0" : "=r"(pml4_phys));
  uint64_t *pml4_virt = phys_to_virt(pml4_phys & PADDR_MASK);

  // PML4 -> PDPT
  if (!(pml4_virt[pml4_index] & PAGE_PRESENT)) {
    uint64_t pdpt_phys = pfa_alloc_low();
    if (!pdpt_phys) {
      kprintf("MM: map_page OOM (PDPT) virt=0x%lx\n", 0xFF0000, virt_addr);
      return;
    }
    custom_memset(phys_to_virt(pdpt_phys), 0, PAGE_SIZE);
    pml4_virt[pml4_index] = pdpt_phys | PAGE_PRESENT | PAGE_RW |
                            (flags & PAGE_USER) | caching_flags;
  } else {
    pml4_virt[pml4_index] |= (flags & PAGE_USER) | caching_flags;
  }
  uint64_t *pdpt_virt = phys_to_virt(pml4_virt[pml4_index] & PADDR_MASK);

  // PDPT -> PDT
  if (!(pdpt_virt[pdpt_index] & PAGE_PRESENT)) {
    uint64_t pdt_phys = pfa_alloc_low();
    if (!pdt_phys) {
      kprintf("MM: map_page OOM (PDT) virt=0x%lx\n", 0xFF0000, virt_addr);
      return;
    }
    custom_memset(phys_to_virt(pdt_phys), 0, PAGE_SIZE);
    pdpt_virt[pdpt_index] =
        pdt_phys | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER) | caching_flags;
  } else {
    pdpt_virt[pdpt_index] |= (flags & PAGE_USER) | caching_flags;
  }
  uint64_t *pdt_virt = phys_to_virt(pdpt_virt[pdpt_index] & PADDR_MASK);

  // PDT -> PT
  if (!(pdt_virt[pdt_index] & PAGE_PRESENT)) {
    uint64_t pt_phys = pfa_alloc_low();
    if (!pt_phys) {
      kprintf("MM: map_page OOM (PT) virt=0x%lx\n", 0xFF0000, virt_addr);
      return;
    }
    custom_memset(phys_to_virt(pt_phys), 0, PAGE_SIZE);
    pdt_virt[pdt_index] =
        pt_phys | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER) | caching_flags;
  } else {
    pdt_virt[pdt_index] |= (flags & PAGE_USER) | caching_flags;
  }
  uint64_t *pt_virt = phys_to_virt(pdt_virt[pdt_index] & PADDR_MASK);

  // Map the leaf page.
  pt_virt[pt_index] = phys_addr | flags;

  asm volatile("mfence" ::: "memory");
  asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

// ---------------------------------------------------------------------------
// mmio_map_low — remap a low (<4GB) physical MMIO region with UC flags.
// ---------------------------------------------------------------------------
static void *mmio_map_low(uint64_t physical_addr, size_t size) {
  uint64_t phys_base = physical_addr & ~0xFFFULL;
  uint64_t offset = physical_addr & 0xFFFULL;

  size_t pages_needed = (size + offset + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages_needed == 0)
    pages_needed = 1;
  if (pages_needed > 64)
    pages_needed = 64; // safety cap

  uint64_t virt_base = next_mmio_addr;

  kprintf("MM: Mapping low MMIO phys=0x%lx -> virt=0x%lx (%u pages, UC)\n",
          0x00FF0000, physical_addr, virt_base, (uint32_t)pages_needed);

  for (size_t i = 0; i < pages_needed; i++) {
    map_page(phys_base + i * PAGE_SIZE, virt_base + i * PAGE_SIZE,
             PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT | PAGE_NO_EXEC);
  }

  next_mmio_addr += pages_needed * PAGE_SIZE;

  return (void *)(virt_base + offset);
}

// ---------------------------------------------------------------------------
// mm_init — entry point, called once at kernel startup
// ---------------------------------------------------------------------------

void mm_init(void) {
  kprintf("MM: Initializing memory manager...\n", 0x00FF0000);

  if (memmap_request.response == NULL) {
    kprintf("MM: FATAL - Limine memory map not found!\n", 0xFF0000);
    return;
  }

  uint64_t num_entries = memmap_request.response->entry_count;
  kprintf("MM: mmap entries = %lu\n", 0x00FF0000, num_entries);

  uint64_t max_phys_addr = 0;
  for (uint64_t i = 0; i < num_entries; i++) {
    struct limine_memmap_entry *entry = memmap_request.response->entries[i];
    if (entry->type == LIMINE_MEMMAP_USABLE ||
        entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
      uint64_t end = entry->base + entry->length;
      if (end > max_phys_addr)
        max_phys_addr = end;
    }
  }
  kprintf("MM: Highest physical address : 0x%lx\n", 0x00FF0000, max_phys_addr);

  bitmap_base = 0x100000000ULL;

  uint64_t req_pages = 32;
  uint64_t req_size = req_pages * PAGE_SIZE;

  kprintf("MM: Main bitmap base         : 0x%lx\n", 0x00FF0000, bitmap_base);
  kprintf("MM: low_bitmap  : %lu KB (%lu pages)\n", 0x00FF0000,
          BITMAP_SIZE / 1024, BITMAP_SIZE / PAGE_SIZE);
  kprintf("MM: Need %lu KB (%lu pages) for bitmaps\n", 0x00FF0000,
          req_size / 1024, req_pages);

  uint64_t bitmap_placement_addr = 0;
  for (uint64_t i = 0; i < num_entries; i++) {
    struct limine_memmap_entry *entry = memmap_request.response->entries[i];
    if (entry->type != LIMINE_MEMMAP_USABLE &&
        entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
      continue;

    uint64_t region_start =
        (entry->base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t region_end =
        (entry->base + entry->length) & ~(uint64_t)(PAGE_SIZE - 1);

    if (region_end > region_start && (region_end - region_start) >= req_size) {
      if (!bitmap_placement_addr)
        bitmap_placement_addr = region_start;
    }
  }

  if (!bitmap_placement_addr) {
    kprintf("MM: FATAL - No contiguous region found for bitmaps!\n", 0xFF0000);
    return;
  }

  low_bitmap = (uint8_t *)(bitmap_placement_addr + DIRECT_MAP_OFFSET);
  bitmap = (uint8_t *)(bitmap_placement_addr + BITMAP_SIZE + DIRECT_MAP_OFFSET);

  kprintf("MM: Placing low_bitmap at 0x%lx, main bitmap at 0x%lx\n", 0x00FF0000,
          bitmap_placement_addr, bitmap_placement_addr + BITMAP_SIZE);
  kprintf("MM: Bitmap occupies physical 0x%lx - 0x%lx\n", 0x00FF0000,
          bitmap_placement_addr, bitmap_placement_addr + req_size);

  custom_memset(bitmap, 0xFF, BITMAP_SIZE);
  custom_memset(low_bitmap, 0xFF, BITMAP_SIZE);

  struct {
    uint64_t start;
    uint64_t end;
  } reserved_ranges[10];
  int reserved_count = 0;

  reserved_ranges[0].start = bitmap_placement_addr;
  reserved_ranges[0].end = bitmap_placement_addr + req_size;
  reserved_count++;

  for (uint64_t i = 0; i < num_entries; i++) {
    struct limine_memmap_entry *entry = memmap_request.response->entries[i];

    if (entry->type != LIMINE_MEMMAP_USABLE &&
        entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
      continue;

    kprintf("MM: Usable RAM at 0x%lx, size 0x%lx\n", 0x00FF0000, entry->base,
            entry->length);

    uint64_t first_page =
        (entry->base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last_page =
        (entry->base + entry->length) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t p = last_page; p > first_page;) {
      p -= PAGE_SIZE;
      if (p == 0)
        continue;

      int is_reserved = 0;
      for (int r = 0; r < reserved_count; r++) {
        if (p < reserved_ranges[r].end &&
            (p + PAGE_SIZE) > reserved_ranges[r].start) {
          is_reserved = 1;
          break;
        }
      }
      if (is_reserved)
        continue;

      pfa_free(p);
    }
  }

  kprintf("MM: PFA initialized.\n", 0x00FF0000);
  kprintf("MM: Free low  memory : %lu MB\n", 0x00FF0000,
          (low_free_count * PAGE_SIZE) / (1024 * 1024));
  kprintf("MM: Free high memory : %lu MB\n", 0x00FF0000,
          (free_pages_count * PAGE_SIZE) / (1024 * 1024));
  kprintf("MM: Total usable     : %lu MB\n", 0x00FF0000,
          mm_get_total_memory() / (1024 * 1024));
  kprintf("MM: Total free       : %lu MB\n", 0x00FF0000,
          mm_get_free_memory() / (1024 * 1024));

  // Reserve MMIO virtual address region high in kernel space.
  next_mmio_addr = 0xFFFFFFFF80000000ULL;
  kprintf("MM: MMIO mapping region starts at 0x%lx\n", 0x00FF0000,
          next_mmio_addr);
}

// ---------------------------------------------------------------------------
// mmio_remap — map a physical MMIO region into virtual address space
// ---------------------------------------------------------------------------
void *mmio_remap(uint64_t physical_addr, size_t size) {
  uint64_t phys_base = physical_addr & ~0xFFFULL;
  uint64_t offset = physical_addr & 0xFFFULL;

  size_t pages_needed = (size + offset + PAGE_SIZE - 1) / PAGE_SIZE;
  if (pages_needed == 0)
    pages_needed = 1;
  if (pages_needed > 64)
    pages_needed = 64;

  uint64_t virt_base = next_mmio_addr;

  kprintf("MM: mmio_remap phys=0x%lx size=%u -> virt=0x%lx (%u pages, UC)\n",
          0x00FF0000, physical_addr, (uint32_t)size, virt_base,
          (uint32_t)pages_needed);

  for (size_t i = 0; i < pages_needed; i++) {
    map_page(phys_base + i * PAGE_SIZE, virt_base + i * PAGE_SIZE,
             PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT | PAGE_NO_EXEC);
  }

  next_mmio_addr += pages_needed * PAGE_SIZE;

  kprintf("MM: mmio_remap done, virt=0x%lx\n", 0x00FF0000, virt_base + offset);
  return (void *)(virt_base + offset);
}

// ---------------------------------------------------------------------------
// virt_to_phys — translate a kernel virtual address to physical
// ---------------------------------------------------------------------------

uint64_t virt_to_phys(void *vaddr) {
  uint64_t addr = (uint64_t)vaddr;

  if (addr >= DIRECT_MAP_OFFSET)
    return addr - DIRECT_MAP_OFFSET;

  return addr;
}