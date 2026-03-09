#include "include/xhci.h"
#include "include/console.h"
#include "include/io.h"
#include "include/mm.h"
#include "include/nvnode.h"
#include "include/stdio.h"
#include "include/vray.h"
#include <stddef.h>
#include <stdint.h>

// Forward declaration for kprintf from main.c
extern void kprintf(const char *format, uint32_t color, ...);
extern uint64_t virt_to_phys(void *vaddr);

// --- xHCI Register Structures (Spec 5.3) ---
typedef volatile struct __attribute__((packed)) {
  uint8_t caplength;
  uint8_t res1;
  uint16_t hciversion;
  uint32_t hcsparams1;
  uint32_t hcsparams2;
  uint32_t hcsparams3;
  uint32_t hccparams1;
  uint32_t dboff;
  uint32_t rtsoff;
  uint32_t hccparams2;
} xhci_cap_regs_t;

typedef volatile struct __attribute__((packed)) {
  uint32_t usbcmd;
  uint32_t usbsts;
  uint32_t pagesize;
  uint32_t res1[2];
  uint32_t dnctrl;
  uint64_t crcr;
  uint32_t res2[4];
  uint64_t dcbaap;
  uint32_t config;
} xhci_op_regs_t;

typedef volatile struct __attribute__((packed)) {
  uint32_t mfindex;
} xhci_runtime_regs_t;

// Transfer Request Block (TRB) types
enum {
  TRB_TYPE_NORMAL = 1,
  TRB_TYPE_SETUP_STAGE = 2,
  TRB_TYPE_DATA_STAGE = 3,
  TRB_TYPE_STATUS_STAGE = 4,
  TRB_TYPE_ISOCH = 5,
  TRB_TYPE_LINK = 6,
  TRB_TYPE_EVENT_DATA = 7,
  TRB_TYPE_NO_OP = 8,
  TRB_TYPE_ENABLE_SLOT = 9,
  TRB_TYPE_DISABLE_SLOT = 10,
  TRB_TYPE_ADDRESS_DEVICE = 11,
  TRB_TYPE_CONFIGURE_ENDPOINT = 12,
  TRB_TYPE_EVALUATE_CONTEXT = 13,
  TRB_TYPE_RESET_ENDPOINT = 14,
  TRB_TYPE_STOP_ENDPOINT = 15,
  TRB_TYPE_SET_TR_DEQUEUE = 16,
  TRB_TYPE_RESET_DEVICE = 17,
  TRB_TYPE_FORCE_EVENT = 18,
  TRB_TYPE_NEGOTIATE_BANDWIDTH = 19,
  TRB_TYPE_SET_LATENCY_TOLERANCE = 20,
  TRB_TYPE_GET_PORT_BANDWIDTH = 21,
  TRB_TYPE_FORCE_HEADER = 22,
  TRB_TYPE_NO_OP_COMMAND = 23,
  TRB_TYPE_TRANSFER_EVENT = 32,
  TRB_TYPE_COMMAND_COMPLETION_EVENT = 33,
  TRB_TYPE_PORT_STATUS_CHANGE_EVENT = 34,
  TRB_TYPE_BANDWIDTH_REQUEST_EVENT = 35,
  TRB_TYPE_DOORBELL_EVENT = 36,
  TRB_TYPE_HOST_CONTROLLER_EVENT = 37,
  TRB_TYPE_DEVICE_NOTIFICATION_EVENT = 38,
  TRB_TYPE_MFINDEX_WRAP_EVENT = 39,
} xhci_trb_type;

// Generic TRB structure (16 bytes)
typedef struct __attribute__((packed)) {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
} xhci_trb_t;

// Event Ring Segment Table (ERST) Entry (Spec 6.5)
typedef struct __attribute__((packed)) {
  uint64_t ring_segment_base_addr;
  uint32_t ring_segment_size;
  uint32_t rsvd;
} xhci_erst_entry_t;

// Slot Context (32 bytes)
typedef struct __attribute__((packed)) {
  uint32_t dwords[8];
} xhci_slot_context_t;

// Endpoint Context (32 bytes)
typedef struct __attribute__((packed)) {
  uint32_t dwords[8];
} xhci_endpoint_context_t;

// Device Context: 1 Slot Context + 31 Endpoint Contexts
typedef struct __attribute__((packed, aligned(64))) {
  xhci_slot_context_t slot;
  xhci_endpoint_context_t eps[31];
} xhci_device_context_t;

// Input Context: 1 Input Control Context + 1 Slot Context + 31 Endpoint
// Contexts
typedef struct __attribute__((packed, aligned(64))) {
  uint32_t drop_flags;
  uint32_t add_flags;
  uint32_t rsvd[6];
  xhci_slot_context_t slot;
  xhci_endpoint_context_t eps[31];
} xhci_input_context_t;

// Ring sizes
// FIX: Reserve one slot for the Link TRB, so usable TRBs = SIZE - 1
#define XHCI_CMD_RING_SIZE 32
#define XHCI_EVENT_RING_SIZE 32
#define XHCI_MAX_SLOTS 256
#define XHCI_DCBAA_SIZE (XHCI_MAX_SLOTS + 1)
#define XHCI_EP_RING_SIZE 32

// FIX #3: All DMA-visible structures must come from low memory (below 4GB).
// These are now pointers filled by pfa_alloc_low() at init time.
// Only structures accessed purely by the CPU (not DMA'd by controller) can be
// static.
static xhci_trb_t *xhci_cmd_ring = NULL;
static xhci_erst_entry_t *xhci_event_ring_seg_table = NULL;
static xhci_trb_t *xhci_event_ring = NULL;
static uint64_t *xhci_dcbaap_array = NULL;

// Device/input contexts and EP rings: also DMA targets, must be low memory
static xhci_device_context_t *xhci_device_context_pool = NULL;
static xhci_input_context_t *xhci_input_context_pool = NULL;
static xhci_trb_t *xhci_ep_transfer_rings =
    NULL; // [XHCI_MAX_SLOTS+1][XHCI_EP_RING_SIZE]

// Helper: index into the flat ep_transfer_rings array
#define EP_RING(slot, idx)                                                     \
  (xhci_ep_transfer_rings + ((slot) * XHCI_EP_RING_SIZE) + (idx))

static void *custom_memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  while (n-- > 0)
    *p++ = (unsigned char)c;
  return s;
}

// xHCI register pointers
static xhci_cap_regs_t *xhci_cap_regs = NULL;
static xhci_op_regs_t *xhci_op_regs = NULL;
static volatile uint32_t *xhci_doorbell_regs = NULL;
static xhci_runtime_regs_t *xhci_runtime_regs = NULL;

// Command Ring Management
static uint32_t cmd_ring_enqueue_ptr = 0;
static uint32_t cmd_ring_cycle_state = 1;

// Event Ring Management
static uint32_t event_ring_dequeue_ptr = 0;
static uint32_t event_ring_cycle_state = 1;

// USB Keyboard state
static uint32_t usb_kbd_slot = 0;
static uint16_t usb_kbd_max_packet = 8;
static uint32_t usb_kbd_port_speed = 0;
static uint32_t usb_kbd_port_id = 0;
static int usb_kbd_ep1_configured = 0;

// EP1 (keyboard interrupt IN) ring — also a DMA target
static xhci_trb_t *usb_kbd_ep1_ring = NULL;
static uint32_t usb_kbd_ep1_enqueue = 0;
static uint32_t usb_kbd_ep1_cycle = 1;

// Buffer for receiving HID report — DMA target
static uint8_t *usb_kbd_report_data = NULL;

// Forward declarations
static void xhci_address_device(uint32_t slot_id, uint32_t port_id,
                                uint32_t port_speed);
static void xhci_configure_kbd_endpoint(uint32_t slot_id, uint32_t port_speed);
static void xhci_queue_kbd_transfer(void);
static void xhci_set_boot_protocol(uint32_t slot_id);

// ---------------------------------------------------------------------------
// Allocate a physically-contiguous page from low memory (<4GB) and return
// both the physical address and a CPU-accessible virtual pointer.
// ---------------------------------------------------------------------------
static void *xhci_alloc_low(uint64_t *phys_out) {
  uint64_t phys = pfa_alloc_low();
  if (!phys)
    return NULL;
  *phys_out = phys;
  void *virt = (void *)phys_to_virt(phys);
  custom_memset(virt, 0, 4096);
  return virt;
}

// ---------------------------------------------------------------------------
// FIX #1 (CRCR) + FIX #6 (Link TRB): Place Link TRB at end of command ring.
// FIX #3: ring memory already from low pages.
// ---------------------------------------------------------------------------
static void xhci_init_cmd_ring(void) {
  cmd_ring_enqueue_ptr = 0;
  cmd_ring_cycle_state = 1;
  custom_memset(xhci_cmd_ring, 0, 4096);

  // Place Link TRB at last slot to close the ring
  xhci_trb_t *link = &xhci_cmd_ring[XHCI_CMD_RING_SIZE - 1];
  uint64_t ring_phys = virt_to_phys(xhci_cmd_ring);
  link->parameter = ring_phys; // wrap back to start
  link->status = 0;
  link->control = (TRB_TYPE_LINK << 10) | (1 << 1) | 1;
  //                 ^TRB type              ^TC bit     ^cycle bit matches
  //                 initial state

  // FIX #1: bit 0 of CRCR is RCS (Ring Cycle State), NOT bit 1.
  // Old (wrong): crcr = phys | (1 << 1)
  // New (correct):
  xhci_op_regs->crcr = ring_phys | (1 << 0); // RCS = 1
}

// ---------------------------------------------------------------------------
// xhci_send_command — blocking poll for command completion.
// FIX #6: stop before Link TRB slot (XHCI_CMD_RING_SIZE - 1).
// ---------------------------------------------------------------------------
static int xhci_send_command(xhci_trb_t *command_trb,
                             uint32_t *completion_code_out,
                             uint32_t *slot_id_out) {
  // FIX #6: leave last slot for Link TRB
  uint32_t next = (cmd_ring_enqueue_ptr + 1) % XHCI_CMD_RING_SIZE;
  if (next == (XHCI_CMD_RING_SIZE - 1) || next == 0) {
    kprintf("xHCI: Command Ring is full!\n", 0xFF0000);
    return -1;
  }

  xhci_trb_t *slot = &xhci_cmd_ring[cmd_ring_enqueue_ptr];
  slot->parameter = command_trb->parameter;
  slot->status = command_trb->status;
  // Write control last; set/clear cycle bit atomically with the rest
  uint32_t ctrl = command_trb->control;
  if (cmd_ring_cycle_state)
    ctrl |= (1 << 0);
  else
    ctrl &= ~(1 << 0);
  slot->control = ctrl;

  // Advance enqueue pointer; wrap before the Link TRB slot
  cmd_ring_enqueue_ptr++;
  if (cmd_ring_enqueue_ptr == (XHCI_CMD_RING_SIZE - 1)) {
    // Update Link TRB cycle bit to match the upcoming toggle
    xhci_trb_t *link = &xhci_cmd_ring[XHCI_CMD_RING_SIZE - 1];
    // TC bit is set; controller will toggle cycle after following the link.
    // Our software cycle state also toggles now.
    cmd_ring_cycle_state = !cmd_ring_cycle_state;
    cmd_ring_enqueue_ptr = 0;
  }

  // Ring the Host Controller doorbell (slot 0, target 0)
  xhci_doorbell_regs[0] = 0;
  kprintf("xHCI: Command sent. Waiting for completion...\n", 0x00FF0000);

  // Poll event ring for Command Completion Event
  int timeout = 2000000;
  while (timeout-- > 0) {
    xhci_trb_t *ev = &xhci_event_ring[event_ring_dequeue_ptr];
    uint32_t ev_cycle = ev->control & 1;

    if (ev_cycle == (uint32_t)event_ring_cycle_state) {
      uint32_t trb_type = (ev->control >> 10) & 0x3F;
      uint32_t completion_code = (ev->status >> 24) & 0xFF;
      uint32_t slot_id = (ev->control >> 24) & 0xFF;

      // Advance dequeue pointer
      event_ring_dequeue_ptr =
          (event_ring_dequeue_ptr + 1) % XHCI_EVENT_RING_SIZE;
      if (event_ring_dequeue_ptr == 0)
        event_ring_cycle_state = !event_ring_cycle_state;

      // Update ERDP — clear EHB (bit 3) by writing the new pointer with bit 3
      // set
      volatile uint64_t *erdp =
          (uint64_t *)((uint8_t *)xhci_runtime_regs + 0x38);
      *erdp = virt_to_phys(&xhci_event_ring[event_ring_dequeue_ptr]) | (1 << 3);

      if (trb_type == TRB_TYPE_COMMAND_COMPLETION_EVENT) {
        kprintf("xHCI: Cmd complete (Slot: %u, Code: %u)\n", 0x00FF0000,
                slot_id, completion_code);
        if (completion_code_out)
          *completion_code_out = completion_code;
        if (slot_id_out)
          *slot_id_out = slot_id;
        return (completion_code == 1) ? 0 : -2;
      } else {
        kprintf("xHCI: Ignoring event type %u while waiting for cmd.\n",
                0xFFFF00, trb_type);
      }
    }

    for (volatile int i = 0; i < 100; i++)
      ;
  }

  kprintf("xHCI: Command Completion Event timeout!\n", 0xFF0000);
  return -1;
}

// ---------------------------------------------------------------------------
// BIOS Handoff — claim controller from SMM firmware.
// Walks the xECP list, finds cap ID 1, sets OS-owned semaphore,
// waits for BIOS to clear its semaphore, then disables all SMI enables.
// ---------------------------------------------------------------------------
static void xhci_perform_bios_handoff(volatile xhci_cap_regs_t *cap_regs,
                                      uintptr_t mmio_virt_base) {
  uint32_t hccparams1 = cap_regs->hccparams1;
  uint32_t xecp_dwords = (hccparams1 >> 16) & 0xFFFF;

  if (xecp_dwords == 0) {
    kprintf("xHCI: No Extended Capabilities.\n", 0x00FF0000);
    return;
  }

  uintptr_t cap_addr = mmio_virt_base + (xecp_dwords * 4);

  while (1) {
    volatile uint32_t *cap_hdr = (volatile uint32_t *)cap_addr;
    uint32_t val = *cap_hdr;
    uint8_t cap_id = val & 0xFF;
    uint8_t next_off = (val >> 8) & 0xFF; // dwords to next cap

    if (cap_id == 1) { // USB Legacy Support Capability
      kprintf("xHCI: Found USB Legacy Support cap at 0x%lx\n", 0x00FF0000,
              cap_addr);

      volatile uint32_t *usblegsup = cap_hdr;

      // Set OS Owned Semaphore (bit 24) regardless of current owner
      *usblegsup |= (1 << 24);

      // Wait for BIOS Owned Semaphore (bit 16) to clear
      int timeout = 500000;
      while ((*usblegsup & (1 << 16)) && --timeout) {
        for (volatile int i = 0; i < 100; i++)
          ;
      }

      if (timeout > 0) {
        kprintf("xHCI: BIOS Handoff successful.\n", 0x00FF0000);
      } else {
        kprintf("xHCI: BIOS Handoff timed out! Forcing.\n", 0xFF0000);
        *usblegsup &= ~(1 << 16);
      }

      // Disable ALL SMI enables in USBLEGCTLSTS (cap + 4)
      // This prevents SMM from reclaiming the controller mid-operation
      volatile uint32_t *usblegctl = cap_hdr + 1;
      *usblegctl = 0;

      break;
    }

    if (next_off == 0)
      break;
    cap_addr += (uint32_t)next_off * 4;
  }
}

// ---------------------------------------------------------------------------
// xhci_init — main entry point
// ---------------------------------------------------------------------------
void xhci_init(void) {
  int device_index = vray_find_first_by_class_prog_if(0x0C, 0x03, 0x30);
  const struct vray_device *xhci_controller = NULL;
  if (device_index != -1)
    xhci_controller = &vray_devices()[device_index];

  if (!xhci_controller) {
    kprintf("No xHCI controller found.\n", 0xFF0000);
    return;
  }

  kprintf("xHCI controller found at %x:%x.%d\n", 0x00FF0000,
          xhci_controller->bus, xhci_controller->device,
          xhci_controller->function);

  // ------------------------------------------------------------------
  // FIX #2: Enable PCI Bus Mastering AND Memory Space before anything else
  // ------------------------------------------------------------------
  uint32_t pci_cmd =
      vray_cfg_read(xhci_controller->bus, xhci_controller->device,
                    xhci_controller->function, 0x04);
  pci_cmd |= (1 << 2) | (1 << 1); // Bus Master Enable | Memory Space Enable
  vray_cfg_write(xhci_controller->bus, xhci_controller->device,
                 xhci_controller->function, 0x04, pci_cmd);

  // ------------------------------------------------------------------
  // Read and decode BAR0 (64-bit memory BAR)
  // ------------------------------------------------------------------
  uint32_t bar0 = vray_cfg_read(xhci_controller->bus, xhci_controller->device,
                                xhci_controller->function, 0x10);
  uint64_t base_addr = 0;

  if ((bar0 & 0x6) == 0x4) { // 64-bit memory BAR (type field bits 2:1 == 0b10)
    uint32_t bar1 = vray_cfg_read(xhci_controller->bus, xhci_controller->device,
                                  xhci_controller->function, 0x14);
    base_addr = ((uint64_t)bar1 << 32) | (bar0 & 0xFFFFFFF0);
  } else if ((bar0 & 0x1) == 0) { // 32-bit memory BAR
    base_addr = (uint64_t)(bar0 & 0xFFFFFFF0);
  } else {
    kprintf("xHCI: BAR0 is not a memory BAR. Aborting.\n", 0xFF0000);
    return;
  }
  kprintf("xHCI: BAR physical = 0x%lx\n", 0x00FF0000, base_addr);

  // ------------------------------------------------------------------
  // FIX #4: Map MMIO as uncacheable (UC). mmio_remap must set PCD+PWT.
  // ------------------------------------------------------------------
  void *virt_base = mmio_remap(base_addr, 64 * 1024);
  if (!virt_base) {
    kprintf("xHCI: Failed to map MMIO.\n", 0xFF0000);
    return;
  }
  kprintf("xHCI: MMIO virt = 0x%lx\n", 0x00FF0000, (uint64_t)virt_base);

  xhci_cap_regs = (xhci_cap_regs_t *)virt_base;
  xhci_op_regs =
      (xhci_op_regs_t *)((uint8_t *)virt_base + xhci_cap_regs->caplength);
  xhci_doorbell_regs =
      (volatile uint32_t *)((uint8_t *)virt_base + xhci_cap_regs->dboff);
  xhci_runtime_regs =
      (xhci_runtime_regs_t *)((uint8_t *)virt_base + xhci_cap_regs->rtsoff);

  kprintf("xHCI: CAPLENGTH=%u HCIVERSION=0x%x\n", 0x00FF0000,
          xhci_cap_regs->caplength, xhci_cap_regs->hciversion);

  // ------------------------------------------------------------------
  // BIOS Handoff — must happen before reset
  // ------------------------------------------------------------------
  xhci_perform_bios_handoff(xhci_cap_regs, (uintptr_t)virt_base);

  // ------------------------------------------------------------------
  // FIX #2 (continued): Stop the controller before resetting it.
  // Intel Comet Lake requires HCH before HCRST.
  // ------------------------------------------------------------------
  kprintf("xHCI: Stopping controller before reset...\n", 0x00FF0000);
  xhci_op_regs->usbcmd &= ~(1 << 0); // Clear RS (Run/Stop)
  int halt_timeout = 200000;
  while (!(xhci_op_regs->usbsts & (1 << 0)) && --halt_timeout) // Wait for HCH=1
    for (volatile int i = 0; i < 10; i++)
      ;
  if (!halt_timeout)
    kprintf("xHCI: Warning: controller did not halt cleanly.\n", 0xFFFF00);
  else
    kprintf("xHCI: Controller halted.\n", 0x00FF0000);

  // Host Controller Reset
  kprintf("xHCI: Performing HCRST...\n", 0x00FF0000);
  xhci_op_regs->usbcmd |= (1 << 1); // HCRST

  int hcrst_timeout = 1000000;
  while (((xhci_op_regs->usbcmd & (1 << 1)) ||   // wait HCRST to clear
          (xhci_op_regs->usbsts & (1 << 11))) && // wait CNR to clear
         --hcrst_timeout)
    for (volatile int i = 0; i < 100; i++)
      ;

  if (!hcrst_timeout) {
    kprintf("xHCI: HCRST timeout! USBCMD=0x%x USBSTS=0x%x\n", 0xFF0000,
            xhci_op_regs->usbcmd, xhci_op_regs->usbsts);
    return;
  }
  kprintf("xHCI: HCRST complete.\n", 0x00FF0000);

  // ------------------------------------------------------------------
  // FIX #3: Allocate ALL DMA structures from low physical memory (<4GB).
  // Each pfa_alloc_low() returns a 4KB page (already zeroed by xhci_alloc_low).
  // ------------------------------------------------------------------
  uint64_t phys;

  // DCBAA (Device Context Base Address Array) — needs (MAX_SLOTS+1)*8 bytes =
  // 2056 B < 4KB
  xhci_dcbaap_array = (uint64_t *)xhci_alloc_low(&phys);
  if (!xhci_dcbaap_array) {
    kprintf("xHCI: OOM dcbaa\n", 0xFF0000);
    return;
  }
  uint64_t dcbaap_phys = phys;

  // Command Ring — 32 * 16 = 512 B < 4KB
  xhci_cmd_ring = (xhci_trb_t *)xhci_alloc_low(&phys);
  if (!xhci_cmd_ring) {
    kprintf("xHCI: OOM cmd_ring\n", 0xFF0000);
    return;
  }

  // Event Ring — 32 * 16 = 512 B < 4KB
  xhci_event_ring = (xhci_trb_t *)xhci_alloc_low(&phys);
  if (!xhci_event_ring) {
    kprintf("xHCI: OOM event_ring\n", 0xFF0000);
    return;
  }

  // Event Ring Segment Table — 1 * 16 = 16 B < 4KB
  xhci_event_ring_seg_table = (xhci_erst_entry_t *)xhci_alloc_low(&phys);
  if (!xhci_event_ring_seg_table) {
    kprintf("xHCI: OOM erst\n", 0xFF0000);
    return;
  }

  // Device Context pool — (256+1) * 1024 B = ~257 KB: need 64 pages
  // Allocate contiguously using pfa_alloc_low in sequence.
  // For simplicity we allocate one page per slot (1KB per slot, so one page
  // holds 4 slots). A portable approach: allocate enough pages and use the
  // first virtual address.
  {
    // 257 device contexts * 1024 bytes = 263168 bytes = 65 pages (ceil)
    uint64_t dc_phys_base;
    uint8_t *dc_virt = NULL;
    // Allocate first page and record base
    dc_virt = (uint8_t *)xhci_alloc_low(&dc_phys_base);
    if (!dc_virt) {
      kprintf("xHCI: OOM dev_ctx\n", 0xFF0000);
      return;
    }
    xhci_device_context_pool = (xhci_device_context_t *)dc_virt;
    // Allocate remaining pages (64 more = 65 total)
    for (int p = 1; p < 65; p++) {
      uint64_t pg_phys;
      void *pg = xhci_alloc_low(&pg_phys);
      if (!pg) {
        kprintf("xHCI: OOM dev_ctx page %d\n", 0xFF0000, p);
        return;
      }
    }
  }

  // Input Context pool — same size as device context pool
  {
    uint64_t ic_phys_base;
    uint8_t *ic_virt = (uint8_t *)xhci_alloc_low(&ic_phys_base);
    if (!ic_virt) {
      kprintf("xHCI: OOM input_ctx\n", 0xFF0000);
      return;
    }
    xhci_input_context_pool = (xhci_input_context_t *)ic_virt;
    for (int p = 1; p < 67;
         p++) { // input ctx = 1056 bytes each, 257 * 1056 / 4096 = 67 pages
      uint64_t pg_phys;
      void *pg = xhci_alloc_low(&pg_phys);
      if (!pg) {
        kprintf("xHCI: OOM input_ctx page %d\n", 0xFF0000, p);
        return;
      }
    }
  }

  // EP Transfer Rings — (256+1) slots * 32 TRBs * 16 bytes = ~131 KB = 33 pages
  {
    uint64_t ep_phys_base;
    uint8_t *ep_virt = (uint8_t *)xhci_alloc_low(&ep_phys_base);
    if (!ep_virt) {
      kprintf("xHCI: OOM ep_rings\n", 0xFF0000);
      return;
    }
    xhci_ep_transfer_rings = (xhci_trb_t *)ep_virt;
    for (int p = 1; p < 33; p++) {
      uint64_t pg_phys;
      void *pg = xhci_alloc_low(&pg_phys);
      if (!pg) {
        kprintf("xHCI: OOM ep_ring page %d\n", 0xFF0000, p);
        return;
      }
    }
  }

  // EP1 keyboard ring — 32 * 16 = 512 B < 4KB
  usb_kbd_ep1_ring = (xhci_trb_t *)xhci_alloc_low(&phys);
  if (!usb_kbd_ep1_ring) {
    kprintf("xHCI: OOM kbd_ep1_ring\n", 0xFF0000);
    return;
  }

  // HID report buffer — 8 bytes < 4KB
  usb_kbd_report_data = (uint8_t *)xhci_alloc_low(&phys);
  if (!usb_kbd_report_data) {
    kprintf("xHCI: OOM kbd_report\n", 0xFF0000);
    return;
  }

  // ------------------------------------------------------------------
  // Configure Max Device Slots
  // ------------------------------------------------------------------
  uint32_t hcsparams1 = xhci_cap_regs->hcsparams1;
  uint32_t max_slots = hcsparams1 & 0xFF;
  kprintf("xHCI: Max Slots: %u\n", 0x00FF0000, max_slots);
  xhci_op_regs->config = max_slots;

  // ------------------------------------------------------------------
  // Scratchpad Buffer Allocation
  // ------------------------------------------------------------------
  uint32_t hcsparams2 = xhci_cap_regs->hcsparams2;
  uint32_t max_scratch_lo = (hcsparams2 >> 21) & 0x1F;
  uint32_t max_scratch_hi = (hcsparams2 >> 27) & 0x1F;
  uint32_t max_scratchpad_bufs = (max_scratch_hi << 5) | max_scratch_lo;
  kprintf("xHCI: Scratchpad buffers required: %u\n", 0x00FF0000,
          max_scratchpad_bufs);

  if (max_scratchpad_bufs > 0) {
    if (max_scratchpad_bufs > 256)
      max_scratchpad_bufs = 256;

    uint64_t sp_array_phys;
    uint64_t *sp_array_virt = (uint64_t *)xhci_alloc_low(&sp_array_phys);
    if (!sp_array_virt) {
      kprintf("xHCI: Failed to alloc scratchpad array!\n", 0xFF0000);
      xhci_dcbaap_array[0] = 0;
    } else {
      for (uint32_t i = 0; i < max_scratchpad_bufs; i++) {
        uint64_t sp_page_phys = pfa_alloc_low();
        if (!sp_page_phys) {
          kprintf("xHCI: OOM scratchpad page %u\n", 0xFF0000, i);
          break;
        }
        sp_array_virt[i] = sp_page_phys;
      }
      xhci_dcbaap_array[0] = sp_array_phys;
      kprintf("xHCI: DCBAAP[0] (scratchpad) = 0x%lx\n", 0x00FF0000,
              sp_array_phys);
    }
  } else {
    xhci_dcbaap_array[0] = 0;
  }

  // Set DCBAAP register
  if (dcbaap_phys >= 0x100000000ULL)
    kprintf("xHCI: WARNING DCBAAP >4GB! DMA will fail.\n", 0xFF0000);
  xhci_op_regs->dcbaap = dcbaap_phys;
  kprintf("xHCI: DCBAAP = 0x%lx\n", 0x00FF0000, dcbaap_phys);

  // ------------------------------------------------------------------
  // FIX #1 + #6: Initialize Command Ring with Link TRB, set CRCR correctly
  // ------------------------------------------------------------------
  xhci_init_cmd_ring(); // sets crcr with bit 0 (RCS), places Link TRB at last
                        // slot

  // ------------------------------------------------------------------
  // Initialize Event Ring
  // ------------------------------------------------------------------
  event_ring_dequeue_ptr = 0;
  event_ring_cycle_state = 1;

  // ERST entry
  xhci_event_ring_seg_table[0].ring_segment_base_addr =
      virt_to_phys(xhci_event_ring);
  xhci_event_ring_seg_table[0].ring_segment_size = XHCI_EVENT_RING_SIZE;
  xhci_event_ring_seg_table[0].rsvd = 0;

  // Interrupter 0 register set (runtime base + 0x20)
  // Offsets within interrupter set: IMAN=+0, IMOD=+4, ERSTSZ=+8, ERSTBA=+0x10,
  // ERDP=+0x18
  volatile uint32_t *iman = (uint32_t *)((uint8_t *)xhci_runtime_regs + 0x20);
  volatile uint32_t *erstsz = (uint32_t *)((uint8_t *)xhci_runtime_regs + 0x28);
  volatile uint64_t *erstba = (uint64_t *)((uint8_t *)xhci_runtime_regs + 0x30);
  volatile uint64_t *erdp = (uint64_t *)((uint8_t *)xhci_runtime_regs + 0x38);

  // Per spec 4.9.3: ERSTSZ → ERDP → ERSTBA (order matters)
  *erstsz = 1;
  *erdp = virt_to_phys(xhci_event_ring); // initial dequeue pointer, EHB=0
  *erstba = virt_to_phys(xhci_event_ring_seg_table);

  // FIX #7: Enable Interrupter 0 (IE bit = bit 1 of IMAN)
  *iman |= (1 << 1);

  kprintf("xHCI: ERSTBA=0x%lx ERSTSZ=%u ERDP=0x%lx\n", 0x00FF0000, *erstba,
          *erstsz, *erdp);

  // Enable host-controller-level interrupt flag in USBCMD (INTE = bit 2)
  xhci_op_regs->usbcmd |= (1 << 2);

  // ------------------------------------------------------------------
  // Start the controller
  // ------------------------------------------------------------------
  kprintf("xHCI: Starting controller...\n", 0x00FF0000);
  xhci_op_regs->usbcmd |= (1 << 0); // RS = 1

  int hch_timeout = 1000000;
  while ((xhci_op_regs->usbsts & (1 << 0)) && --hch_timeout) // wait HCH=0
    for (volatile int i = 0; i < 100; i++)
      ;
  if (!hch_timeout) {
    kprintf("xHCI: Start timeout! USBSTS=0x%x\n", 0xFF0000,
            xhci_op_regs->usbsts);
    return;
  }
  kprintf("xHCI: Controller running.\n", 0x00FF0000);

  // ------------------------------------------------------------------
  // Scan root hub ports
  // ------------------------------------------------------------------
  uint32_t max_ports = (xhci_cap_regs->hcsparams1 >> 24) & 0xFF;
  kprintf("xHCI: Ports: %u\n", 0x00FF0000, max_ports);

  // Pass 1: Power on all ports
  for (uint32_t i = 0; i < max_ports; i++) {
    volatile uint32_t *portsc =
        (uint32_t *)((uint8_t *)xhci_op_regs + 0x400 + i * 0x10);
    uint32_t val = *portsc;
    if (!(val & (1 << 9))) // PP not set
      *portsc = val | (1 << 9);
  }

  // Wait for ports to stabilize (USB spec requires ≥100 ms debounce)
  kprintf("xHCI: Port stabilization delay...\n", 0x00FF0000);
  for (volatile int d = 0; d < 10000000; d++)
    ;

  // Pass 2: Enumerate connected devices
  for (uint32_t i = 0; i < max_ports; i++) {
    uint32_t port_id = i + 1;
    volatile uint32_t *portsc =
        (uint32_t *)((uint8_t *)xhci_op_regs + 0x400 + i * 0x10);
    uint32_t val = *portsc;

    kprintf("xHCI: Port %u PORTSC=0x%x\n", 0x00FF0000, port_id, val);

    if (!(val & (1 << 0))) // CCS = 0: no device
      goto clear_port_bits;

    kprintf("xHCI: Device on Port %u\n", 0x00FF0000, port_id);

    // Ensure port is powered
    if (!(val & (1 << 9))) {
      *portsc = val | (1 << 9);
      for (volatile int d = 0; d < 100000; d++)
        ;
      val = *portsc;
    }

    // Reset port (PR = bit 4)
    // Write only PR; preserve PP (bit 9). Do NOT use |= on PORTSC directly with
    // status-change bits present — that would accidentally clear them.
    // Safe approach: write a value with only the bits we intend to
    // set/preserve.
    *portsc = (1 << 9) |
              (1 << 4); // PP=1, PR=1, all R/WC bits written as 0 (no effect)

    {
      int pr_timeout = 1000000;
      while (!(*portsc & (1 << 21)) && --pr_timeout) // Wait PRC=1
        for (volatile int d = 0; d < 100; d++)
          ;
      if (!pr_timeout) {
        kprintf("xHCI: Port %u reset timeout!\n", 0xFF0000, port_id);
        goto clear_port_bits;
      }
    }

    kprintf("xHCI: Port %u reset done. PORTSC=0x%x\n", 0x00FF0000, port_id,
            *portsc);

    // Clear PRC (bit 21) — write 1 to clear
    *portsc = (1 << 9) | (1 << 21);

    {
      uint32_t port_speed = (*portsc >> 10) & 0xF;
      kprintf("xHCI: Port %u speed ID=%u\n", 0x00FF0000, port_id, port_speed);

      // Flush any pending events before sending commands
      for (int flush = 0; flush < 32; flush++) {
        xhci_trb_t *ev = &xhci_event_ring[event_ring_dequeue_ptr];
        if ((ev->control & 1) != (uint32_t)event_ring_cycle_state)
          break;
        uint32_t ftype = (ev->control >> 10) & 0x3F;
        kprintf("xHCI: Flushed event type %u\n", 0x00FF0000, ftype);
        event_ring_dequeue_ptr =
            (event_ring_dequeue_ptr + 1) % XHCI_EVENT_RING_SIZE;
        if (event_ring_dequeue_ptr == 0)
          event_ring_cycle_state = !event_ring_cycle_state;
      }
      volatile uint64_t *erdp2 =
          (uint64_t *)((uint8_t *)xhci_runtime_regs + 0x38);
      *erdp2 =
          virt_to_phys(&xhci_event_ring[event_ring_dequeue_ptr]) | (1 << 3);

      // Clear EINT/PCD status bits
      xhci_op_regs->usbsts = (1 << 3) | (1 << 4);

      if (xhci_op_regs->usbsts & (1 << 0)) {
        kprintf("xHCI: Controller halted before Enable Slot!\n", 0xFF0000);
        goto clear_port_bits;
      }

      // Enable Slot
      xhci_trb_t enable_slot_cmd;
      custom_memset(&enable_slot_cmd, 0, sizeof(xhci_trb_t));
      enable_slot_cmd.control = (TRB_TYPE_ENABLE_SLOT << 10);

      uint32_t completion_code = 0, slot_id = 0;
      if (xhci_send_command(&enable_slot_cmd, &completion_code, &slot_id) ==
          0) {
        kprintf("xHCI: Slot %u allocated for Port %u\n", 0x00FF0000, slot_id,
                port_id);
        xhci_address_device(slot_id, port_id, port_speed);
      } else {
        kprintf("xHCI: Enable Slot failed for Port %u (code %u)\n", 0xFF0000,
                port_id, completion_code);
      }
    }

  clear_port_bits:
    // Clear all R/WC status-change bits (CSC, PEC, WRC, OCC, PRC, PLC, CEC)
    *portsc = (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20) | (1 << 21) |
              (1 << 22) | (1 << 23);
  }
}

// ---------------------------------------------------------------------------
// Address Device
// ---------------------------------------------------------------------------
static void xhci_address_device(uint32_t slot_id, uint32_t port_id,
                                uint32_t port_speed) {
  kprintf("xHCI: Address Device Slot=%u Port=%u\n", 0x00FF0000, slot_id,
          port_id);

  xhci_device_context_t *dev_ctx = &xhci_device_context_pool[slot_id];
  xhci_input_context_t *input_ctx = &xhci_input_context_pool[slot_id];
  custom_memset(dev_ctx, 0, sizeof(*dev_ctx));
  custom_memset(input_ctx, 0, sizeof(*input_ctx));

  // Register device context in DCBAA
  xhci_dcbaap_array[slot_id] = virt_to_phys(dev_ctx);

  // Input Control Context: add Slot (A0) + EP0 (A1)
  input_ctx->add_flags = (1 << 0) | (1 << 1);

  // Slot Context
  // DW0: Context Entries=1, Speed
  input_ctx->slot.dwords[0] = (port_speed << 20) | (1 << 27);
  // DW1: Root Hub Port Number
  input_ctx->slot.dwords[1] = (port_id << 16);

  // EP0 Context (control endpoint)
  uint16_t max_packet_size;
  switch (port_speed) {
  case 1:
    max_packet_size = 64;
    break; // Full-speed
  case 2:
    max_packet_size = 8;
    break; // Low-speed
  case 3:
    max_packet_size = 64;
    break; // High-speed
  case 4:
    max_packet_size = 512;
    break; // SuperSpeed
  case 5:
    max_packet_size = 512;
    break; // SuperSpeed+
  default:
    max_packet_size = 64;
    break;
  }
  kprintf("xHCI: EP0 MaxPacketSize=%u (speed %u)\n", 0x00FF0000,
          max_packet_size, port_speed);

  // DW1: EP Type=Control(4), MaxPacketSize, CErr=3
  input_ctx->eps[0].dwords[1] = (4 << 3) | (max_packet_size << 16) | (3 << 1);

  // DW2/3: TR Dequeue Pointer | DCS
  uint64_t ep0_ring_phys = virt_to_phys(EP_RING(slot_id, 0)) | 1;
  *((uint64_t *)&input_ctx->eps[0].dwords[2]) = ep0_ring_phys;

  // DW4: Average TRB Length
  input_ctx->eps[0].dwords[4] = 8;

  // Send Address Device command
  xhci_trb_t cmd;
  custom_memset(&cmd, 0, sizeof(cmd));
  cmd.parameter = virt_to_phys(input_ctx);
  cmd.control = (slot_id << 24) | (TRB_TYPE_ADDRESS_DEVICE << 10);
  // BSR=0: actually perform SET_ADDRESS on the bus

  uint32_t code = 0;
  if (xhci_send_command(&cmd, &code, NULL) == 0) {
    kprintf("xHCI: Address Device OK Slot=%u\n", 0x00FF0000, slot_id);
    usb_kbd_slot = slot_id;
    usb_kbd_max_packet = max_packet_size;
    usb_kbd_port_speed = port_speed;
    usb_kbd_port_id = port_id;
    nvnode_add_usb_device(0, 0);
    xhci_configure_kbd_endpoint(slot_id, port_speed);
  } else {
    kprintf("xHCI: Address Device FAILED Slot=%u Code=%u\n", 0xFF0000, slot_id,
            code);
  }
}

// ---------------------------------------------------------------------------
// Configure keyboard interrupt endpoint (EP1 IN)
// ---------------------------------------------------------------------------
static void xhci_configure_kbd_endpoint(uint32_t slot_id, uint32_t port_speed) {
  kprintf("xHCI: Configuring EP1 IN for keyboard...\n", 0x00FF0000);

  xhci_input_context_t *input_ctx = &xhci_input_context_pool[slot_id];
  xhci_device_context_t *dev_ctx = &xhci_device_context_pool[slot_id];
  custom_memset(input_ctx, 0, sizeof(*input_ctx));

  // Reset EP1 ring state
  custom_memset(usb_kbd_ep1_ring, 0, 4096);
  usb_kbd_ep1_enqueue = 0;
  usb_kbd_ep1_cycle = 1;

  // Input Control: add Slot (A0) + EP1 IN = DCI 3 (A3)
  input_ctx->add_flags = (1 << 0) | (1 << 3);

  // Carry over slot context; update Context Entries to 3
  input_ctx->slot.dwords[0] = dev_ctx->slot.dwords[0];
  input_ctx->slot.dwords[1] = dev_ctx->slot.dwords[1];
  input_ctx->slot.dwords[0] =
      (input_ctx->slot.dwords[0] & ~(0x1Fu << 27)) | (3u << 27);

  // EP1 IN context sits at eps[2] (DCI 3, index = DCI-1 = 2)
  xhci_endpoint_context_t *ep1 = &input_ctx->eps[2];

  // Interval: per USB spec, HID keyboards use bInterval=10 (ms) for FS/LS,
  // encoded as 2^(Interval-1) for SS/HS (so value=4 → 8ms).
  // For FS/LS the xHCI interval field is in frames (1ms each), so 10.
  uint32_t interval = (port_speed >= 3) ? 4 : 10;

  // DW0: Interval
  ep1->dwords[0] = (interval << 16);
  // DW1: EP Type=Interrupt IN(7), MaxPacketSize=8, CErr=3
  ep1->dwords[1] = (7 << 3) | (8 << 16) | (3 << 1);
  // DW2/3: TR Dequeue Pointer | DCS
  uint64_t ep1_phys = virt_to_phys(usb_kbd_ep1_ring) | 1;
  *((uint64_t *)&ep1->dwords[2]) = ep1_phys;
  // DW4: Average TRB Length
  ep1->dwords[4] = 8;

  xhci_trb_t cmd;
  custom_memset(&cmd, 0, sizeof(cmd));
  cmd.parameter = virt_to_phys(input_ctx);
  cmd.control = (slot_id << 24) | (TRB_TYPE_CONFIGURE_ENDPOINT << 10);

  uint32_t code = 0;
  if (xhci_send_command(&cmd, &code, NULL) == 0) {
    kprintf("xHCI: EP1 configured OK.\n", 0x00FF00);
    usb_kbd_ep1_configured = 1;
    xhci_set_boot_protocol(slot_id);
    xhci_queue_kbd_transfer();
  } else {
    kprintf("xHCI: Configure EP failed. Code=%u\n", 0xFF0000, code);
  }
}

// ---------------------------------------------------------------------------
// SET_PROTOCOL — switch HID keyboard to Boot Protocol
// Uses EP0 transfer ring of the given slot.
// ---------------------------------------------------------------------------
static void xhci_set_boot_protocol(uint32_t slot_id) {
  kprintf("xHCI: SET_PROTOCOL (Boot)...\n", 0x00FF0000);

  xhci_trb_t *ring = EP_RING(slot_id, 0);
  custom_memset(ring, 0, sizeof(xhci_trb_t) * 2);

  // Setup Stage TRB (8-byte USB setup packet packed into parameter field)
  // bmRequestType=0x21, bRequest=0x0B, wValue=0x0000, wIndex=0x0000,
  // wLength=0x0000
  uint64_t setup = (uint64_t)0x21 | ((uint64_t)0x0B << 8) |
                   ((uint64_t)0x00 << 16)    // wValue
                   | ((uint64_t)0x00 << 32)  // wIndex
                   | ((uint64_t)0x00 << 48); // wLength
  ring[0].parameter = setup;
  ring[0].status = 8; // TRB Transfer Length = 8 (setup packet size)
  // IDT=1 (Immediate Data), TRT=0 (No Data Stage), Cycle=1
  ring[0].control = (TRB_TYPE_SETUP_STAGE << 10) | (1 << 6) | 1;

  // Status Stage TRB — direction IN (no data), IOC=1
  ring[1].parameter = 0;
  ring[1].status = 0;
  ring[1].control = (TRB_TYPE_STATUS_STAGE << 10) | (1 << 5) | (1 << 16) | 1;
  //                                                    ^IOC       ^DIR=IN

  // Ring EP0 doorbell (DCI 1)
  xhci_doorbell_regs[slot_id] = 1;

  // Give controller time to process (no proper completion polling here,
  // a real driver should wait for the Transfer Event)
  for (volatile int i = 0; i < 500000; i++)
    ;

  kprintf("xHCI: SET_PROTOCOL sent.\n", 0x00FF00);
}

// ---------------------------------------------------------------------------
// Queue one Normal TRB on the EP1 IN ring to receive a HID report
// ---------------------------------------------------------------------------
static void xhci_queue_kbd_transfer(void) {
  if (!usb_kbd_ep1_configured || !usb_kbd_slot)
    return;

  // Usable slots: leave last one for Link TRB
  int usable = XHCI_EP_RING_SIZE - 1;

  xhci_trb_t *trb = &usb_kbd_ep1_ring[usb_kbd_ep1_enqueue];
  custom_memset(trb, 0, sizeof(*trb));

  trb->parameter = virt_to_phys(usb_kbd_report_data);
  trb->status = 8; // Transfer Length = 8 bytes
  trb->control = (TRB_TYPE_NORMAL << 10) | (1 << 5); // IOC

  if (usb_kbd_ep1_cycle)
    trb->control |= 1;

  usb_kbd_ep1_enqueue++;

  if (usb_kbd_ep1_enqueue >= usable) {
    // Place/update Link TRB at end of ring
    xhci_trb_t *link = &usb_kbd_ep1_ring[usable];
    custom_memset(link, 0, sizeof(*link));
    link->parameter = virt_to_phys(usb_kbd_ep1_ring);
    link->control = (TRB_TYPE_LINK << 10) | (1 << 1); // TC bit
    if (usb_kbd_ep1_cycle)
      link->control |= 1;

    usb_kbd_ep1_enqueue = 0;
    usb_kbd_ep1_cycle = !usb_kbd_ep1_cycle;
  }

  // Ring doorbell for EP1 IN (DCI 3)
  xhci_doorbell_regs[usb_kbd_slot] = 3;
}

// ---------------------------------------------------------------------------
// Poll for keyboard transfer events (call from main input loop)
// ---------------------------------------------------------------------------
extern void usb_kbd_process_report(void *report);

void usb_kbd_poll(void) {
  if (!usb_kbd_ep1_configured || !xhci_op_regs)
    return;

  for (int i = 0; i < 8; i++) {
    xhci_trb_t *ev = &xhci_event_ring[event_ring_dequeue_ptr];
    if ((ev->control & 1) != (uint32_t)event_ring_cycle_state)
      break;

    uint32_t trb_type = (ev->control >> 10) & 0x3F;
    uint32_t completion_code = (ev->status >> 24) & 0xFF;
    uint32_t slot = (ev->control >> 24) & 0xFF;

    event_ring_dequeue_ptr =
        (event_ring_dequeue_ptr + 1) % XHCI_EVENT_RING_SIZE;
    if (event_ring_dequeue_ptr == 0)
      event_ring_cycle_state = !event_ring_cycle_state;

    volatile uint64_t *erdp = (uint64_t *)((uint8_t *)xhci_runtime_regs + 0x38);
    *erdp = virt_to_phys(&xhci_event_ring[event_ring_dequeue_ptr]) | (1 << 3);

    if (trb_type == TRB_TYPE_TRANSFER_EVENT && slot == usb_kbd_slot) {
      if (completion_code == 1) {
        usb_kbd_process_report(usb_kbd_report_data);
      }
      // Requeue regardless of success/failure
      xhci_queue_kbd_transfer();
    }
  }
}