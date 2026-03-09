#include "include/ps2.h"
#include "include/console.h"
#include "include/nvec.h" // ← NVEC mode toggle + tick
#include <stdint.h>

// Simple ring buffer for keyboard input (shared between PS/2 and USB)
static volatile unsigned int in_head = 0;
static volatile unsigned int in_tail = 0;
static volatile char in_buf[256];

// Add a character to the input buffer (callable from USB HID driver)
void input_add_char(char c) {
  unsigned int next = (in_head + 1) & 255;
  if (next != in_tail) {
    in_buf[in_head] = c;
    in_head = next;
  }
}

// Port addresses
#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Minimal scancode set 1 -> ASCII map for common keys
static const char scancode_map[128] = {
    0,   27,  '1',  '2',  '3',  '4', '5', '6',  '7', '8', '9', '0',
    '-', '=', '\b', '\t', 'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
    'o', 'p', '[',  ']',  '\n', 0,   'a', 's',  'd', 'f', 'g', 'h',
    'j', 'k', 'l',  ';',  '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm',  ',',  '.',  '/', 0,   '*',  0,   ' ',
};

// Shifted character map (scancode set 1)
static const char scancode_shift_map[128] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',  0,
    0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|',  'Z',
    'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ',
};

void ps2_handle_interrupt(void) {
  uint8_t status = inb(KBD_STATUS_PORT);
  if (!(status & 1))
    return;
  uint8_t sc = inb(KBD_DATA_PORT);

  static volatile int shift = 0;
  static volatile int caps = 0;
  static volatile int numlock = 0;
  static volatile int leds_state = 0;
  static volatile int extended = 0;

  // ── Modifier tracking for Ctrl+Alt+F10 ──────────────────────────────────
  // We use static ints so state persists across calls.
  static volatile int key_ctrl = 0; // Left Ctrl  = 0x1D
  static volatile int key_alt = 0;  // Left Alt   = 0x38
  // ────────────────────────────────────────────────────────────────────────

  // Ignore ACK / resend bytes from keyboard controller
  if (sc == 0xFA || sc == 0xFE)
    return;

  // ── Break codes (key release) ────────────────────────────────────────────
  if (sc & 0x80) {
    uint8_t code = sc & 0x7F;

    if (code == 0x2A || code == 0x36)
      shift = 0; // Shift released
    if (code == 0x1D)
      key_ctrl = 0; // Ctrl released
    if (code == 0x38)
      key_alt = 0; // Alt released

    extended = 0;
    return;
  }

  // ── Extended prefix ──────────────────────────────────────────────────────
  if (sc == 0xE0) {
    extended = 1;
    return;
  }

  // ── Modifier make codes ──────────────────────────────────────────────────
  if (sc == 0x2A || sc == 0x36) {
    shift = 1;
    return;
  } // Left/Right Shift
  if (sc == 0x1D) {
    key_ctrl = 1;
    return;
  } // Left Ctrl
  if (sc == 0x38) {
    key_alt = 1;
    return;
  } // Left Alt

  // ── Ctrl+Alt+F10 → toggle NVEC graphics mode ────────────────────────────
  // F10 = scancode 0x44
  if (sc == 0x44 && key_ctrl) {
    nvec_toggle();
    extended = 0;
    return;
  }

  // ── In graphics mode: swallow all remaining key presses ─────────────────
  // The shell must not receive characters while NVEC owns the screen.
  // Modifier tracking above still runs (so Ctrl+Alt+F10 to exit works).
  if (nvec_suppress_console()) {
    extended = 0;
    return;
  }

  // ── Caps Lock ────────────────────────────────────────────────────────────
  if (sc == 0x3A) {
    caps = !caps;
    leds_state = (leds_state & ~0x04) | (caps ? 0x04 : 0);
    for (int i = 0; i < 100000; i++) {
      if (!(inb(KBD_STATUS_PORT) & 2))
        break;
    }
    outb(KBD_DATA_PORT, 0xED);
    for (int i = 0; i < 100000; i++) {
      if (!(inb(KBD_STATUS_PORT) & 2))
        break;
    }
    outb(KBD_DATA_PORT, (uint8_t)leds_state);
    fb_puts(caps ? "CAPS ON\n" : "CAPS OFF\n");
    return;
  }

  // ── Num Lock ─────────────────────────────────────────────────────────────
  if (sc == 0x77) {
    numlock = !numlock;
    leds_state = (leds_state & ~0x02) | (numlock ? 0x02 : 0);
    for (int i = 0; i < 100000; i++) {
      if (!(inb(KBD_STATUS_PORT) & 2))
        break;
    }
    outb(KBD_DATA_PORT, 0xED);
    for (int i = 0; i < 100000; i++) {
      if (!(inb(KBD_STATUS_PORT) & 2))
        break;
    }
    outb(KBD_DATA_PORT, (uint8_t)leds_state);
    fb_puts(numlock ? "NUM ON\n" : "NUM OFF\n");
    return;
  }

  // ── Keypad numeric block (0x47..0x53) with NumLock ──────────────────────
  if (sc >= 0x47 && sc <= 0x53) {
    static const char keypad_num_map[] = {'7', '8', '9', '-', '4', '5', '6',
                                          '+', '1', '2', '3', '0', '.'};
    char out = 0;
    if (numlock)
      out = keypad_num_map[sc - 0x47];
    if (out) {
      unsigned int next = (in_head + 1) & 255;
      if (next != in_tail) {
        in_buf[in_head] = out;
        in_head = next;
      }
    }
    return;
  }

  // ── Regular character keys ───────────────────────────────────────────────
  if (sc < sizeof(scancode_map)) {
    char c = scancode_map[sc];
    char c_shift = scancode_shift_map[sc];
    if (c) {
      char out = c;
      if (shift && c_shift) {
        out = c_shift;
      } else {
        // Caps Lock: toggle case for letters only
        if (c >= 'a' && c <= 'z') {
          if (shift ^ caps)
            out = (char)(c - ('a' - 'A'));
        }
      }

      unsigned int next = (in_head + 1) & 255;
      if (next != in_tail) {
        in_buf[in_head] = out;
        in_head = next;
      }
    }
  }

  extended = 0;
}

// USB keyboard polling function (from xhci.c)
extern void usb_kbd_poll(void);

int ps2_getchar(void) {
  while (in_head == in_tail) {
    // In NVEC graphics mode: drive the particle animation each loop
    // iteration.  Keyboard input (except modifier tracking) is suppressed
    // by ps2_handle_interrupt, so we just keep ticking until the user
    // presses Ctrl+Alt+F12 to return to shell mode.
    if (nvec_suppress_console()) {
      nvec_tick();
      // Still check PS/2 so Ctrl+Alt+F12 is caught (modifiers are always
      // tracked inside ps2_handle_interrupt even in graphics mode).
      uint8_t st = inb(KBD_STATUS_PORT);
      if (st & 1)
        ps2_handle_interrupt();
      continue;
    }

    usb_kbd_poll();

    uint8_t status = inb(KBD_STATUS_PORT);
    if (status & 1)
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

  uint8_t status = inb(KBD_STATUS_PORT);
  if (status & 1)
    ps2_handle_interrupt();

  if (in_head == in_tail)
    return -1;
  char c = in_buf[in_tail];
  in_tail = (in_tail + 1) & 255;
  return (int)c;
}

void ps2_init(void) {
  // Flush output buffer
  while (inb(KBD_STATUS_PORT) & 1)
    inb(KBD_DATA_PORT);

  // Enable Keyboard (Command 0xAE)
  outb(KBD_STATUS_PORT, 0xAE);

  // Enable Scanning (0xF4)
  for (int i = 0; i < 10000; i++) {
    if (!(inb(KBD_STATUS_PORT) & 2))
      break;
  }
  outb(KBD_DATA_PORT, 0xF4);

  // Wait for ACK
  uint8_t ack = 0;
  for (int i = 0; i < 100000; i++) {
    if (inb(KBD_STATUS_PORT) & 1) {
      ack = inb(KBD_DATA_PORT);
      break;
    }
  }
  (void)ack;
}