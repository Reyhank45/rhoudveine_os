//
// ps2.c — PS/2 keyboard driver + USB HID report handler
//
// Features
//   • i8042 controller probe (works on AMD laptops that lack legacy PS/2)
//   • Scancode set 1 -> ASCII, shift, caps, numlock, extended keys
//   • Modifier tracking (Ctrl, Alt) for Ctrl+Alt+F10 → NVEC toggle
//   • USB HID report injection via input_add_char() (decoder lives in
//   usb_hid.c) • Shared ring buffer: both PS/2 and USB feed the same queue •
//   try_getchar() for non-blocking reads (used by shell tab-complete etc.)
//

#include "include/ps2.h"
#include "include/console.h"
#include "include/nvec.h"
#include <stddef.h>
#include <stdint.h>

// ── Shared input ring buffer ─────────────────────────────────────────────────
static volatile unsigned int in_head = 0;
static volatile unsigned int in_tail = 0;
static volatile char in_buf[256];

void input_add_char(char c) {
  unsigned int next = (in_head + 1) & 255;
  if (next != in_tail) {
    in_buf[in_head] = c;
    in_head = next;
  }
}

// ── Port helpers ─────────────────────────────────────────────────────────────
#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64
#define KBD_CMD_PORT 0x64

static inline uint8_t inb_ps2(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}
static inline void outb_ps2(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ── i8042 presence detection ─────────────────────────────────────────────────
//
// Modern AMD platforms (and some Intel thin-and-lights) may not have a real
// 8042.  We probe by sending Self Test (0xAA) and checking for 0x55 response.
// This is the same approach used by Linux atkbd.c.

static int g_i8042_present = 0;

static int i8042_wait_write(void) {
  int t = 100000;
  while (--t && (inb_ps2(KBD_STATUS_PORT) & 2))
    ;
  return t;
}
static int i8042_wait_read(void) {
  int t = 100000;
  while (--t && !(inb_ps2(KBD_STATUS_PORT) & 1))
    ;
  return t;
}

static int i8042_probe(void) {
  // Flush stale output
  for (int i = 0; i < 16 && (inb_ps2(KBD_STATUS_PORT) & 1); i++)
    inb_ps2(KBD_DATA_PORT);

  if (!i8042_wait_write())
    return 0;
  outb_ps2(KBD_CMD_PORT, 0xAA); // Self Test
  if (!i8042_wait_read())
    return 0;
  uint8_t resp = inb_ps2(KBD_DATA_PORT);
  return (resp == 0x55 || resp == 0xFC) ? 1 : 0;
}

// ── Scancode Set 1 maps
// ───────────────────────────────────────────────────────

static const char scancode_map[128] = {
    0,   27,  '1',  '2',  '3',  '4', '5', '6',  '7', '8', '9', '0',
    '-', '=', '\b', '\t', 'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
    'o', 'p', '[',  ']',  '\n', 0,   'a', 's',  'd', 'f', 'g', 'h',
    'j', 'k', 'l',  ';',  '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm',  ',',  '.',  '/', 0,   '*',  0,   ' ',
};

static const char scancode_shift_map[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',  0,
    0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|',  'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ',
};

// ── PS/2 interrupt handler
// ────────────────────────────────────────────────────

void ps2_handle_interrupt(void) {
  if (!g_i8042_present)
    return;
  uint8_t status = inb_ps2(KBD_STATUS_PORT);
  if (!(status & 1))
    return;
  uint8_t sc = inb_ps2(KBD_DATA_PORT);

  static volatile int shift = 0;
  static volatile int caps = 0;
  static volatile int numlock = 0;
  static volatile int leds_state = 0;
  static volatile int extended = 0;
  static volatile int key_ctrl = 0;
  static volatile int key_alt = 0;

  if (sc == 0xFA || sc == 0xFE)
    return; // ACK / resend

  // Break codes
  if (sc & 0x80) {
    uint8_t code = sc & 0x7F;
    if (code == 0x2A || code == 0x36)
      shift = 0;
    if (code == 0x1D)
      key_ctrl = 0;
    if (code == 0x38)
      key_alt = 0;
    extended = 0;
    return;
  }

  if (sc == 0xE0) {
    extended = 1;
    return;
  }

  // Modifier make
  if (sc == 0x2A || sc == 0x36) {
    shift = 1;
    return;
  }
  if (sc == 0x1D) {
    key_ctrl = 1;
    return;
  }
  if (sc == 0x38) {
    key_alt = 1;
    return;
  }

  // Ctrl+Alt+F10 → NVEC toggle (F10 = 0x44)
  if (sc == 0x44 && key_ctrl && key_alt) {
    nvec_toggle();
    extended = 0;
    return;
  }

  // Swallow all keypresses while in graphics mode
  if (nvec_suppress_console()) {
    extended = 0;
    return;
  }

  // Caps Lock
  if (sc == 0x3A) {
    caps = !caps;
    leds_state = (leds_state & ~0x04) | (caps ? 0x04 : 0);
    i8042_wait_write();
    outb_ps2(KBD_DATA_PORT, 0xED);
    i8042_wait_write();
    outb_ps2(KBD_DATA_PORT, (uint8_t)leds_state);
    fb_puts(caps ? "CAPS ON\n" : "CAPS OFF\n");
    return;
  }

  // Num Lock
  if (sc == 0x77) {
    numlock = !numlock;
    leds_state = (leds_state & ~0x02) | (numlock ? 0x02 : 0);
    i8042_wait_write();
    outb_ps2(KBD_DATA_PORT, 0xED);
    i8042_wait_write();
    outb_ps2(KBD_DATA_PORT, (uint8_t)leds_state);
    fb_puts(numlock ? "NUM ON\n" : "NUM OFF\n");
    return;
  }

  // Keypad 0x47..0x53
  if (sc >= 0x47 && sc <= 0x53) {
    static const char keypad_num_map[] = {'7', '8', '9', '-', '4', '5', '6',
                                          '+', '1', '2', '3', '0', '.'};
    if (numlock)
      input_add_char(keypad_num_map[sc - 0x47]);
    return;
  }

  // Regular keys
  if (sc < (uint8_t)sizeof(scancode_map)) {
    char c = scancode_map[sc];
    char c_shift =
        (sc < (uint8_t)sizeof(scancode_shift_map)) ? scancode_shift_map[sc] : 0;
    if (c) {
      char out = c;
      if (shift && c_shift) {
        out = c_shift;
      } else if (c >= 'a' && c <= 'z' && (shift ^ caps)) {
        out = (char)(c - 'a' + 'A');
      }
      input_add_char(out);
    }
  }
  extended = 0;
}

// ── getchar API
// ───────────────────────────────────────────────────────────────

extern void usb_kbd_poll(void);

int ps2_getchar(void) {
  while (in_head == in_tail) {
    if (nvec_suppress_console()) {
      nvec_tick();
      if (g_i8042_present && (inb_ps2(KBD_STATUS_PORT) & 1))
        ps2_handle_interrupt();
      continue;
    }
    usb_kbd_poll();
    if (g_i8042_present && (inb_ps2(KBD_STATUS_PORT) & 1))
      ps2_handle_interrupt();
    for (volatile int i = 0; i < 1000; i++)
      ;
  }
  char c = in_buf[in_tail];
  in_tail = (in_tail + 1) & 255;
  return (int)c;
}

int try_getchar(void) {
  usb_kbd_poll();
  if (g_i8042_present && (inb_ps2(KBD_STATUS_PORT) & 1))
    ps2_handle_interrupt();
  if (in_head == in_tail)
    return -1;
  char c = in_buf[in_tail];
  in_tail = (in_tail + 1) & 255;
  return (int)c;
}

// ── ps2_init
// ──────────────────────────────────────────────────────────────────
//
// 1. Probe for i8042.  On AMD-only laptops without PS/2, probe() returns 0
//    and we skip all setup.  USB keyboards still work via usb_kbd_poll().
// 2. If found: disable ports, flush, re-enable port 1, reset keyboard,
//    wait for BAT, enable scanning.

void ps2_init(void) {
  extern void kprintf(const char *, uint32_t, ...);

  g_i8042_present = i8042_probe();
  if (!g_i8042_present) {
    kprintf("PS/2: no i8042 found (AMD/USB-only mode)\n", 0xFFFF00);
    return;
  }
  kprintf("PS/2: i8042 present\n", 0x00FF00);

  // Disable both ports while configuring
  i8042_wait_write();
  outb_ps2(KBD_CMD_PORT, 0xAD); // disable port 1
  i8042_wait_write();
  outb_ps2(KBD_CMD_PORT, 0xA7); // disable port 2

  // Flush output buffer
  for (int i = 0; i < 16 && (inb_ps2(KBD_STATUS_PORT) & 1); i++)
    inb_ps2(KBD_DATA_PORT);

  // Re-enable port 1
  i8042_wait_write();
  outb_ps2(KBD_CMD_PORT, 0xAE);

  // Reset keyboard (0xFF)
  i8042_wait_write();
  outb_ps2(KBD_DATA_PORT, 0xFF);
  if (i8042_wait_read())
    inb_ps2(KBD_DATA_PORT); // ACK
  if (i8042_wait_read())
    inb_ps2(KBD_DATA_PORT); // BAT result (0xAA)

  // Enable scanning (0xF4)
  i8042_wait_write();
  outb_ps2(KBD_DATA_PORT, 0xF4);
  if (i8042_wait_read())
    inb_ps2(KBD_DATA_PORT); // eat ACK

  kprintf("PS/2: keyboard ready\n", 0x00FF00);
}