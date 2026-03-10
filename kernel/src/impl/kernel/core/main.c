#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// BSD COMPATIBILITY FONT
typedef uint8_t u_int8_t;
typedef uint8_t u_char;
typedef uint16_t u_int16_t;
typedef uint16_t u_short;
typedef uint32_t u_int32_t;
typedef uint32_t u_int;

#define WSDISPLAY_FONTENC_ISO 0
#define WSDISPLAY_FONTORDER_L2R 0

struct wsdisplay_font {
  const char *name;
  int index;
  int firstchar;
  int numchars;
  int encoding;
  int fontwidth;
  int fontheight;
  int stride;
  int bitorder;
  int byteorder;
  void *cookie;
  void *data;
};

#include "fs/fat32.h"
#include "include/acpi.h"
#include "include/ahci.h"
#include "include/autoconf.h"
#include "include/beep.h"
#include "include/devfs.h"
#include "include/elf.h"
#include "include/fat32_vfs.h"
#include "include/gallant12x22.h"
#include "include/idt.h"
#include "include/init_fs.h"
#include "include/limine.h"
#include "include/mm.h"
#include "include/nvec.h"
#include "include/nvnode.h"
#include "include/panic.h"
#include "include/procfs.h"
#include "include/ps2.h"
#include "include/ramfs.h"
#include "include/serial.h"
#include "include/tar_extract.h"
#include "include/timer.h"
#include "include/usb.h"
#include "include/vfs.h"
#include "include/vnode.h"
#include "include/vray.h"

#define PACKED __attribute__((packed))
#define FONT_WIDTH 12
#define FONT_HEIGHT 22
#define FONT_FIRST_CHAR 32
#define FB_BG_COLOR 0xFF000000

void kprint(const char *str, uint32_t color);
void kprintf(const char *format, uint32_t color, ...);

uint8_t *fb_addr;
#define FB_BACKBUFFER_MAX_SIZE (1920 * 1200 * 4)
static uint8_t fb_backbuffer_static[FB_BACKBUFFER_MAX_SIZE]
    __attribute__((aligned(4096)));
uint8_t *fb_backbuffer = NULL;
uint32_t fb_pitch, fb_width, fb_height;
uint8_t fb_bpp;
uint32_t fb_size = 0;
uint32_t cursor_x = 0, cursor_y = 0;
static int suppress_fb = 0;
static int fb_dirty = 0;

static int str_eq(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && *b) {
    if (*a != *b)
      return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static int contains_substr(const char *s, const char *sub) {
  if (!s || !sub)
    return 0;
  for (int i = 0; s[i]; i++) {
    int j = 0;
    while (sub[j] && s[i + j] && s[i + j] == sub[j])
      j++;
    if (!sub[j])
      return 1;
  }
  return 0;
}

static int my_strcmp(const char *s1, const char *s2) {
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  while (*s1 && *s2 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return *s1 - *s2;
}

static size_t my_strlen(const char *s) {
  if (!s)
    return 0;
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

static char *my_strncpy(char *dest, const char *src, size_t n) {
  if (!dest || !src)
    return dest;
  size_t i = 0;
  while (i < n && src[i]) {
    dest[i] = src[i];
    i++;
  }
  if (i < n)
    dest[i] = '\0';
  return dest;
}

void put_pixel(int x, int y, uint32_t color) {
  if (x >= (int)fb_width || y >= (int)fb_height)
    return;
  uint64_t offset = (y * fb_pitch) + (x * (fb_bpp / 8));
  if (fb_backbuffer)
    *(uint32_t *)(fb_backbuffer + offset) = color;
  else
    *(volatile uint32_t *)(fb_addr + offset) = color;
  fb_dirty = 1;
}

void fb_flush(void) {
  if (!fb_backbuffer || !fb_addr || !fb_dirty)
    return;
  uint64_t *src = (uint64_t *)fb_backbuffer;
  uint64_t *dst = (uint64_t *)fb_addr;
  uint32_t count = fb_size / 8;
  for (uint32_t i = 0; i < count; i++)
    dst[i] = src[i];
  fb_dirty = 0;
}

void draw_char(char c, int x, int y, uint32_t color) {
  unsigned char uc = (unsigned char)c;
  if (uc < FONT_FIRST_CHAR)
    return;
  int index = uc - FONT_FIRST_CHAR;
  int offset = index * FONT_HEIGHT * 2;
  for (int row = 0; row < FONT_HEIGHT; row++) {
    uint8_t byte1 = gallant12x22_data[offset + (row * 2)];
    uint8_t byte2 = gallant12x22_data[offset + (row * 2) + 1];
    uint16_t line = (byte1 << 8) | byte2;
    for (int col = 0; col < FONT_WIDTH; col++)
      if ((line >> (15 - col)) & 1)
        put_pixel(x + col, y + row, color);
  }
}

void kprint(const char *str, uint32_t color) {
  if (nvec_suppress_console()) {
    serial_write(str);
    return;
  }
  for (int i = 0; str[i] != '\0'; i++) {
    char c = str[i];
    if (c == '\n') {
      if (!suppress_fb) {
        cursor_x = 0;
        cursor_y += FONT_HEIGHT;
        if ((uint32_t)cursor_y >= fb_height) {
          uint32_t row_bytes = fb_pitch;
          uint32_t rows_to_move = fb_height - FONT_HEIGHT;
          uint8_t *buffer = fb_backbuffer ? fb_backbuffer : fb_addr;
          uint8_t *src2 = buffer + (FONT_HEIGHT * row_bytes);
          uint8_t *dst2 = buffer;
          size_t bytes_to_move = (size_t)rows_to_move * row_bytes;
          if (bytes_to_move > 0) {
            uint64_t *sd = (uint64_t *)dst2, *ss = (uint64_t *)src2;
            size_t q = bytes_to_move / 8;
            for (size_t t = 0; t < q; t++)
              sd[t] = ss[t];
            size_t rem = bytes_to_move % 8;
            uint8_t *bd = dst2 + q * 8, *bs = src2 + q * 8;
            for (size_t t = 0; t < rem; t++)
              bd[t] = bs[t];
          }
          if (fb_bpp == 32 && (fb_pitch % 4) == 0) {
            uint32_t *base =
                (uint32_t *)(buffer + (fb_height - FONT_HEIGHT) * fb_pitch);
            uint32_t wpl = fb_pitch / 4;
            for (uint32_t y = 0; y < FONT_HEIGHT; y++)
              for (uint32_t x = 0; x < wpl; x++)
                base[y * wpl + x] = FB_BG_COLOR;
          } else {
            for (uint32_t y = fb_height - FONT_HEIGHT; y < fb_height; y++)
              for (uint32_t x = 0; x < fb_width; x++)
                put_pixel(x, y, FB_BG_COLOR);
          }
          cursor_y = fb_height - FONT_HEIGHT;
          fb_dirty = 1;
        }
      }
      continue;
    }
    if (!suppress_fb)
      draw_char(c, cursor_x, cursor_y, color);
    if (!suppress_fb) {
      cursor_x += FONT_WIDTH;
      if (cursor_x >= fb_width - FONT_WIDTH) {
        cursor_x = 0;
        cursor_y += FONT_HEIGHT;
      }
    }
  }
  serial_write(str);
  fb_flush();
}

void fb_putc(char c) {
  if (nvec_suppress_console()) {
    serial_putc(c);
    return;
  }
  int old = suppress_fb;
  suppress_fb = 0;
  char buf[2] = {c, '\0'};
  kprint(buf, 0xFFFFFFFF);
  suppress_fb = old;
  fb_flush();
}

void fb_puts(const char *s) {
  if (nvec_suppress_console()) {
    serial_write(s);
    return;
  }
  int old = suppress_fb;
  suppress_fb = 0;
  kprint(s, 0xFFFFFFFF);
  suppress_fb = old;
  fb_flush();
}

void fb_backspace(void) {
  if (nvec_suppress_console())
    return;
  if (cursor_x >= FONT_WIDTH) {
    cursor_x -= FONT_WIDTH;
  } else {
    if (cursor_y >= FONT_HEIGHT) {
      cursor_y -= FONT_HEIGHT;
      cursor_x = fb_width - FONT_WIDTH;
    } else {
      cursor_x = 0;
      return;
    }
  }
  for (int y = 0; y < FONT_HEIGHT; y++)
    for (int x = 0; x < FONT_WIDTH; x++)
      put_pixel(cursor_x + x, cursor_y + y, FB_BG_COLOR);
  fb_flush();
  serial_putc('\b');
  serial_putc(' ');
  serial_putc('\b');
}

static uint32_t cursor_saved[FONT_WIDTH * FONT_HEIGHT];
static int cursor_saved_valid = 0;
static int cursor_visible = 0;

void fb_cursor_show(void) {
  if (suppress_fb || nvec_suppress_console())
    return;
  if (cursor_visible || !fb_addr)
    return;
  uint8_t *base = fb_backbuffer ? fb_backbuffer : fb_addr;
  uint32_t bpp_bytes = fb_bpp / 8;
  for (int y = 0; y < FONT_HEIGHT; y++) {
    for (int x = 0; x < FONT_WIDTH; x++) {
      uint64_t off =
          (uint64_t)(cursor_y + y) * fb_pitch + (cursor_x + x) * bpp_bytes;
      uint32_t *pixel = (uint32_t *)(base + off);
      cursor_saved[y * FONT_WIDTH + x] = *pixel;
      *pixel = (~(*pixel) & 0x00FFFFFF) | (*pixel & 0xFF000000);
    }
  }
  cursor_saved_valid = 1;
  cursor_visible = 1;
  fb_flush();
}

void fb_cursor_hide(void) {
  if (suppress_fb || nvec_suppress_console())
    return;
  if (!cursor_visible || !fb_addr || !cursor_saved_valid)
    return;
  uint8_t *base = fb_backbuffer ? fb_backbuffer : fb_addr;
  uint32_t bpp_bytes = fb_bpp / 8;
  for (int y = 0; y < FONT_HEIGHT; y++) {
    for (int x = 0; x < FONT_WIDTH; x++) {
      uint64_t off =
          (uint64_t)(cursor_y + y) * fb_pitch + (cursor_x + x) * bpp_bytes;
      *(uint32_t *)(base + off) = cursor_saved[y * FONT_WIDTH + x];
    }
  }
  cursor_visible = 0;
  fb_flush();
}

void reverse(char s[]) {
  int i, j;
  char c;
  for (i = 0, j = 0; s[j] != '\0'; j++)
    ;
  for (i = 0, j = j - 1; i < j; i++, j--) {
    c = s[i];
    s[i] = s[j];
    s[j] = c;
  }
}

void itoa(int64_t n, char s[], int base) {
  int i = 0, sign = 0;
  uint64_t un;
  if (base == 10 && n < 0) {
    sign = 1;
    un = -n;
  } else
    un = (uint64_t)n;
  if (un == 0) {
    s[i++] = '0';
  } else {
    do {
      int d = un % base;
      s[i++] = (d > 9) ? (d - 10) + 'A' : d + '0';
    } while ((un /= base) > 0);
  }
  if (sign)
    s[i++] = '-';
  s[i] = '\0';
  reverse(s);
}

void utoa(uint64_t n, char s[], int base) {
  int i = 0;
  if (n == 0) {
    s[i++] = '0';
  } else {
    do {
      int d = n % base;
      s[i++] = (d > 9) ? (d - 10) + 'A' : d + '0';
    } while ((n /= base) > 0);
  }
  s[i] = '\0';
  reverse(s);
}

void kprintf(const char *format, uint32_t color, ...) {
  va_list args;
  va_start(args, color);
  if (nvec_suppress_console()) {
    for (int i = 0; format[i]; i++) {
      if (format[i] == '%') {
        i++;
        int il = 0;
        if (format[i] == 'l') {
          il = 1;
          i++;
        }
        switch (format[i]) {
        case 's':
          serial_write(va_arg(args, char *));
          break;
        case 'd': {
          long long n = il ? va_arg(args, long long) : va_arg(args, int);
          char b[32];
          itoa(n, b, 10);
          serial_write(b);
          break;
        }
        case 'u': {
          uint64_t n = il ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
          char b[32];
          utoa(n, b, 10);
          serial_write(b);
          break;
        }
        case 'x': {
          uint64_t n = il ? va_arg(args, uint64_t) : va_arg(args, uint32_t);
          char b[32];
          utoa(n, b, 16);
          serial_write("0x");
          serial_write(b);
          break;
        }
        case '%':
          serial_write("%");
          break;
        }
      } else {
        char t[2] = {format[i], '\0'};
        serial_write(t);
      }
    }
    va_end(args);
    return;
  }
  for (int i = 0; format[i]; i++) {
    if (format[i] == '%') {
      i++;
      int il = 0;
      if (format[i] == 'l') {
        il = 1;
        i++;
      }
      switch (format[i]) {
      case 's':
        kprint(va_arg(args, char *), color);
        break;
      case 'd': {
        long long n = il ? va_arg(args, long long) : va_arg(args, int);
        char b[32];
        itoa(n, b, 10);
        kprint(b, color);
        break;
      }
      case 'u': {
        uint64_t n = il ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
        char b[32];
        utoa(n, b, 10);
        kprint(b, color);
        break;
      }
      case 'x': {
        uint64_t n = il ? va_arg(args, uint64_t) : va_arg(args, uint32_t);
        char b[32];
        utoa(n, b, 16);
        kprint("0x", color);
        kprint(b, color);
        break;
      }
      case '%':
        kprint("%", color);
        break;
      }
    } else {
      char b[2] = {format[i], '\0'};
      kprint(b, color);
    }
  }
  va_end(args);
}

LIMINE_BASE_REVISION(2)

__attribute__((used,
               section(".requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST};
__attribute__((
    used,
    section(".requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST};
__attribute__((used,
               section(".requests"))) volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST};
__attribute__((used,
               section(".requests"))) volatile struct limine_module_request
    module_request = {.id = LIMINE_MODULE_REQUEST};
__attribute__((used,
               section(".requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST};

uint64_t hhdm_offset;

struct __attribute__((packed)) gdt_entry {
  uint16_t limit_low, base_low;
  uint8_t base_mid, access, granularity, base_high;
};
struct __attribute__((packed)) gdt_ptr {
  uint16_t limit;
  uint64_t base;
};

struct __attribute__((packed)) gdt_tss_descriptor {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_mid;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
  uint32_t base_upper32;
  uint32_t reserved;
};

struct __attribute__((packed)) tss_entry {
  uint32_t reserved0;
  uint64_t rsp0, rsp1, rsp2;
  uint64_t reserved1;
  uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
};

static struct gdt_entry gdt[7];
static struct gdt_ptr gdt_p;
static struct tss_entry tss;
static uint8_t tss_stack[16384]; // 16 KB kernel stack for interrupts from user

static void set_gdt_entry(int i, uint32_t base, uint32_t limit, uint8_t access,
                          uint8_t gran) {
  gdt[i].base_low = base & 0xFFFF;
  gdt[i].base_mid = (base >> 16) & 0xFF;
  gdt[i].base_high = (base >> 24) & 0xFF;
  gdt[i].limit_low = limit & 0xFFFF;
  gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
  gdt[i].access = access;
}

void init_gdt(void) {
  gdt_p.limit = (sizeof(struct gdt_entry) * 7) - 1;
  gdt_p.base = (uint64_t)&gdt;

  set_gdt_entry(0, 0, 0, 0, 0);                // Null
  set_gdt_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Kernel Code (0x08)
  set_gdt_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Kernel Data (0x10)
  set_gdt_entry(3, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User Data   (0x18 | 3 = 0x1B)
  set_gdt_entry(4, 0, 0xFFFFFFFF, 0xFA, 0xAF); // User Code   (0x20 | 3 = 0x23)

  // TSS Descriptor (Selector 0x28) takes two GDT slots (16 bytes)
  uint64_t tss_base = (uint64_t)&tss;
  uint32_t tss_limit = sizeof(tss) - 1;

  struct gdt_tss_descriptor *tss_desc = (struct gdt_tss_descriptor *)&gdt[5];
  tss_desc->limit_low = tss_limit & 0xFFFF;
  tss_desc->base_low = tss_base & 0xFFFF;
  tss_desc->base_mid = (tss_base >> 16) & 0xFF;
  tss_desc->access = 0x89; // Present, Type 9 (Available 64-bit TSS)
  tss_desc->granularity = ((tss_limit >> 16) & 0x0F);
  tss_desc->base_high = (tss_base >> 24) & 0xFF;
  tss_desc->base_upper32 = (uint32_t)(tss_base >> 32);
  tss_desc->reserved = 0;

  // Initialize TSS
  for (int i = 0; i < (int)sizeof(tss); i++)
    ((uint8_t *)&tss)[i] = 0;
  tss.rsp0 = (uint64_t)tss_stack + sizeof(tss_stack);
  tss.iopb_offset = sizeof(tss);

  extern void reload_segments(void);
  __asm__ volatile("lgdt %0" ::"m"(gdt_p));
  reload_segments();

  // Load TSS
  __asm__ volatile("mov $0x28, %%ax\n"
                   "ltr %%ax"
                   :
                   :
                   : "rax", "memory");

  // Enable SSE2 — must happen before ANY xmm instruction, including ones
  // emitted by GCC in init's startup code (e.g. movq %rax,%xmm1).
  // x86-64 power-on state has CR0.EM=1 which makes xmm opcodes raise #UD.
  uint64_t cr0, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1ULL << 2); // clear EM  (was causing #UD on xmm instructions)
  cr0 |= (1ULL << 1);  // set   MP
  __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 9);  // OSFXSR    (enable SSE + FXSAVE)
  cr4 |= (1ULL << 10); // OSXMMEXCPT
  __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
}

struct tar_extract_context {
  int file_count, dir_count, error_count;
};

static int rootfs_extract_callback(const char *filename, const uint8_t *data,
                                   uint64_t size, void *userdata) {
  struct tar_extract_context *ctx = (struct tar_extract_context *)userdata;
  if (!filename || filename[0] == '\0')
    return 0;
  if (my_strcmp(filename, ".") == 0 || my_strcmp(filename, "./") == 0)
    return 0;
  if (filename[0] == '.' && filename[1] == '/')
    filename += 2;

  char abspath[512];
  if (filename[0] == '/') {
    my_strncpy(abspath, filename, sizeof(abspath) - 1);
  } else {
    abspath[0] = '/';
    my_strncpy(abspath + 1, filename, sizeof(abspath) - 2);
  }
  abspath[sizeof(abspath) - 1] = '\0';

  if (data == NULL) {
    char dirname[256];
    my_strncpy(dirname, abspath, sizeof(dirname) - 1);
    dirname[sizeof(dirname) - 1] = '\0';
    if (dirname[my_strlen(dirname) - 1] == '/')
      dirname[my_strlen(dirname) - 1] = '\0';
    if (dirname[0] && my_strcmp(dirname, ".") && my_strcmp(dirname, "/"))
      if (vfs_mkdir(dirname) == 0)
        ctx->dir_count++;
    return 0;
  }

  char parent[256];
  my_strncpy(parent, abspath, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char *last_slash = NULL;
  for (int i = 0; parent[i]; i++)
    if (parent[i] == '/')
      last_slash = &parent[i];
  if (last_slash && last_slash != parent) {
    *last_slash = '\0';
    vfs_mkdir(parent);
  }

  int fd = vfs_open(abspath, 0x0101);
  if (fd >= 0) {
    int written = vfs_write(fd, data, size);
    vfs_close(fd);
    if (written == (int)size) {
      ctx->file_count++;
      return 0;
    }
    kprintf("Warning: %s wrote %d/%lu bytes\n", 0xFFFF0000, abspath, written,
            size);
  } else {
    kprintf("Warning: failed to open %s for write\n", 0xFFFF0000, abspath);
  }
  ctx->error_count++;
  return 0;
}

// ---------------------------------------------------------------------------
// KERNEL MAIN
// ---------------------------------------------------------------------------
void kernel_main(uint64_t _unused_rdi) {

  // ── 1. Architecture init (must be first) ──────────────────────────────────
  init_gdt();
  serial_init();

  // Enable SSE/SSE2 — required before any xmm instruction executes.
  // GCC emits xmm instructions by default for x86-64 (e.g. struct copies,
  // memset). Without this, the first xmm opcode in init triggers #UD.
  //
  // CR0: clear EM (bit 2, emulate FPU), set MP (bit 1, monitor coprocessor)
  // CR4: set OSFXSR (bit 9, enable FXSAVE/FXRSTOR + SSE)
  //      set OSXMMEXCPT (bit 10, enable #XF for SSE exceptions)
  {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); // clear EM
    cr0 |= (1ULL << 1);  // set   MP
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  // OSFXSR
    cr4 |= (1ULL << 10); // OSXMMEXCPT
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr4));
  }

  // ── 2. IDT + exception handlers (before ANY memory allocation) ────────────
  // Without this, any CPU exception during boot = silent triple fault.
  init_idt();
  extern void exception_handlers_install(void);
  exception_handlers_install();

  if (LIMINE_BASE_REVISION_SUPPORTED == false) {
    for (;;)
      ;
  }

  extern unsigned char _binary_build_init_init_elf_start[]
      __attribute__((weak));
  extern unsigned char _binary_build_init_init_elf_end[] __attribute__((weak));
  extern void main(void (*print_fn)(const char *));

  // ── 3. HHDM offset ────────────────────────────────────────────────────────
  hhdm_offset = (hhdm_request.response != NULL) ? hhdm_request.response->offset
                                                : 0xFFFF800000000000ULL;

  void *acpi_rsdp_ptr =
      (rsdp_request.response != NULL) ? rsdp_request.response->address : 0;

  // ── 4. Framebuffer ────────────────────────────────────────────────────────
  struct limine_framebuffer *fb = NULL;
  if (framebuffer_request.response != NULL &&
      framebuffer_request.response->framebuffer_count > 0)
    fb = framebuffer_request.response->framebuffers[0];

  if (fb != NULL) {
    fb_addr = (uint8_t *)fb->address;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch = fb->pitch;
    fb_bpp = fb->bpp;
    fb_size = (uint32_t)fb_pitch * fb_height;
    fb_backbuffer =
        (fb_size <= FB_BACKBUFFER_MAX_SIZE) ? fb_backbuffer_static : NULL;
    for (uint32_t y = 0; y < fb_height; y++)
      for (uint32_t x = 0; x < fb_width; x++)
        put_pixel(x, y, FB_BG_COLOR);
    fb_flush();
    nvec_init((uint32_t *)fb->address, fb->width, fb->height, fb->pitch);
  }

  cursor_x = cursor_y = 0;
  serial_write("Rhoudveine Kernel Booting...\n");

  // Check quiet flag in modules
  if (module_request.response != NULL) {
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
      char *cmd = module_request.response->modules[i]->cmdline;
      if (cmd && contains_substr(cmd, "quiet"))
        suppress_fb = 1;
    }
  }

  // ── 5. Memory manager ─────────────────────────────────────────────────────
  mm_init();

  // ── 6. Timers / subsystems ────────────────────────────────────────────────
  kprintf("Initializing timer...\n", 0x00FF0000);
  pit_init(100);

#ifdef CONFIG_SMP
  extern void sched_init(void);
  sched_init();
#endif
#ifdef CONFIG_CPU_FREQ
  extern void cpufreq_init(void);
  cpufreq_init();
#endif

  extern void blk_init(void);
  blk_init();
  extern void syscall_init(void);
  syscall_init();

  kprintf("Initializing device subsystems...\n", 0x00FF0000);
#ifdef CONFIG_VNODE
  vnode_init();
#endif
#ifdef CONFIG_NVNODE
  nvnode_init();
#endif
#ifdef CONFIG_PS2
  kprintf("Initializing PS/2 Controller...\n", 0x00FF0000);
  ps2_init();
#endif
#ifdef CONFIG_ACPI
  kprintf("Initializing ACPI...\n", 0x00FF0000);
  acpi_init(acpi_rsdp_ptr);
#endif
#ifdef CONFIG_VRAY
  kprintf("Initializing VRAY (PCI)...\n", 0x00FF0000);
  vray_init();
#endif
#ifdef CONFIG_AHCI
  kprintf("Initializing AHCI...\n", 0x00FF0000);
  ahci_init();
#endif
#ifdef CONFIG_USB_HID
  extern void usb_hid_init(void);
  usb_hid_init();
#endif

  // ── 7. VFS ────────────────────────────────────────────────────────────────
#ifdef CONFIG_VFS
  kprintf("Initializing VFS...\n", 0x00FF0000);
  vfs_init();
#ifdef CONFIG_FAT32
  kprintf("Registering FAT32 filesystem...\n", 0x00FF0000);
  fat32_register();
#endif
  devfs_register();
  procfs_register();
#ifdef CONFIG_RAMFS
  ramfs_register();
#endif
#endif

  // Mount root as ramfs
  kprintf("VFS: Mounting ramfs root...\n", 0x00FF0000);
  if (vfs_mount("/", "ramfs", "none") == 0)
    kprintf("VFS: RamFS mounted as root\n", 0x00FF0000);
  else
    kprintf("VFS: Failed to mount RamFS root\n", 0xFFFF0000);

  // ── 8. Load rootfs.tar ────────────────────────────────────────────────────
  kprintf("Loading rootfs.tar from modules...\n", 0x00FF0000);
  struct limine_file *rootfs_module = NULL;
  if (module_request.response != NULL) {
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
      struct limine_file *m = module_request.response->modules[i];
      if (m->cmdline && (contains_substr(m->cmdline, "rootfs.tar") ||
                         contains_substr(m->cmdline, ".tar"))) {
        rootfs_module = m;
        break;
      }
    }
  }
  if (rootfs_module != NULL) {
    kprintf("Found rootfs.tar (%lu bytes), extracting...\n", 0x00FF0000,
            rootfs_module->size);
    struct tar_extract_context ctx = {0};
    tar_extract_all((const uint8_t *)rootfs_module->address,
                    rootfs_module->size, rootfs_extract_callback, &ctx);
    kprintf("Rootfs: %d files, %d dirs, %d errors\n", 0x00FF0000,
            ctx.file_count, ctx.dir_count, ctx.error_count);
  } else {
    kprintf("WARNING: rootfs.tar not found in bootloader modules\n",
            0xFFFF0000);
  }

  // ── 9. Create standard directory tree ────────────────────────────────────
  vfs_mkdir("/System");
  vfs_mkdir("/System/Rhoudveine");
  vfs_mkdir("/System/Rhoudveine/Booter"); // ← init lives here
  vfs_mkdir("/System/Rhoudveine/Runtime");
  vfs_mkdir("/System/Rhoudveine/Runtime/Device");
  vfs_mkdir("/System/Rhoudveine/Runtime/Process");

  // ── 10. Mount DeviceFS / ProcessFS ───────────────────────────────────────
  if (vfs_mount("/System/Rhoudveine/Runtime/Device", "DeviceFS", "none") == 0) {
    kprintf("Mounted DeviceFS\n", 0x00FF0000);
    extern void devfs_add_device(const char *name, void *device_data);
    devfs_add_device("ahci0", NULL);
    devfs_add_device("vga0", NULL);
    devfs_add_device("eth0", NULL);
    devfs_add_device("cpu0", NULL);
  } else
    kprintf("Failed to mount DeviceFS\n", 0xFFFF0000);

  if (vfs_mount("/System/Rhoudveine/Runtime/Process", "ProcessFS", "none") ==
      0) {
    kprintf("Mounted ProcessFS\n", 0x00FF0000);
    extern void procfs_add_entry(const char *name, const char *content);
    procfs_add_entry("init", "PID: 1\nName: init\nState: Running\n");
    extern uint64_t mm_get_total_memory(void);
    extern uint64_t mm_get_free_memory(void);
    char membuf[128];
    extern int sprintf(char *buf, const char *fmt, ...);
    sprintf(membuf, "MemTotal: %lu MB\nMemFree: %lu MB\n",
            mm_get_total_memory() / (1024 * 1024),
            mm_get_free_memory() / (1024 * 1024));
    procfs_add_entry("meminfo", membuf);
    extern int acpi_cpu_count;
    char cpubuf[64];
    sprintf(cpubuf, "CPU: x86_64\nCores: %d\n",
            acpi_cpu_count > 0 ? acpi_cpu_count : 1);
    procfs_add_entry("cpuinfo", cpubuf);
  } else
    kprintf("Failed to mount ProcessFS\n", 0xFFFF0000);

  // ── 11. USB / PCI ─────────────────────────────────────────────────────────
#ifdef CONFIG_XHCI
  kprintf("Initializing USB stack...\n", 0x00FF0000);
  usb_init();
#endif
  kprintf("Populating VNodes from PCI...\n", 0x00FF0000);
#ifdef CONFIG_VNODE
  vnode_populate_from_pci();
  vnode_dump_list();
#endif
#ifdef CONFIG_NVNODE
  nvnode_populate_from_pci();
  nvnode_dump_list();
#endif

  // ── 12. Banner ────────────────────────────────────────────────────────────
  {
    int old = suppress_fb;
    suppress_fb = 0;
    kprint("---- KERNEL START ENTRY ----\n", 0x00FF0000);
    kprint("\nRhoudveine OS PRE-ALPHA Release Alpha-0.004 64-bit\n",
           0xFFFFFFFF);
    kprint("Copyright (c) 2025, 2027, Cibi.inc, Altec and/or its affiliates.\n",
           0xFFFFFFFF);
    kprint("Hostname: localhost\n\n", 0xFFFFFFFF);
    kprint("64 BIT HOST DETECTED !", 0xFFFFFFFF);
    kprintf("HHDM Offset: 0x%lx\n", 0x00FF0000, hhdm_offset);
    suppress_fb = old;
  }

  // ── 13. Load and run init from VFS ───────────────────────────────────────
  const char *init_path = "/System/Rhoudveine/Booter/init";
  kprintf("Looking for init process at %s...\n", 0x00FF0000, init_path);

  int init_fd = vfs_open(init_path, 0);
  if (init_fd >= 0) {
#define INIT_MAX_SIZE (1024 * 1024)
    static uint8_t init_buffer[INIT_MAX_SIZE];
    int bytes_read = vfs_read(init_fd, init_buffer, INIT_MAX_SIZE);
    vfs_close(init_fd);

    if (bytes_read > 0) {
      kprintf("Loaded %d bytes of init. Jumping to ELF loader...\n", 0x00FF0000,
              bytes_read);
      __asm__ volatile("sti");
      int load_res =
          elf64_load_and_run(init_buffer, (uint32_t)bytes_read, fb_puts);
      kprintf("ELF loader returned: %d\n", 0xFFFF0000, load_res);
    } else {
      kprintf("Failed to read init (0 bytes)\n", 0xFFFF0000);
    }
  } else {
    kprintf("Init not found in VFS (fd=%d). Falling back to embedded init.\n",
            0xFFFF0000, init_fd);
  }

  // ── 14. Embedded init fallback ────────────────────────────────────────────
  kprint("Calling embedded init fallback\n", 0x00FF0000);
  __asm__ volatile("sti");
  main(fb_puts);
  kprintf("Embedded init returned unexpectedly\n", 0x00FF0000);
  kernel_panic_shell("Embedded init returned");
  while (1)
    __asm__ volatile("hlt");
}