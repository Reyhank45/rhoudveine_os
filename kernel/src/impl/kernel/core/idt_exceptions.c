//
// idt_exceptions.c — crash-printing exception handlers for vectors 0–21
//
// Call exception_handlers_install() in kernel_main AFTER init_idt().
// Overrides the default silent handler with ones that print vector,
// registers, and (for #PF) the faulting address before halting.
//

#include "include/idt.h"
#include <stdint.h>

extern void kprintf(const char *fmt, uint32_t color, ...);
extern void fb_puts(const char *s);

// Must match struct idt_entry in idt.c exactly.
// Renamed to avoid a duplicate-typedef error since both files are compiled.
struct __attribute__((packed)) exc_idt_entry {
  uint16_t offset_lo;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attr;
  uint16_t offset_mid;
  uint32_t offset_hi;
  uint32_t zero;
};

// Reach into the IDT array defined in idt.c (no longer static there)
extern struct exc_idt_entry idt[256];

static void exc_set_gate(int n, void (*handler)(void)) {
  uint64_t addr = (uint64_t)handler;
  idt[n].offset_lo = (uint16_t)(addr & 0xFFFF);
  idt[n].selector = 0x08;
  idt[n].ist = 0;
  idt[n].type_attr = 0x8E;
  idt[n].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
  idt[n].offset_hi = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
  idt[n].zero = 0;
}

// ── Exception frame
// ─────────────────────────────────────────────────────────── Layout on stack
// at the point exc_common calls exc_handler_c:
//   (high addr) ss, rsp, rflags, cs, rip  ← pushed by CPU
//               error_code, vector        ← pushed by stub
//               rax..r15                  ← pushed by exc_common
//   (low addr / rsp → rdi)
typedef struct __attribute__((packed)) {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
  uint64_t vector;
  uint64_t error_code;
  uint64_t rip, cs, rflags, rsp, ss;
} exc_frame_t;

static const char *exc_names[22] = {
    "#DE Divide Error",
    "#DB Debug",
    "#NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR BOUND Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "#09 Coprocessor Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "#15 Reserved",
    "#MF x87 FP Error",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XF SIMD FP",
    "#VE Virtualization",
    "#CP Control Protection",
};

void exc_handler_c(exc_frame_t *f) {
  uint32_t vec = (uint32_t)f->vector;
  const char *name = (vec < 22) ? exc_names[vec] : "Unknown";

  fb_puts("\n\n*** CPU EXCEPTION ***\n");
  kprintf("Vector %u: %s\n", 0xFF4444, vec, name);
  kprintf("Error code : 0x%lx\n", 0xFF4444, f->error_code);
  kprintf("RIP        : 0x%lx\n", 0xFF4444, f->rip);
  kprintf("CS         : 0x%lx  RPL=%u\n", 0xFF4444, f->cs,
          (uint32_t)(f->cs & 3));
  kprintf("RSP        : 0x%lx\n", 0xFF4444, f->rsp);
  kprintf("RFLAGS     : 0x%lx\n", 0xFF4444, f->rflags);
  kprintf("RAX=0x%lx  RBX=0x%lx\n", 0xFFAAAA, f->rax, f->rbx);
  kprintf("RCX=0x%lx  RDX=0x%lx\n", 0xFFAAAA, f->rcx, f->rdx);
  kprintf("RSI=0x%lx  RDI=0x%lx\n", 0xFFAAAA, f->rsi, f->rdi);
  kprintf("RBP=0x%lx\n", 0xFFAAAA, f->rbp);

  if (vec == 14) { // #PF — page fault
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("Fault addr (CR2): 0x%lx\n", 0xFF6666, cr2);
    kprintf("Reason: %s | %s | %s | %s\n", 0xFF6666,
            (f->error_code & 1) ? "protection" : "not-present",
            (f->error_code & 2) ? "write" : "read",
            (f->error_code & 4) ? "user-mode" : "kernel-mode",
            (f->error_code & 16) ? "ifetch" : "data");
  }

  if (vec == 13) { // #GP
    fb_puts("Tip: #GP at RIP above — check segment, alignment, or bad MSR\n");
  }

  fb_puts("\nSystem halted. Reset to reboot.\n");
  __asm__ volatile("cli");
  for (;;)
    __asm__ volatile("hlt");
}

// ── Stubs
// ───────────────────────────────────────────────────────────────────── Vectors
// that do NOT push an error code: push dummy 0 to keep frame uniform. Vectors
// that DO push an error code: just push vector number and jump.

#define STUB_NOERR(n)                                                          \
  __attribute__((naked)) static void exc_stub_##n(void) {                      \
    __asm__ volatile("pushq $0\npushq $" #n "\njmp exc_common\n" :::);         \
  }
#define STUB_ERR(n)                                                            \
  __attribute__((naked)) static void exc_stub_##n(void) {                      \
    __asm__ volatile("pushq $" #n "\njmp exc_common\n" :::);                   \
  }

// Which vectors push an error code:  8, 10, 11, 12, 13, 14, 17, 21
STUB_NOERR(0)
STUB_NOERR(1) STUB_NOERR(2) STUB_NOERR(3) STUB_NOERR(4) STUB_NOERR(5)
    STUB_NOERR(6) STUB_NOERR(7) STUB_ERR(8) STUB_NOERR(9) STUB_ERR(10)
        STUB_ERR(11) STUB_ERR(12) STUB_ERR(13) STUB_ERR(14) STUB_NOERR(15)
            STUB_NOERR(16) STUB_ERR(17) STUB_NOERR(18) STUB_NOERR(19)
                STUB_NOERR(20) STUB_ERR(21)

                    __attribute__((naked)) static void exc_common(void) {
  __asm__ volatile("pushq %%rax\npushq %%rbx\npushq %%rcx\npushq %%rdx\n"
                   "pushq %%rsi\npushq %%rdi\npushq %%rbp\n"
                   "pushq %%r8\npushq  %%r9\npushq %%r10\npushq %%r11\n"
                   "pushq %%r12\npushq %%r13\npushq %%r14\npushq %%r15\n"
                   "movq  %%rsp, %%rdi\n" // arg0 = pointer to exc_frame_t
                   "andq  $-16,  %%rsp\n" // 16-byte align before C call
                   "callq exc_handler_c\n"
                   "hlt\n" // exc_handler_c never returns, but just in case
                   ::
                       : "memory");
}

// ── Public install
// ────────────────────────────────────────────────────────────
void exception_handlers_install(void) {
  exc_set_gate(0, exc_stub_0);
  exc_set_gate(1, exc_stub_1);
  exc_set_gate(2, exc_stub_2);
  exc_set_gate(3, exc_stub_3);
  exc_set_gate(4, exc_stub_4);
  exc_set_gate(5, exc_stub_5);
  exc_set_gate(6, exc_stub_6);
  exc_set_gate(7, exc_stub_7);
  exc_set_gate(8, exc_stub_8);
  exc_set_gate(9, exc_stub_9);
  exc_set_gate(10, exc_stub_10);
  exc_set_gate(11, exc_stub_11);
  exc_set_gate(12, exc_stub_12);
  exc_set_gate(13, exc_stub_13);
  exc_set_gate(14, exc_stub_14);
  exc_set_gate(15, exc_stub_15);
  exc_set_gate(16, exc_stub_16);
  exc_set_gate(17, exc_stub_17);
  exc_set_gate(18, exc_stub_18);
  exc_set_gate(19, exc_stub_19);
  exc_set_gate(20, exc_stub_20);
  exc_set_gate(21, exc_stub_21);

  kprintf("IDT: exception handlers installed (vectors 0-21)\n", 0x00FF00);
}