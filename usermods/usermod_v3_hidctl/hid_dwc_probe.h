// hid_dwc_probe.h
//
// A look inside the DWC2 host controller, for the deck's interrupt IN pipe.
//
// The pipe has never completed a transfer: not a report, not a
// zero-length packet, not an error. The core is polling the endpoint and
// the device is saying nothing, or the core is not polling at all, and
// from the host's side the two look the same. These are the registers
// that tell them apart.
//
// Everything is read raw. The driver's own accessors for the interrupt
// registers WRITE the register back as they read it (to clear the
// interrupt), and a diagnostic that cleared the interrupt the host's ISR
// was waiting for would be worse than useless.
//
// The controller is at 0x50000000 (USB_DWC_HS, the high-speed block;
// the full-speed one is at 0x50040000 and our host drives the HS one -
// peripheral_map = BIT(0)).
//
//   global registers
//     0x0014  GINTSTS   the host controller's interrupts
//     0x0024  GRXFSIZ   the RX FIFO's size, in four-byte lines
//     0x0028  GNPTXFSIZ the non-periodic TX FIFO
//     0x0100  HPTXFSIZ  the periodic TX FIFO
//     0x0400  HCFG
//     0x0408  HFNUM     the frame number
//     0x0414  HAINT     the channels with an interrupt pending
//     0x041C  HFLBADDR  where the periodic frame list is
//     0x0440  HPRT
//
//   host channel n, at 0x0500 + 0x20 * n
//     +0x00  HCCHAR  the endpoint, its type and its MPS
//     +0x08  HCINT   what happened on it
//     +0x10  HCTSIZ  the packet count, the size and the PID
//     +0x14  HCDMA   where its transfer descriptors are
//
// The frame list is 32 entries of 32 bits: the bit for a channel in an
// entry says "poll this channel in this frame". A channel that is not in
// ANY of them is never polled - which is the whole question.

#pragma once

#include <stdint.h>
#include "soc/usb_dwc_struct.h"
#include "esp_cache.h"

#define HIDCTL_DWC_FRAME_LIST_LEN 32

// One channel's state.
struct HidctlDwcChannel {
  uint32_t hcchar = 0;    // the endpoint, its type, its MPS, is it enabled
  uint32_t hcint  = 0;    // what happened on it
  uint32_t hctsiz = 0;    // the packet count, the size, the PID
  uint32_t hcdma  = 0;    // where its transfer descriptors are
  uint8_t  frames = 0;    // how many frames of the list poll it
  uint32_t qtd_status = 0;  // the descriptor's own status word
  uint32_t qtd_buffer = 0;  // where the descriptor says the data goes
  uint32_t qtd_fields = 0;  // the descriptor's own fields (the size)
  uint16_t qtd_remaining = 0;  // the bytes it has left
  // The fields of HCCHAR, decoded.
  uint16_t mps = 0;
  uint8_t  epnum = 0;
  uint8_t  epdir = 0;
  uint8_t  eptype = 0;
  uint8_t  devaddr = 0;
  uint8_t  chena = 0;
  // The fields of HCTSIZ, decoded.
  uint32_t xfersize = 0;
  uint32_t pktcnt = 0;
  uint8_t  pid = 0;
};

// The state of the controller.
struct HidctlDwcProbe {
  bool     ok = false;
  uint32_t grxfsiz = 0;      // the RX FIFO, in lines
  uint32_t gnptxfsiz = 0;
  uint32_t hptxfsiz = 0;
  uint32_t gintsts = 0;
  uint32_t hcfg = 0;
  uint32_t hfnum = 0;
  uint32_t haint = 0;
  uint32_t hflbaddr = 0;
  uint32_t frame_list[4] = {0, 0, 0, 0};   // the first four entries
  uint8_t  channel_count = 0;
  HidctlDwcChannel channels[16];
};

// Read the controller's registers.
//
// The channel list is found by looking for the channels that are
// enabled, and each one's qTD is read raw: the driver's own accessor
// for the qTD's status ERASES the descriptor the transfer is running on,
// which would break the transfer being diagnosed.
inline void hidctlDwcSnapshot(HidctlDwcProbe& out) {
  memset(&out, 0, sizeof(out));
  volatile usb_dwc_dev_t* hw = &USB_DWC_HS;
  if (!hw) return;
  out.ok = true;

  out.grxfsiz    = hw->grxfsiz_reg.val;
  out.gnptxfsiz  = hw->gnptxfsiz_reg.val;
  out.hptxfsiz   = hw->hptxfsiz_reg.val;
  out.gintsts    = hw->gintsts_reg.val;      // read, not read-and-clear
  out.hcfg       = hw->hcfg_reg.val;
  out.hfnum      = hw->hfnum_reg.val;
  out.haint      = hw->haint_reg.val;
  out.hflbaddr   = hw->hflbaddr_reg.hflbaddr;

  // The frame list: where the periodic channels are polled.
  const uint32_t* fl = (const uint32_t*)(uintptr_t)out.hflbaddr;
  if (out.hflbaddr) {
    for (uint8_t i = 0; i < 4; i++) out.frame_list[i] = fl[i];
  }

  for (uint8_t c = 0; c < 16; c++) {
    volatile usb_dwc_host_chan_regs_t* ch = &hw->host_chans[c];
    HidctlDwcChannel& d = out.channels[c];
    d.hcchar = ch->hcchar_reg.val;
    d.hcint  = ch->hcint_reg.val;
    d.hctsiz = ch->hctsiz_reg.val;
    d.hcdma  = ch->hcdma_reg.val;
    // HCCHAR, decoded.
    d.mps     = (uint16_t)(d.hcchar & 0x7FF);
    d.epnum   = (uint8_t)((d.hcchar >> 11) & 0xF);
    d.epdir   = (uint8_t)((d.hcchar >> 15) & 1);
    d.eptype  = (uint8_t)((d.hcchar >> 18) & 3);
    d.devaddr = (uint8_t)((d.hcchar >> 22) & 0x7F);
    d.chena   = (uint8_t)((d.hcchar >> 31) & 1);
    // HCTSIZ, decoded.
    d.xfersize = (d.hctsiz & 0x7FFFF);
    d.pktcnt   = (uint32_t)((d.hctsiz >> 19) & 0x3FF);
    d.pid      = (uint8_t)((d.hctsiz >> 29) & 3);
    if (!d.chena) continue;                 // the channel is off
    out.channel_count++;

    // How many frames of the list poll this channel?
    if (fl) {
      for (uint8_t i = 0; i < HIDCTL_DWC_FRAME_LIST_LEN; i++) {
        if (fl[i] & (1u << c)) d.frames++;
      }
    }

    // The transfer descriptor: its status word says whether it was ever
    // executed and how much is left of it.
    //
    // It is in DMA memory and the DMA wrote the status word, so it is read
    // through the cache - and the cache is already right: the host driver
    // invalidates the descriptor list itself after every transfer
    // (cache_sync_xfer_descriptor_list() with mem_to_cache, in the IDF's
    // hcd_dwc.c).  Nothing is synced here on purpose:
    //
    // An invalidate from here lands in the middle of the driver writing
    // that very qTD - the setup fields are still dirty in the cache, and
    // invalidating the line throws them away before they are written back.
    // The descriptor then points at nothing, the transfer never completes,
    // and the OUT pipe is wedged for good.  It happened: the pipe stopped
    // the moment anything read /json/ar and the probe ran.
    const uint32_t qtd_addr = d.hcdma & 0xFFFFFE00;
    if (qtd_addr) {
      uint32_t* q = (uint32_t*)(uintptr_t)qtd_addr;
      // The qTD: the buffer pointer, then the status word, then the
      // descriptor's own fields (the size among them).
      // The qTD on this SoC: the first word is the status (the bytes
      // still outstanding in bits 0..20, the HALT flag in bit 31) and
      // the second is the buffer the data goes into.
      d.qtd_status = q[0];
      d.qtd_buffer = q[1];
      d.qtd_fields = q[2];
      d.qtd_remaining = (uint16_t)(d.qtd_status & 0x1FFFF);
    }
  }
}

// Write the data toggle (the PID) of the interrupt IN channel for an
// endpoint.
//
// The driver only ever writes the PID for CONTROL pipes, so a pipe that
// has never transferred keeps whatever the channel had. If the device's
// toggle is out of step with the host's, the device ignores every token
// and nothing ever arrives - and there is no other way to change it.
// The channel has to be disabled for the write to take effect, which for
// a periodic channel means stopping the core from polling it.
inline bool hidctlDwcSetInPid(uint8_t addr, uint8_t epnum, uint8_t pid) {
  volatile usb_dwc_dev_t* hw = &USB_DWC_HS;
  for (uint8_t c = 0; c < 16; c++) {
    volatile usb_dwc_host_chan_regs_t* ch = &hw->host_chans[c];
    const uint32_t hcchar = ch->hcchar_reg.val;
    const uint8_t caddr = (uint8_t)((hcchar >> 22) & 0x7F);
    const uint8_t cep   = (uint8_t)((hcchar >> 11) & 0xF);
    const uint8_t cdir  = (uint8_t)((hcchar >> 15) & 1);
    const uint8_t ctype = (uint8_t)((hcchar >> 18) & 3);
    if (caddr != addr || cep != epnum || cdir != 1) continue;
    // The DWC2's endpoint type: 0 = control, 1 = isochronous,
    // 2 = bulk, 3 = interrupt.
    if (ctype != 3) continue;
    ch->hctsiz_reg.pid = pid & 3;
    return true;
  }
  return false;
}
