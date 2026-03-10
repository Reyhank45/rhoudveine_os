//
// usb_hid.c — USB HID class driver (keyboard, Boot Protocol)
//
// Architecture
//   • usb_hid_init()           — called at boot, logs HID subsystem status
//   • usb_kbd_register()       — called by xhci.c after GET_DESCRIPTOR;
//                                logs VID/PID and registers with NVNODE
//   • usb_kbd_process_report() — called by xhci.c usb_kbd_poll() on every
//                                Transfer Event; decodes 8-byte Boot Protocol
//                                report and pushes ASCII into the shared input
//                                ring via input_add_char()
//
// Guarded by autoconf.h macros so unused code is compiled away when features
// are disabled (consistent with the rest of the Rhoudveine build system).
//
// NVEC integration
//   • Ctrl+Alt+F10 (HID usage 0x43) triggers nvec_toggle() regardless of
//     graphics mode — so the user can exit NVEC from a USB keyboard.
//   • All other keypresses are swallowed while nvec_suppress_console() != 0.
//

#include "include/nvnode.h"
#include "include/usb.h"
#include <stddef.h>
#include <stdint.h>

extern void kprintf(const char *format, uint32_t color, ...);
extern void usb_device_register(void *desc_raw);

// ── Shared input ring (owned by ps2.c, published via ps2.h) ─────────────────
extern void input_add_char(char c);

// ── USB HID Boot Protocol keyboard report (8 bytes) ─────────────────────────
//
//   Byte 0  – modifier bitmask
//               bit 0 = Left  Ctrl    bit 4 = Right Ctrl
//               bit 1 = Left  Shift   bit 5 = Right Shift
//               bit 2 = Left  Alt     bit 6 = Right Alt
//               bit 3 = Left  GUI     bit 7 = Right GUI
//   Byte 1  – reserved (always 0)
//   Bytes 2..7 – up to 6 simultaneous keycodes; 0x00 = no key, 0x01 = rollover

typedef struct {
  uint8_t modifiers;
  uint8_t reserved;
  uint8_t keycodes[6];
} __attribute__((packed)) hid_kbd_report_t;

// ── Keyboard state ───────────────────────────────────────────────────────────

static uint8_t kbd_modifiers = 0;
static uint8_t prev_keycodes[6] = {0, 0, 0, 0, 0, 0};

// ── HID Usage Page 0x07 (Keyboard) → ASCII ──────────────────────────────────
//
// Index 0 = usage 0x00. Only usages 0x04..0x38 produce printable characters
// on a US QWERTY layout.  Everything outside that range stays 0.
//
// 0x04 = a/A   0x1E = 1/!   0x28 = Enter   0x2C = Space
// 0x27 = 0/)   0x2D = -/_   0x38 = //?

static const char hid_to_ascii[128] = {
    /* 0x00-0x03 */ 0,
    0,
    0,
    0,
    /* 0x04-0x1D */ 'a',
    'b',
    'c',
    'd',
    'e',
    'f',
    'g',
    'h',
    'i',
    'j',
    'k',
    'l',
    'm',
    'n',
    'o',
    'p',
    'q',
    'r',
    's',
    't',
    'u',
    'v',
    'w',
    'x',
    'y',
    'z',
    /* 0x1E-0x27 */ '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    '0',
    /* 0x28-0x2C */ '\n',
    0x1B /*ESC*/,
    '\b',
    '\t',
    ' ',
    /* 0x2D-0x31 */ '-',
    '=',
    '[',
    ']',
    '\\',
    /* 0x32-0x38 */ 0 /*non-US#*/,
    ';',
    '\'',
    '`',
    ',',
    '.',
    '/',
    /* 0x39      */ 0, /* Caps Lock — handled as toggle, no ASCII */
    /* 0x3A-0x45 */ 0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0, /* F1-F12 */
    /* rest      */ 0,
};

static const char hid_to_ascii_shift[128] = {
    /* 0x00-0x03 */ 0,
    0,
    0,
    0,
    /* 0x04-0x1D */ 'A',
    'B',
    'C',
    'D',
    'E',
    'F',
    'G',
    'H',
    'I',
    'J',
    'K',
    'L',
    'M',
    'N',
    'O',
    'P',
    'Q',
    'R',
    'S',
    'T',
    'U',
    'V',
    'W',
    'X',
    'Y',
    'Z',
    /* 0x1E-0x27 */ '!',
    '@',
    '#',
    '$',
    '%',
    '^',
    '&',
    '*',
    '(',
    ')',
    /* 0x28-0x2C */ '\n',
    0x1B,
    '\b',
    '\t',
    ' ',
    /* 0x2D-0x31 */ '_',
    '+',
    '{',
    '}',
    '|',
    /* 0x32-0x38 */ 0,
    ':',
    '"',
    '~',
    '<',
    '>',
    '?',
    /* 0x39+     */ 0,
};

// ── usb_kbd_process_report ───────────────────────────────────────────────────
//
// Called by xhci.c:usb_kbd_poll() on every Transfer Event for the keyboard
// EP1 IN ring.  `report` points to the 8-byte DMA buffer.
//
// Signature is `void *` to match the extern in xhci.c without requiring
// xhci.c to include this header.  We cast internally.

void usb_kbd_process_report(void *report_raw) {
#ifdef CONFIG_USB_KBD
  hid_kbd_report_t *report = (hid_kbd_report_t *)report_raw;
  if (!report)
    return;

  kbd_modifiers = report->modifiers;

  int shift_held = (kbd_modifiers & 0x22) != 0; // L or R Shift
  int ctrl_held = (kbd_modifiers & 0x11) != 0;  // L or R Ctrl
  int alt_held = (kbd_modifiers & 0x44) != 0;   // L or R Alt

  // ── Ctrl+Alt+F10 → NVEC toggle ──────────────────────────────────────────
  // HID usage 0x43 = F10.  Edge-detect: only fire on new press.
  {
    int f10_now = 0, f10_prev = 0;
    for (int i = 0; i < 6; i++) {
      if (report->keycodes[i] == 0x43)
        f10_now = 1;
      if (prev_keycodes[i] == 0x43)
        f10_prev = 1;
    }
    if (f10_now && !f10_prev && ctrl_held)
      nvec_toggle();
  }

  // ── Swallow all keypresses while NVEC owns the screen ───────────────────
  if (nvec_suppress_console()) {
    for (int i = 0; i < 6; i++)
      prev_keycodes[i] = report->keycodes[i];
    return;
  }

  // ── Decode new key presses (edge-triggered) ──────────────────────────────
  for (int i = 0; i < 6; i++) {
    uint8_t kc = report->keycodes[i];
    if (kc == 0x00 || kc == 0x01)
      continue; // no key / rollover error

    // Skip keys already held from previous report
    int already_held = 0;
    for (int j = 0; j < 6; j++) {
      if (prev_keycodes[j] == kc) {
        already_held = 1;
        break;
      }
    }
    if (already_held)
      continue;

    char c = 0;

    // Ctrl+letter → control character (Ctrl+A=0x01 … Ctrl+Z=0x1A)
    if (ctrl_held && kc >= 0x04 && kc <= 0x1D) {
      c = (char)(kc - 0x03);
    } else if (kc < 128) {
      c = shift_held ? hid_to_ascii_shift[kc] : hid_to_ascii[kc];
    }

    if (c)
      input_add_char(c);
  }

  // Save for next comparison
  for (int i = 0; i < 6; i++)
    prev_keycodes[i] = report->keycodes[i];
#else
  (void)report_raw;
#endif
}

// ── usb_hid_init ─────────────────────────────────────────────────────────────

void usb_hid_init(void) {
#ifdef CONFIG_USB_HID
  kprintf("USB_HID: Initializing USB HID class driver\n", 0x00FF0000);
#ifdef CONFIG_USB_KBD
  kprintf("USB_HID: Keyboard (Boot Protocol) enabled\n", 0x00FF0000);
#endif
  kprintf("USB_HID: Ready\n", 0x00FF0000);
#endif
}

void usb_kbd_register(void *desc_raw) {
#ifdef CONFIG_USB_KBD
  if (!desc_raw)
    return;

  // Log vendor/product via centralized USB logic
  usb_device_register(desc_raw);

  // Still need VID/PID for NVNODE registration
  typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
  } __attribute__((packed)) minimal_usb_desc_t;

  minimal_usb_desc_t *desc = (minimal_usb_desc_t *)desc_raw;

#ifdef CONFIG_NVNODE
  nvnode_add_usb_device(desc->idVendor, desc->idProduct);
#endif
#endif
}