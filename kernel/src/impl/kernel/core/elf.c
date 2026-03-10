//
// elf.c — ELF64 loader for Rhoudveine init process
//
// Fixes vs original:
//
//  BUG 1 — SMAP crash during copy
//    Original mapped pages with PAGE_USER then immediately wrote to them from
//    kernel mode.  SMAP (Supervisor Mode Access Prevention, CR4.SMAP bit 21)
//    raises #PF whenever ring-0 code touches a U/S=1 page without STAC.
//    Fix: bracket the copy with STAC/CLAC (temporarily allow kernel→user
//    access)
//         OR map kernel-only first, copy, then remap with PAGE_USER.
//    We use the STAC/CLAC approach — simpler and avoids double page-table
//    walks. If the CPU doesn't support SMAP the instructions are NOPs (via
//    CPUID check).
//
//  BUG 2 — SMEP crash at entry jump
//    Jumping to a user-space virtual address (e.g. 0x400000) from CPL 0
//    triggers SMEP (#PF, CR4 bit 20).  A proper OS uses IRETQ/SYSRET to drop to
//    ring 3 before reaching user code.  Until an IDT + syscall handler exists,
//    we clear SMEP and SMAP in CR4 before the jump.
//
//  BUG 3 — No init stack
//    The kernel stack (RSP) must not be used by init: it may be overwritten by
//    kernel interrupt handlers, and its address leaks kernel layout to
//    userspace. Fix: allocate INIT_STACK_PAGES fresh pages, map them, set RSP.
//
//  BUG 4 — Program header pointer arithmetic
//    Original did "ph++" which assumes e_phentsize == sizeof(elf64_phdr).
//    The ELF spec allows e_phentsize to be larger.  Use byte offsets.
//

#include "include/elf.h"
#include "include/console.h"
#include "include/mm.h"
#include "include/stdio.h"
#include <stdint.h>

// ── ELF64 header/phdr ────────────────────────────────────────────────────────

struct elf64_hdr {
  unsigned char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct elf64_phdr {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define PT_LOAD 1
#define PF_X 0x1 // segment executable flag
#define PF_W 0x2 // segment writable flag
#define PF_R 0x4 // segment readable flag

// Init stack: 16 KB just below 0x70000000000
#define INIT_STACK_PAGES 4
#define INIT_STACK_TOP 0x0000700000000000ULL

// ── Minimal string/memory utils (no libc) ────────────────────────────────────

static void elf_memcpy(void *dst, const void *src, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  while (n--)
    *d++ = *s++;
}
static void elf_memset(void *dst, uint8_t c, uint64_t n) {
  uint8_t *d = (uint8_t *)dst;
  while (n--)
    *d++ = c;
}

// ── CPUID + CR4 helpers
// ───────────────────────────────────────────────────────

// Returns 1 if SMAP is supported (CPUID leaf 7, EBX bit 20)
static int cpu_has_smap(void) {
  uint32_t ebx = 0;
  __asm__ volatile("mov $7, %%eax\n"
                   "xor %%ecx, %%ecx\n"
                   "cpuid\n"
                   : "=b"(ebx)
                   :
                   : "eax", "ecx", "edx");
  return (ebx >> 20) & 1;
}

static inline uint64_t read_cr4(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr4, %0" : "=r"(v));
  return v;
}
static inline void write_cr4(uint64_t v) {
  __asm__ volatile("mov %0, %%cr4" ::"r"(v) : "memory");
}

// ── elf64_load_and_run
// ────────────────────────────────────────────────────────

int elf64_load_and_run(void *image, uint32_t size,
                       void (*print_fn)(const char *)) {

  if (!image || size < sizeof(struct elf64_hdr)) {
    fb_puts("ELF: Invalid image pointer or size too small\n");
    return -1;
  }

  uint8_t *data = (uint8_t *)image;
  struct elf64_hdr *eh = (struct elf64_hdr *)data;

  // Magic
  if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
      eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
    fb_puts("ELF: Invalid magic signature\n");
    return -2;
  }
  if (eh->e_ident[4] != 2) { // EI_CLASS != ELFCLASS64
    fb_puts("ELF: Not 64-bit ELF\n");
    return -3;
  }

  // ── Check CPU SMAP/SMEP support upfront ──────────────────────────────────
  int has_smap = cpu_has_smap();

  // ── Load PT_LOAD segments ─────────────────────────────────────────────────
  for (int i = 0; i < (int)eh->e_phnum; i++) {
    // FIX 4: byte-offset arithmetic respects variable e_phentsize
    struct elf64_phdr *ph =
        (struct elf64_phdr *)(data + eh->e_phoff +
                              (uint64_t)i * (uint64_t)eh->e_phentsize);

    if (ph->p_type != PT_LOAD)
      continue;

    if (ph->p_offset + ph->p_filesz > (uint64_t)size) {
      fb_puts("ELF: Segment out of bounds\n");
      return -4;
    }

    uint64_t virt_start = ph->p_vaddr;
    uint64_t virt_end = virt_start + ph->p_memsz;
    uint64_t page_start = virt_start & ~0xFFFULL;
    uint64_t page_end = (virt_end + 0xFFFULL) & ~0xFFFULL;

    // Build PTE flags from ELF segment flags.
    // PAGE_USER always set — these are user-space pages.
    // PAGE_NO_EXEC on non-executable segments.
    // We always add PAGE_RW for now (W^X enforcement deferred).
    uint64_t seg_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
    if (!(ph->p_flags & PF_X))
      seg_flags |= PAGE_NO_EXEC;

    fb_puts("ELF: Mapping segment...\n");
    for (uint64_t v = page_start; v < page_end; v += 4096) {
      uint64_t phys = pfa_alloc_low();
      if (!phys) {
        fb_puts("ELF: OOM during segment mapping\n");
        return -5;
      }
      // FIX 1 (part A): map with PAGE_USER | PAGE_RW.
      // The copy below uses STAC to allow kernel access to U/S pages.
      map_page(phys, v, seg_flags);
      kprintf("ELF: Map v=0x%lx to p=0x%lx flags=0x%lx\n", 0x00FFFF00, v, phys,
              seg_flags);
    }

    // FIX 1 (part B): STAC — Supervisor Temporary Allow user-space Access.
    // Sets AC flag in RFLAGS, suppressing SMAP for this block.
    // Safe on CPUs without SMAP (STAC is a NOP there per Intel SDM).
    fb_puts("ELF: Loading segment data...\n");
    kprintf("ELF: Copying 0x%lx bytes from offset 0x%lx to v=0x%lx\n",
            0x00FFFF00, (uint64_t)ph->p_filesz, (uint64_t)ph->p_offset,
            (uint64_t)ph->p_vaddr);
    if (has_smap)
      __asm__ volatile("stac" ::: "memory");
    elf_memcpy((void *)ph->p_vaddr, data + ph->p_offset, ph->p_filesz);

    if (ph->p_memsz > ph->p_filesz) {
      kprintf("ELF: Zeroing 0x%lx bytes of BSS at v=0x%lx\n", 0x00FFFF00,
              (uint64_t)(ph->p_memsz - ph->p_filesz),
              (uint64_t)(ph->p_vaddr + ph->p_filesz));
      elf_memset((void *)(ph->p_vaddr + ph->p_filesz), 0,
                 ph->p_memsz - ph->p_filesz);
    }

    if (has_smap)
      __asm__ volatile("clac" ::: "memory");
  }

  fb_puts("ELF: Segments loaded\n");

  // ── Allocate and map init stack ───────────────────────────────────────────
  // FIX 3: give init its own stack so it doesn't clobber kernel state.
  fb_puts("ELF: Allocating init stack...\n");
  for (int p = 0; p < INIT_STACK_PAGES; p++) {
    uint64_t phys = pfa_alloc_low();
    if (!phys) {
      fb_puts("ELF: OOM for init stack\n");
      return -6;
    }
    // Stack grows down: page 0 is the highest (just below INIT_STACK_TOP)
    uint64_t sv = INIT_STACK_TOP - (uint64_t)(p + 1) * 4096;
    map_page(phys, sv, PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_NO_EXEC);
  }

  // 16-byte aligned (System V AMD64 ABI requirement)
  uint64_t init_rsp = INIT_STACK_TOP;

  // ── FIX 2: Disable SMEP + SMAP before jumping ─────────────────────────────
  // SMEP (CR4 bit 20): prevents CPL 0 from fetching instructions at
  //                    pages with U/S=1 — fires on jmp to 0x400000.
  // SMAP (CR4 bit 21): prevents CPL 0 data access to U/S pages without STAC.
  // Both must be clear for the jmp-to-user-vaddr pattern.
  // A proper ring-3 transition via IRETQ/SYSRET restores them automatically
  // through the privilege-level change; that's the correct long-term fix.
  fb_puts("ELF: Clearing SMEP/SMAP, jumping to init...\n");
  {
    uint64_t cr4 = read_cr4();
    cr4 &= ~((1ULL << 20) | (1ULL << 21)); // SMEP=0, SMAP=0
    write_cr4(cr4);
  }

  __asm__ volatile("mfence" ::: "memory");

  // ── Jump to entry point with init's stack (Ring 3 Transition) ─────────────
  fb_puts("ELF: Dropping to Ring 3...\n");

  uint64_t r_fn = (uint64_t)(uintptr_t)print_fn;
  uint64_t r_rsp = init_rsp;
  uint64_t r_entry = eh->e_entry;

  __asm__ volatile("mov %0, %%rdi\n"    // rdi = print_fn
                   "xor %%rbp, %%rbp\n" // rbp = 0
                   "push $0x1B\n"       // SS (User Data)
                   "push %1\n"          // RSP
                   "push $0x202\n"      // RFLAGS (IF=1, Reserved=1)
                   "push $0x23\n"       // CS (User Code)
                   "push %2\n"          // RIP
                   "iretq\n"
                   :
                   : "r"(r_fn), "r"(r_rsp), "r"(r_entry)
                   : "memory");

  __builtin_unreachable(); // tell GCC this path never continues
}