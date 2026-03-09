#ifndef NVEC_H
#define NVEC_H

#include <stddef.h>
#include <stdint.h>

// Display mode state
typedef enum {
  DISPLAY_MODE_SHELL = 0,
  DISPLAY_MODE_GRAPHICS = 1
} display_mode_t;

// Framebuffer info (filled at init from Limine)
typedef struct {
  uint32_t *addr;
  uint32_t width;
  uint32_t height;
  uint32_t pitch; // bytes per scanline
} nvec_fb_t;

// ── Public API ──────────────────────────────────────────────────────────────

// Call once after Limine gives you the framebuffer.
void nvec_init(uint32_t *fb_addr, uint32_t width, uint32_t height,
               uint32_t pitch);

// Toggle / enter / exit graphics mode.
void nvec_toggle(void);
void nvec_enter_graphics(void);
void nvec_exit_graphics(void);

// Query current mode.
display_mode_t nvec_get_mode(void);

// Returns 1 while in graphics mode — used by kprintf/kprint to suppress
// framebuffer output (route to serial only).
int nvec_suppress_console(void);

// ── Animation tick ──────────────────────────────────────────────────────────
// Call this from your main idle/poll loop as fast as possible while in
// graphics mode.  It advances particle positions, redraws the frame, and
// updates the FPS counter in the status bar.
//
// Example (in ps2_getchar or your kernel main loop):
//
//   while (1) {
//       if (nvec_suppress_console())
//           nvec_tick();
//       else {
//           usb_kbd_poll();
//           // check PS/2 ...
//       }
//   }
//
void nvec_tick(void);

#endif // NVEC_H