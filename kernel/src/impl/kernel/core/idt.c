#include "include/idt.h"
#include <stdint.h>

extern void isr_default_handler();
extern void isr_irq0();
extern void isr_irq1();

struct __attribute__((packed)) idt_entry {
  uint16_t offset_lo;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attr;
  uint16_t offset_mid;
  uint32_t offset_hi;
  uint32_t zero;
};

struct __attribute__((packed)) idtr {
  uint16_t limit;
  uint64_t base;
};

// NOT static — idt_exceptions.c needs to reach this symbol
struct idt_entry idt[256];

static void set_idt_entry(int n, void (*handler)()) {
  uint64_t addr = (uint64_t)handler;
  idt[n].offset_lo = addr & 0xFFFF;
  idt[n].selector = 0x08;
  idt[n].ist = 0;
  idt[n].type_attr = 0x8E;
  idt[n].offset_mid = (addr >> 16) & 0xFFFF;
  idt[n].offset_hi = (addr >> 32) & 0xFFFFFFFF;
  idt[n].zero = 0;
}

static inline void lidt_load(void) {
  struct idtr idtr;
  idtr.limit = sizeof(idt) - 1;
  idtr.base = (uint64_t)&idt;
  __asm__ volatile("lidt %0" ::"m"(idtr));
}

// PIC ports
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}

static void pic_remap(void) {
  outb(PIC1_CMD, 0x11);
  outb(PIC2_CMD, 0x11);
  outb(PIC1_DATA, 0x20); // master offset → vector 32
  outb(PIC2_DATA, 0x28); // slave offset  → vector 40
  outb(PIC1_DATA, 0x04);
  outb(PIC2_DATA, 0x02);
  outb(PIC1_DATA, 0x01);
  outb(PIC2_DATA, 0x01);
  // unmask IRQ0 (timer) and IRQ1 (keyboard), mask everything else
  outb(PIC1_DATA, 0xFF & ~(1 << 0) & ~(1 << 1));
  outb(PIC2_DATA, 0xFF);
}

void init_idt(void) {
  for (int i = 0; i < 256; i++)
    set_idt_entry(i, isr_default_handler);

  set_idt_entry(32, isr_irq0);
  set_idt_entry(33, isr_irq1);

  pic_remap();
  lidt_load();
}