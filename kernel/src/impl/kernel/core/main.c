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

// The Solaris Gallant font
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
#include "include/nvec.h"         // ← NVEC graphics mode
#include "include/nvnode.h"
#include "include/panic.h"
#include "include/procfs.h"
#include "include/ps2.h"
#include "include/ramfs.h"
#include "include/serial.h"
#include "include/timer.h"
#include "include/usb.h"
#include "include/vfs.h"
#include "include/vnode.h"
#include "include/vray.h"

// GRAPHICS & TEXT
#define PACKED __attribute__((packed))
#define FONT_WIDTH 12
#define FONT_HEIGHT 22
#define FONT_FIRST_CHAR 32

#define FB_BG_COLOR 0xFF000000 // Black

// forward declare kprint/kprintf so helper can use them before their
// definitions
void kprint(const char *str, uint32_t color);
void kprintf(const char *format, uint32_t color, ...);

// Framebuffer variables
uint8_t *fb_addr;
// Static backbuffer in BSS for double buffering (max 1920x1200x4 = ~9MB)
#define FB_BACKBUFFER_MAX_SIZE (1920 * 1200 * 4)
static uint8_t fb_backbuffer_static[FB_BACKBUFFER_MAX_SIZE]
    __attribute__((aligned(4096)));
uint8_t *fb_backbuffer = NULL;
uint32_t fb_pitch, fb_width, fb_height;
uint8_t fb_bpp;
uint32_t fb_size = 0;
uint32_t cursor_x = 0, cursor_y = 0;

// If set, suppress all framebuffer drawing (still mirror to serial)
// NOTE: nvec_suppress_console() now also suppresses when in graphics mode.
static int suppress_fb = 0;

// Track if backbuffer needs flushing
static int fb_dirty = 0;

// small helper to compare strings (file-scope)
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

// contains substring (simple)
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

// Helper to extract value from key=value in cmdline
static int get_cmdline_arg(const char *cmdline, const char *key,
                           char *value_buf, int buf_len) {
  if (!cmdline || !key || !value_buf)
    return 0;

  int key_len = 0;
  while (key[key_len])
    key_len++;

  const char *p = cmdline;
  while (*p) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;

    int match = 1;
    for (int i = 0; i < key_len; i++) {
      if (p[i] != key[i]) {
        match = 0;
        break;
      }
    }

    if (match && p[key_len] == '=') {
      p += key_len + 1;
      int i = 0;
      while (*p && *p != ' ' && i < buf_len - 1) {
        value_buf[i++] = *p++;
      }
      value_buf[i] = '\0';
      return 1;
    }

    while (*p && *p != ' ')
      p++;
  }
  return 0;
}

void put_pixel(int x, int y, uint32_t color) {
  if (x >= (int)fb_width || y >= (int)fb_height)
    return;
  uint64_t offset = (y * fb_pitch) + (x * (fb_bpp / 8));
  if (fb_backbuffer) {
    *(uint32_t *)(fb_backbuffer + offset) = color;
  } else {
    *(volatile uint32_t *)(fb_addr + offset) = color;
  }
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

    for (int col = 0; col < FONT_WIDTH; col++) {
      if ((line >> (15 - col)) & 1)
        put_pixel(x + col, y + row, color);
    }
  }
}

void kprint(const char *str, uint32_t color) {
  // ── NVEC suppression ────────────────────────────────────────────────────
  // When in graphics mode, kprint must NOT touch the framebuffer at all —
  // the NVEC scene owns it. We still emit to serial for debugging.
  if (nvec_suppress_console()) {
    serial_write(str);
    return;
  }
  // ────────────────────────────────────────────────────────────────────────

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
          uint8_t *src = buffer + (FONT_HEIGHT * row_bytes);
          uint8_t *dst = buffer;
          size_t bytes_to_move = (size_t)rows_to_move * row_bytes;
          if (bytes_to_move > 0) {
            uint64_t *sd = (uint64_t *)dst;
            uint64_t *ss = (uint64_t *)src;
            size_t q = bytes_to_move / 8;
            for (size_t t = 0; t < q; t++)
              sd[t] = ss[t];
            size_t rem = bytes_to_move % 8;
            uint8_t *bd = dst + q * 8;
            uint8_t *bs = src + q * 8;
            for (size_t t = 0; t < rem; t++)
              bd[t] = bs[t];
          }

          if (fb_bpp == 32 && (fb_pitch % 4) == 0) {
            uint32_t *base =
                (uint32_t *)(buffer + (fb_height - FONT_HEIGHT) * fb_pitch);
            uint32_t words_per_line = fb_pitch / 4;
            for (uint32_t y = 0; y < FONT_HEIGHT; y++)
              for (uint32_t x = 0; x < words_per_line; x++)
                base[y * words_per_line + x] = FB_BG_COLOR;
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

// Simple framebuffer console helpers exported for other modules
void fb_putc(char c) {
  // In graphics mode, swallow fb output but keep serial
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
  // Ignore backspace in graphics mode
  if (nvec_suppress_console()) return;

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

// Cursor saved pixels for blinking
static uint32_t cursor_saved[FONT_WIDTH * FONT_HEIGHT];
static int cursor_saved_valid = 0;
static int cursor_visible = 0;

void fb_cursor_show(void) {
  if (suppress_fb || nvec_suppress_console()) return;
  if (cursor_visible) return;
  if (!fb_addr) return;
  int w = FONT_WIDTH;
  int h = FONT_HEIGHT;
  uint8_t *base = fb_backbuffer ? fb_backbuffer : fb_addr;
  uint32_t bpp_bytes = fb_bpp / 8;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint64_t offset =
          (uint64_t)(cursor_y + y) * fb_pitch + (cursor_x + x) * bpp_bytes;
      uint32_t *pixel = (uint32_t *)(base + offset);
      uint32_t val = *pixel;
      cursor_saved[y * w + x] = val;
      uint32_t inv = (~val & 0x00FFFFFF) | (val & 0xFF000000);
      *pixel = inv;
    }
  }
  cursor_saved_valid = 1;
  cursor_visible = 1;
  fb_flush();
}

void fb_cursor_hide(void) {
  if (suppress_fb || nvec_suppress_console()) return;
  if (!cursor_visible) return;
  if (!fb_addr || !cursor_saved_valid) return;
  int w = FONT_WIDTH;
  int h = FONT_HEIGHT;
  uint8_t *base = fb_backbuffer ? fb_backbuffer : fb_addr;
  uint32_t bpp_bytes = fb_bpp / 8;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint64_t offset =
          (uint64_t)(cursor_y + y) * fb_pitch + (cursor_x + x) * bpp_bytes;
      uint32_t *pixel = (uint32_t *)(base + offset);
      *pixel = cursor_saved[y * w + x];
    }
  }
  cursor_visible = 0;
  fb_flush();
}

// --------------------------------------------------------------------------
// PRINTF IMPLEMENTATION
// --------------------------------------------------------------------------
void reverse(char s[]) {
  int i, j;
  char c;
  for (i = 0, j = 0; s[j] != '\0'; j++);
  for (i = 0, j = j - 1; i < j; i++, j--) {
    c = s[i]; s[i] = s[j]; s[j] = c;
  }
}

void itoa(int64_t n, char s[], int base) {
  int i = 0;
  int sign = 0;
  uint64_t un;
  if (base == 10 && n < 0) { sign = 1; un = -n; }
  else un = (uint64_t)n;
  if (un == 0) { s[i++] = '0'; }
  else {
    do {
      int digit = un % base;
      s[i++] = (digit > 9) ? (digit - 10) + 'A' : digit + '0';
    } while ((un /= base) > 0);
  }
  if (sign) s[i++] = '-';
  s[i] = '\0';
  reverse(s);
}

void utoa(uint64_t n, char s[], int base) {
  int i = 0;
  if (n == 0) { s[i++] = '0'; }
  else {
    do {
      int digit = n % base;
      s[i++] = (digit > 9) ? (digit - 10) + 'A' : digit + '0';
    } while ((n /= base) > 0);
  }
  s[i] = '\0';
  reverse(s);
}

void kprintf(const char *format, uint32_t color, ...) {
  // Graphics mode: emit to serial only, don't touch framebuffer
  if (nvec_suppress_console()) {
    // minimal serial-only path
    va_list args;
    va_start(args, color);
    for (int i = 0; format[i] != '\0'; i++) {
      if (format[i] == '%') {
        i++;
        int is_long = 0;
        if (format[i] == 'l') { is_long = 1; i++; }
        switch (format[i]) {
          case 's': serial_write(va_arg(args, char *)); break;
          case 'd': {
            long long num = is_long ? va_arg(args, long long) : va_arg(args, int);
            char buf[32]; itoa(num, buf, 10); serial_write(buf); break;
          }
          case 'u': {
            uint64_t num = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
            char buf[32]; utoa(num, buf, 10); serial_write(buf); break;
          }
          case 'x': {
            uint64_t num = is_long ? va_arg(args, uint64_t) : va_arg(args, uint32_t);
            char buf[32]; utoa(num, buf, 16);
            serial_write("0x"); serial_write(buf); break;
          }
          case '%': serial_write("%"); break;
        }
      } else {
        char tmp[2] = {format[i], '\0'};
        serial_write(tmp);
      }
    }
    va_end(args);
    return;
  }

  // Normal shell mode path
  va_list args;
  va_start(args, color);
  for (int i = 0; format[i] != '\0'; i++) {
    if (format[i] == '%') {
      i++;
      int is_long = 0;
      if (format[i] == 'l') { is_long = 1; i++; }
      switch (format[i]) {
        case 's': kprint(va_arg(args, char *), color); break;
        case 'd': {
          long long num = is_long ? va_arg(args, long long) : va_arg(args, int);
          char buffer[32]; itoa(num, buffer, 10); kprint(buffer, color); break;
        }
        case 'u': {
          uint64_t num = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
          char buffer[32]; utoa(num, buffer, 10); kprint(buffer, color); break;
        }
        case 'x': {
          uint64_t num = is_long ? va_arg(args, uint64_t) : va_arg(args, uint32_t);
          char buffer[32]; utoa(num, buffer, 16);
          kprint("0x", color); kprint(buffer, color); break;
        }
        case '%': kprint("%", color); break;
      }
    } else {
      char buffer[2] = {format[i], '\0'};
      kprint(buffer, color);
    }
  }
  va_end(args);
}

// Set the base revision to 2
LIMINE_BASE_REVISION(2)

// HHDM mapping request
__attribute__((used,
               section(".requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST};

__attribute__((
    used,
    section(".requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST};

// Memory map request
__attribute__((used,
               section(".requests"))) volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST};

// Module request
__attribute__((used,
               section(".requests"))) volatile struct limine_module_request
    module_request = {.id = LIMINE_MODULE_REQUEST};

// RSDP request (for ACPI)
__attribute__((used,
               section(".requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST};

uint64_t hhdm_offset;

// GDT structure
struct __attribute__((packed)) gdt_entry {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t base_mid;
  uint8_t access;
  uint8_t granularity;
  uint8_t base_high;
};

struct __attribute__((packed)) gdt_ptr {
  uint16_t limit;
  uint64_t base;
};

static struct gdt_entry gdt[3];
static struct gdt_ptr gdt_p;

static void set_gdt_entry(int i, uint32_t base, uint32_t limit, uint8_t access,
                          uint8_t gran) {
  gdt[i].base_low = (base & 0xFFFF);
  gdt[i].base_mid = (base >> 16) & 0xFF;
  gdt[i].base_high = (base >> 24) & 0xFF;
  gdt[i].limit_low = (limit & 0xFFFF);
  gdt[i].granularity = (limit >> 16) & 0x0F;
  gdt[i].granularity |= gran & 0xF0;
  gdt[i].access = access;
}

void init_gdt() {
  gdt_p.limit = (sizeof(struct gdt_entry) * 3) - 1;
  gdt_p.base = (uint64_t)&gdt;
  set_gdt_entry(0, 0, 0, 0, 0);
  set_gdt_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
  set_gdt_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
  extern void reload_segments(void);
  __asm__ volatile("lgdt %0" : : "m"(gdt_p));
  reload_segments();
}

// KERNEL MAIN ENTRY
void kernel_main(uint64_t _unused_rdi) {
  init_gdt();
  serial_init();

  if (LIMINE_BASE_REVISION_SUPPORTED == false) {
    for (;;);
  }

  const char *init_path = "/System/Rhoudveine/Booter/init";
  int found_init = 0;

  extern unsigned char _binary_build_init_init_elf_start[] __attribute__((weak));
  extern unsigned char _binary_build_init_init_elf_end[]   __attribute__((weak));
  extern void main(void (*print_fn)(const char *));

  // Determine HHDM offset from Limine
  if (hhdm_request.response != NULL)
    hhdm_offset = hhdm_request.response->offset;
  else
    hhdm_offset = 0xFFFF800000000000ULL;

  void *acpi_rsdp_ptr = 0;
  if (rsdp_request.response != NULL)
    acpi_rsdp_ptr = rsdp_request.response->address;

  if (module_request.response != NULL) {
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
      struct limine_file *m = module_request.response->modules[i];
      char *cmd = m->cmdline;
      if (cmd && contains_substr(cmd, "quiet"))
        suppress_fb = 1;
    }
  }

  // --- INITIALIZE FRAMEBUFFER & DOUBLE BUFFERING ---
  struct limine_framebuffer *fb = NULL;
  if (framebuffer_request.response != NULL &&
      framebuffer_request.response->framebuffer_count > 0)
    fb = framebuffer_request.response->framebuffers[0];

  if (fb != NULL) {
    fb_addr   = (uint8_t *)fb->address;
    fb_width  = fb->width;
    fb_height = fb->height;
    fb_pitch  = fb->pitch;
    fb_bpp    = fb->bpp;
    fb_size   = (uint32_t)fb_pitch * fb_height;

    if (fb_size <= FB_BACKBUFFER_MAX_SIZE)
      fb_backbuffer = fb_backbuffer_static;
    else
      fb_backbuffer = NULL;

    // Clear screen
    for (uint32_t y = 0; y < fb_height; y++)
      for (uint32_t x = 0; x < fb_width; x++)
        put_pixel(x, y, FB_BG_COLOR);
    fb_flush();

    // ── NVEC INIT ────────────────────────────────────────────────────────
    // Must happen after framebuffer is set up.
    // We hand nvec the REAL framebuffer address (not the backbuffer) because
    // nvec writes directly through mmio_remap'd UC memory for correctness.
    // The backbuffer is the console's domain; nvec bypasses it intentionally.
    nvec_init(
        (uint32_t *)fb->address,
        (uint32_t)fb->width,
        (uint32_t)fb->height,
        (uint32_t)fb->pitch
    );
    // ─────────────────────────────────────────────────────────────────────
  }

  cursor_x = 0;
  cursor_y = 0;

  serial_write("Rhoudveine Kernel Booting...\n");

  if (module_request.response != NULL) {
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
      struct limine_file *m = module_request.response->modules[i];
      char *cmd = m->cmdline;
      kprint("Found module: ", 0x00FF0000);
      if (cmd) {
        kprint(cmd, 0xFFFFFFFF);
        if (cmd[0] == '/' && str_eq(cmd, init_path))
          found_init = 1;
      }
      kprint("\n", 0xFFFFFFFF);
    }
  }

  // Initialize memory manager
  mm_init();

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
#ifdef CONFIG_VFS
  kprintf("Initializing VFS...\n", 0x00FF0000);
  vfs_init();
#ifdef CONFIG_FAT32
  kprintf("Registering FAT32 filesystem...\n", 0x00FF0000);
  fat32_register();
#endif
  kprintf("Initializing Runtime Filesystems...\n", 0x00FF0000);
  devfs_register();
  procfs_register();
#ifdef CONFIG_RAMFS
  ramfs_register();
#endif
#endif

  char root_dev[64];
  root_dev[0] = '\0';

  if (root_dev[0] != '\0') {
    kprintf("Mounting root filesystem from %s...\n", 0x00FF0000, root_dev);
    if (vfs_mount("/", "fat32", root_dev) == 0)
      kprintf("VFS: Root filesystem mounted successfully (FAT32)\n", 0x00FF0000);
    else
      kprintf("VFS: Failed to mount root filesystem on %s\n", 0xFFFF0000, root_dev);
  } else {
    kprintf("VFS: No root= argument, defaulting to ramfs root...\n", 0xFFFFFF00);
    kprintf("DEBUG: About to call vfs_mount for ramfs\n", 0x00FFFF00);
    int result = vfs_mount("/", "ramfs", "none");
    kprintf("DEBUG: vfs_mount returned %d\n", 0x00FFFF00, result);
    if (result == 0)
      kprintf("VFS: RamFS mounted as root\n", 0x00FF0000);
    else
      kprintf("VFS: Failed to mount RamFS root\n", 0xFFFF0000);
  }

  kprintf("DEBUG: About to create directory structure\n", 0x00FFFF00);
  kprintf("DEBUG: Creating /System\n", 0x00FFFF00);
  vfs_mkdir("/System");
  kprintf("DEBUG: Creating /System/Rhoudveine\n", 0x00FFFF00);
  vfs_mkdir("/System/Rhoudveine");
  kprintf("DEBUG: Creating /System/Rhoudveine/Runtime\n", 0x00FFFF00);
  vfs_mkdir("/System/Rhoudveine/Runtime");
  kprintf("DEBUG: Creating mount points\n", 0x00FFFF00);
  vfs_mkdir("/System/Rhoudveine/Runtime/Device");
  vfs_mkdir("/System/Rhoudveine/Runtime/Process");

  if (vfs_mount("/System/Rhoudveine/Runtime/Device", "DeviceFS", "none") == 0) {
    kprintf("Mounted Device filesystem.\n", 0x00FF0000);
    extern void devfs_add_device(const char *name, void *device_data);
    devfs_add_device("ahci0", NULL);
    devfs_add_device("vga0",  NULL);
    devfs_add_device("eth0",  NULL);
    devfs_add_device("cpu0",  NULL);
    kprintf("DeviceFS: Populated with device stubs\n", 0x00FFFF00);
  } else {
    kprintf("Failed to mount Device filesystem.\n", 0xFFFF0000);
  }

  if (vfs_mount("/System/Rhoudveine/Runtime/Process", "ProcessFS", "none") == 0) {
    kprintf("Mounted Process filesystem.\n", 0x00FF0000);
    extern void procfs_add_entry(const char *name, const char *content);
    procfs_add_entry("init", "PID: 1\nName: init\nState: Running\n");

    extern uint64_t mm_get_total_memory(void);
    extern uint64_t mm_get_free_memory(void);
    uint64_t total_mb = mm_get_total_memory() / (1024 * 1024);
    uint64_t free_mb  = mm_get_free_memory()  / (1024 * 1024);

    char meminfo_buf[128];
    extern int sprintf(char *buf, const char *fmt, ...);
    sprintf(meminfo_buf, "MemTotal: %lu MB\nMemFree: %lu MB\n", total_mb, free_mb);
    procfs_add_entry("meminfo", meminfo_buf);

    extern int acpi_cpu_count;
    int cpu_cores = acpi_cpu_count > 0 ? acpi_cpu_count : 1;
    char cpuinfo_buf[128];
    sprintf(cpuinfo_buf, "CPU: x86_64\nCores: %d\n", cpu_cores);
    procfs_add_entry("cpuinfo", cpuinfo_buf);
    kprintf("ProcessFS: Populated with real system info\n", 0x00FFFF00);
  } else {
    kprintf("Failed to mount Process filesystem.\n", 0xFFFF0000);
  }

#ifdef CONFIG_XHCI
  kprintf("Initializing USB stack...\n", 0x00FF0000);
  usb_init();
#endif

  kprintf("Populating VNodes from PCI...\n", 0x00FF0000);
#ifdef CONFIG_VNODE
  vnode_populate_from_pci();
#endif
#ifdef CONFIG_NVNODE
  nvnode_populate_from_pci();
#endif
#ifdef CONFIG_VNODE
  vnode_dump_list();
#endif
#ifdef CONFIG_NVNODE
  nvnode_dump_list();
#endif

  // Always show the banner regardless of quiet mode
  {
    int old = suppress_fb;
    suppress_fb = 0;
    kprint("---- KERNEL START ENTRY ----\n", 0x00FF0000);
    kprint("\nRhoudveine OS PRE-ALPHA Release Alpha-0.004 64-bit\n", 0xFFFFFFFF);
    kprint("Copyright (c) 2025, 2027, Cibi.inc, Altec and/or its affiliates.\n", 0xFFFFFFFF);
    kprint("Hostname: localhost\n\n", 0xFFFFFFFF);
    kprint("64 BIT HOST DETECTED !", 0xFFFFFFFF);
    kprintf("HHDM Offset: 0x%lx\n", 0x00FF0000, hhdm_offset);
    suppress_fb = old;
  }

  kprint("Calling embedded init\n", 0x00FF0000);
  __asm__("sti");
  main(fb_puts);
  kprintf("Embedded init returned unexpectedly\n", 0x00FF0000);
  kernel_panic_shell("Embedded init returned");

  while (1) {
    __asm__("hlt");
  }
}
