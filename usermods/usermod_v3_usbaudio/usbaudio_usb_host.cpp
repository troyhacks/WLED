// usbaudio_usb_host.cpp
//
// USB Audio (UAC) host client for the ESP32-P4.
//
// NOT a standalone translation unit. Like midi_usb_host.cpp and
// deck_usb_host.cpp, this file is #include'd directly into wled.cpp inside
// `#ifdef USERMOD_USBAUDIO_USB`, and every definition below is `static` so
// there is no ODR risk and nothing gets stranded by -fdata-sections /
// --gc-sections. It must NOT be added to any build_src_filter, and it must
// not gain non-static symbols except for the four the AudioReactive usermod
// needs (uac_usb_* in usbaudio_usb_host.h).
//
// Responsibilities:
//  - Register as an ESP-IDF USB Host client alongside the MIDI, deck and HID
//    clients, and claim ONLY the audio-class interfaces of a device that
//    actually offers an isochronous capture endpoint. Anything else is
//    released immediately, so this client never starves the others.
//  - Walk the configuration descriptor, pick the AudioStreaming alternate
//    setting that carries 48 kHz audio on one or two channels, and note its
//    isochronous IN endpoint.
//  - Stream that endpoint with multi-packet isochronous URBs, two in flight,
//    and push the samples into a ring buffer the AudioReactive usermod reads.
//
// Why this is not a port of somebody else's driver: the ESP-IDF 5.5 USB Host
// stack already has the whole isochronous path (usb_host_transfer_alloc()
// takes a packet count, the DWC HCD builds a circular qTD schedule that runs
// from one URB into the next, and every packet's status comes back
// individually). What is missing is a class client, and that is what this is.
//
// The isochronous rules of this stack, taken from the sources this build
// ships (build_output/usbh.c and build_output/hcd_dwc_v55.c):
//
//  - An isochronous IN transfer must request a whole number of MPS per
//    packet: transfer_check_usb_compliance() rejects an ISO IN whose packet
//    length is not a multiple of the endpoint's wMaxPacketSize. So each
//    packet is requested at MPS and the device sends fewer bytes than that;
//    the real length comes back in isoc_packet_desc[].actual_num_bytes.
//  - The HCD's descriptor list is 64 entries and it refuses a URB whose
//    num_isoc_packets * interval exceeds that. The interval is
//    1 << (bInterval - 1) microframes, so an HS endpoint with bInterval 4
//    (one millisecond) admits at most 8 packets per URB.
//  - The schedule runs on from one URB into the next: the HCD keeps
//    next_start_idx and starts the following URB where the previous one
//    stopped, which is why TWO URBs are kept in flight. With only one, the
//    pipe would sit idle between the end of a URB and the submission of its
//    successor.
//
// Build: -D USERMOD_USBAUDIO_USB on the P4 env (esp32p4_8MB_troyhacks).

#include "wled.h"

#ifdef USERMOD_USBAUDIO_USB

#include "usbaudio_usb_host.h"

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// The ring the samples land in, and the AudioReactive usermod reads from.
// 2048 samples is 42 ms of audio at 48 kHz - enough to ride out a USB
// hiccup, small enough to live in internal RAM (the analysis cannot use
// PSRAM here: the ring is touched by the USB task and the AR task).
#define UAC_RING_SAMPLES   2048
#define UAC_RING_MASK      (UAC_RING_SAMPLES - 1)

// How full the ring is kept. A little above one analysis frame (512), so
// the reader normally finds its samples already there.
#define UAC_RING_TARGET    768

// Packets per isochronous URB. Two packets is two milliseconds at the usual
// 1 ms endpoint interval: small bursts keep the ring's fill smooth, and the
// buffer is only 2 x MPS bytes.
#define UAC_URB_PKTS       2

// The biggest isochronous IN endpoint this client will drive. USB 2.0 caps
// a high-speed isochronous packet at 1024 bytes, and the DWC's receive FIFO
// on this board holds 1272. The transfers are allocated for the maximum once
// and reused, so this is also the size of the buffer.
#define UAC_MAX_MPS        1024

// The control transfer used for the UAC2 sample-rate request: 8 bytes of
// setup packet plus a 32-bit rate.
#define UAC_CTRL_BUF       16

// The class-specific descriptor type (CS_INTERFACE). The IDF's headers do
// not name it: 0x21 is "ADAPTER" in the generic table, but inside a
// configuration descriptor it means "class specific, interface".
#define UAC_DESC_CS_INTERFACE 0x21

#define UAC_LOG(fmt, ...)   do { if (canUseSerial()) Serial.printf("[UAC %6lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// The ring
// ---------------------------------------------------------------------------

// Single producer (the USB task), single consumer (the AR task). Both
// indices are free-running counters; the slot is the counter masked to the
// ring size, and the fill is the difference between them.
static int16_t*         uac_ring      = nullptr;
static volatile uint32_t uac_head      = 0;   // written by the USB task
static volatile uint32_t uac_tail      = 0;   // written by the AR task
static float            uac_phase     = 0.0f; // resampler's fractional position
static float            uac_step      = 1.0f;  // resampler's rate correction

// Woken when the producer has put something in the ring.
static QueueHandle_t    uac_ready     = nullptr;

// Counters.
static volatile uint32_t uac_skipped   = 0;  // ISO packets the controller skipped
static volatile uint32_t uac_overruns  = 0;  // samples dropped: the ring was full
static volatile uint32_t uac_underruns = 0;  // frames padded with silence
static volatile uint32_t uac_urbs      = 0;  // isochronous transfers completed
static volatile uint32_t uac_samples   = 0;  // mono samples delivered so far

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

struct UacDevice {
  usb_device_handle_t dev        = nullptr;
  uint8_t  addr                 = 0;
  uint16_t vid                  = 0;
  uint16_t pid                  = 0;

  uint8_t  as_iface             = 0xFF;  // the AudioStreaming interface
  uint8_t  in_ep                = 0;     // the isochronous IN endpoint
  uint16_t in_mps               = 0;
  uint8_t  in_interval          = 1;

  uint8_t  channels             = 0;
  uint8_t  bits                 = 16;
  uint32_t rate                 = 0;

  bool     uac2                 = false; // UAC2 rather than UAC1
  bool     ready                = false;
};

static UacDevice              uac_dev;
static usb_host_client_handle_t uac_client = nullptr;

// The isochronous URBs. Two, so the schedule runs on from one into the next
// without a gap; see the note at the top of the file.
//
// They are allocated ONCE, for the biggest endpoint this client will drive,
// and reused for every device that turns up. A transfer cannot be freed
// safely while it is in flight - the HCD still points at it, and the
// completion callback still fires - so the teardown detaches the transfers
// (device_handle = nullptr) instead of freeing them, which is what the deck
// client does for the same reason.
static usb_transfer_t*        uac_urb[UAC_URB_PKTS]  = {nullptr};
static usb_transfer_t*        uac_ctrl_xfer          = nullptr;
static volatile bool          uac_urb_armed[UAC_URB_PKTS] = {false};

// A generation counter, bumped every time a device is closed.
//
// The controller calls the completion callback of a transfer that was in
// flight when the device was unplugged, and it does so AFTER the device is
// gone. If a new device has already been claimed by then, that stale
// callback would clear the new device's "armed" flag and the URB would be
// submitted twice. The generation tells the two apart.
static volatile uint32_t      uac_gen        = 0;
static volatile uint32_t      uac_urb_gen[UAC_URB_PKTS] = {0};

// Where the start-up sequence has got to.
enum {
  UAC_STAGE_IDLE = 0,   // nothing to start
  UAC_STAGE_RATE,       // the UAC2 sample rate has to be set first
  UAC_STAGE_ARM         // the URBs can be armed
};
static volatile uint8_t      uac_stage = UAC_STAGE_IDLE;

static volatile bool          uac_ctrl_busy = false;

// True once the sample rate has been asked for, so the request is not sent
// twice while it is still in flight.
static volatile bool          uac_rate_sent = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static const char* uac_speed_str(usb_speed_t s) {
  switch (s) {
    case USB_SPEED_LOW:  return "LOW";
    case USB_SPEED_FULL: return "FULL";
    case USB_SPEED_HIGH: return "HIGH";
    default:             return "?";
  }
}

static const char* uac_xfer_str(uint8_t attr) {
  switch (attr & 0x03) {
    case 0: return "CTRL";
    case 1: return "ISO";
    case 2: return "BULK";
    default: return "INTR";
  }
}

// Push one sample. Returns false when the ring was full (the sample is
// dropped and counted - the alternative is blocking the USB task, which
// would stall every other USB client).
static inline bool uac_ring_push(int16_t v) {
  uint32_t h = uac_head;
  if ((h - uac_tail) >= UAC_RING_SAMPLES) {
    uac_overruns = uac_overruns + 1;
    return false;
  }
  uac_ring[h & UAC_RING_MASK] = v;
  uac_head = h + 1;
  return true;
}

// ---------------------------------------------------------------------------
// The descriptor walk
// ---------------------------------------------------------------------------

// What the walk found for one AudioStreaming alternate setting.
struct UacCandidate {
  bool     valid      = false;
  uint8_t  iface      = 0;
  uint8_t  alt        = 0;
  uint8_t  in_ep      = 0;
  uint16_t mps        = 0;
  uint8_t  mult       = 0;     // the high-bandwidth field of wMaxPacketSize
  uint8_t  interval   = 1;
  uint8_t  channels   = 0;
  uint8_t  bits       = 16;
  uint32_t rate       = 0;
  bool     uac2       = false;
};

// Walk the raw configuration descriptor.
//
// The IDF's helpers find the standard descriptors, but the audio class
// details - the format (channels, bits) and the clock source - live in
// class-specific descriptors that the helpers do not walk, so the raw list
// is walked here. Everything the walk sees is logged: this is the only
// source of truth about what a device actually offers, and guessing has
// already cost this project more time than reading it would have.
static void uac_dump_descriptors(const usb_config_desc_t* cfg) {
  const uint8_t* base = (const uint8_t*)cfg;
  int total = cfg->wTotalLength;
  int off = USB_CONFIG_DESC_SIZE;   // past the configuration header

  uint8_t cur_iface = 0, cur_alt = 0;
  uint8_t cur_class = 0, cur_subclass = 0;

  UAC_LOG("--- configuration descriptor: %d bytes, %d interfaces ---",
          total, cfg->bNumInterfaces);

  while (off + 2 <= total) {
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)(base + off);
    uint8_t len = d->bLength;
    uint8_t type = d->bDescriptorType;
    if (len < 2 || off + len > total) break;

    switch (type) {
      case USB_B_DESCRIPTOR_TYPE_INTERFACE: {
        const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
        cur_iface = i->bInterfaceNumber;
        cur_alt = i->bAlternateSetting;
        cur_class = i->bInterfaceClass;
        cur_subclass = i->bInterfaceSubClass;
        UAC_LOG("  interface %u alt %u class 0x%02X subclass 0x%02X protocol 0x%02X eps %u",
                cur_iface, cur_alt, cur_class, cur_subclass, i->bInterfaceProtocol, i->bNumEndpoints);
        break;
      }
      case USB_B_DESCRIPTOR_TYPE_ENDPOINT: {
        const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
        uint16_t raw = e->wMaxPacketSize;
        UAC_LOG("    ep 0x%02X %-4s mps %u raw 0x%04X interval %u mult %u%s",
                e->bEndpointAddress, uac_xfer_str(e->bmAttributes),
                USB_EP_DESC_GET_MPS(e), raw, e->bInterval,
                USB_EP_DESC_GET_MULT(e),
                USB_EP_DESC_GET_MULT(e) ? "  (HIGH BANDWIDTH - not supported)" : "");
        break;
      }
      case UAC_DESC_CS_INTERFACE: {
        // The class-specific descriptors that matter here:
        //   UAC1 format type (subtype 0x01): bFormatType, bNrChannels,
        //        bSubframeSize, bBitResolution, bmaControls
        //   UAC2 format type (subtype 0x02): wFormatTag, bNrChannels,
        //        bSubframeSize, bBitResolution, bmaControls
        //   UAC2 clock source (subtype 0x0A): bClockID, bClockType,
        //        bmInfo, bmControls
        //
        // The bytes are read through a raw pointer: usb_standard_desc_t is
        // a two-byte union, so its val[] cannot be indexed past 1.
        const uint8_t* raw = (const uint8_t*)d;
        uint8_t sub = (len >= 3) ? raw[2] : 0;
        if (len >= 7 && sub == 0x01 && cur_class == 0x01 && cur_subclass == 0x02) {
          UAC_LOG("    CS_INTERFACE UAC1 format: type %u channels %u subframe %u bits %u controls 0x%02X",
                  raw[3], raw[4], raw[5], raw[6], (len >= 8) ? raw[7] : 0);
        } else if (len >= 8 && sub == 0x02 && cur_class == 0x01 && cur_subclass == 0x02) {
          uint16_t tag = (uint16_t)(raw[3] | (raw[4] << 8));
          UAC_LOG("    CS_INTERFACE UAC2 format: tag 0x%04X channels %u subframe %u bits %u controls 0x%02X",
                  tag, raw[5], raw[6], raw[7], (len >= 9) ? raw[8] : 0);
        } else if (len >= 6 && sub == 0x0A && cur_class == 0x01 && cur_subclass == 0x01) {
          uint8_t bminfo = raw[4];
          UAC_LOG("    CS_INTERFACE UAC2 clock source: id %u type %u info 0x%02X (validity %u) controls 0x%02X",
                  raw[3], raw[4], bminfo, bminfo & 0x03, raw[5]);
        } else {
          UAC_LOG("    CS_INTERFACE subtype 0x%02X, %u bytes", sub, len);
        }
        break;
      }
      default:
        break;
    }
    off += len;
  }
}

// Collect the AudioStreaming candidates: every alternate setting that has an
// isochronous IN endpoint, with the format that belongs to it.
static int uac_collect_candidates(const usb_config_desc_t* cfg, UacCandidate* out, int max) {
  const uint8_t* base = (const uint8_t*)cfg;
  int total = cfg->wTotalLength;
  int off = USB_CONFIG_DESC_SIZE;
  int found = 0;

  uint8_t cur_class = 0, cur_subclass = 0;

  // The candidate the current interface/alternate belongs to.
  int cur = -1;

  while (off + 2 <= total) {
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)(base + off);
    uint8_t len = d->bLength;
    uint8_t type = d->bDescriptorType;
    if (len < 2 || off + len > total) break;

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
      cur_class = i->bInterfaceClass;
      cur_subclass = i->bInterfaceSubClass;
      cur = -1;

      if (cur_class == 0x01 && cur_subclass == 0x02) {   // AudioStreaming
        if (found < max) {
          cur = found++;
          UacCandidate* c = &out[cur];
          c->valid = true;
          c->iface = i->bInterfaceNumber;
          c->alt = i->bAlternateSetting;
          c->channels = 0;
          c->bits = 16;
          c->rate = 0;
        }
      }
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cur >= 0) {
      const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
      // Only an isochronous IN endpoint carries audio towards the host.
      if (USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_ISOCHRONOUS &&
          USB_EP_DESC_GET_EP_DIR(e)) {
        UacCandidate* c = &out[cur];
        if (c->in_ep == 0) {                 // the first ISO IN wins
          c->in_ep = e->bEndpointAddress;
          c->mps = USB_EP_DESC_GET_MPS(e);
          c->mult = USB_EP_DESC_GET_MULT(e);
          c->interval = e->bInterval ? e->bInterval : 1;
        }
      }
    } else if (type == UAC_DESC_CS_INTERFACE && cur >= 0) {
      const uint8_t* raw = (const uint8_t*)d;
      uint8_t sub = (len >= 3) ? raw[2] : 0;
      UacCandidate* c = &out[cur];
      if (sub == 0x01 && len >= 7) {         // UAC1 format type
        c->uac2 = false;
        c->channels = raw[4];
        c->bits = raw[6] ? raw[6] : 16;
      } else if (sub == 0x02 && len >= 8) {  // UAC2 format type
        c->uac2 = true;
        c->channels = raw[5];
        c->bits = raw[7] ? raw[7] : 16;
      }
    }
    off += len;
  }
  return found;
}

// ---------------------------------------------------------------------------
// The UAC2 sample rate
// ---------------------------------------------------------------------------

// A UAC2 device is told what rate to run at; the rate is not in the
// descriptor. The request is a class-specific SET_CUR on the AudioStreaming
// interface's format (control selector 0x0100, control 0x01 - the sample
// rate control) with the rate as a 32-bit value.
static void uac_ctrl_cb(usb_transfer_t* xfer) {
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    UAC_LOG("control: transfer failed: %d", (int)xfer->status);
  }
  uac_ctrl_busy = false;
}

// Issue one SET_CUR. The setup packet goes at the head of the buffer, which
// is how this IDF version does control transfers: there is no separate
// setup field in usb_transfer_t.
static bool uac_set_cur(uint8_t iface, uint32_t value) {
  if (!uac_ctrl_xfer || uac_ctrl_busy) return false;

  usb_setup_packet_t setup = {};
  setup.bmRequestType = 0x21;   // host to device, class, interface
  setup.bRequest      = 0x01;   // SET_CUR
  setup.wValue        = 0x0101; // (control selector << 8) | control selector
  setup.wIndex        = iface;
  setup.wLength       = 4;

  uint8_t* buf = uac_ctrl_xfer->data_buffer;
  memcpy(buf, &setup, sizeof(setup));
  buf[8]  = (uint8_t)(value & 0xFF);
  buf[9]  = (uint8_t)((value >> 8) & 0xFF);
  buf[10] = (uint8_t)((value >> 16) & 0xFF);
  buf[11] = (uint8_t)((value >> 24) & 0xFF);

  uac_ctrl_xfer->device_handle = uac_dev.dev;
  uac_ctrl_xfer->num_bytes = 8 + 4;
  uac_ctrl_xfer->callback = uac_ctrl_cb;
  uac_ctrl_xfer->context = nullptr;

  uac_ctrl_busy = true;
  esp_err_t err = usb_host_transfer_submit_control(uac_client, uac_ctrl_xfer);
  if (err != ESP_OK) {
    uac_ctrl_busy = false;
    UAC_LOG("control: SET_CUR(iface=%u, value=%u) failed: %s",
            iface, value, esp_err_to_name(err));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The isochronous URBs
// ---------------------------------------------------------------------------

// The completion callback. It does nothing but record what happened and
// ask for the URB to be re-armed: a URB may NOT be submitted from inside
// its own completion callback on this stack (the pipe is mid-transition),
// so the submission happens in uac_usb_poll().
static void uac_iso_cb(usb_transfer_t* xfer) {
  // The context carries the URB's index PLUS ONE: index 0 would be a null
  // pointer, and a null context is not a thing this callback should have to
  // think about.
  int idx = (int)(intptr_t)xfer->context - 1;
  if (idx < 0 || idx >= UAC_URB_PKTS) return;

  uac_urbs = uac_urbs + 1;

  // A stale completion - the device this URB belonged to is gone, or a new
  // device has taken over since. Its flag was cleared when the device was
  // closed; touching it now would clear the new device's.
  if (uac_urb_gen[idx] != uac_gen) return;

  uac_urb_armed[idx] = false;

  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    UAC_LOG("iso: URB %d finished with status %d", idx, (int)xfer->status);
    return;
  }

  // Unpack the packets. Packet i's data sits at data_buffer + i * MPS and is
  // isoc_packet_desc[i].actual_num_bytes long - the device sends less than
  // the MPS it advertised, and this is where it says how much it sent.
  const uint8_t* data = xfer->data_buffer;
  int channels = uac_dev.channels ? uac_dev.channels : 1;
  int bytes_per_sample = uac_dev.bits / 8;
  if (bytes_per_sample < 1) bytes_per_sample = 1;
  int frame_bytes = channels * bytes_per_sample;
  if (frame_bytes < 1) frame_bytes = 1;

  for (int i = 0; i < xfer->num_isoc_packets; i++) {
    if (xfer->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) {
      // SKIPPED means the controller could not service the packet in
      // time - that is audio lost, and it is worth counting.
      uac_skipped = uac_skipped + 1;
      continue;
    }
    int got = xfer->isoc_packet_desc[i].actual_num_bytes;
    if (got <= 0) { uac_skipped = uac_skipped + 1; continue; }

    const uint8_t* p = data + (size_t)i * uac_dev.in_mps;
    int frames = got / frame_bytes;
    for (int f = 0; f < frames; f++) {
      const uint8_t* s = p + (size_t)f * frame_bytes;
      int32_t v = 0;
      if (bytes_per_sample == 2) {
        v = (int16_t)(s[0] | (s[1] << 8));
      } else if (bytes_per_sample == 3) {
        v = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16));
        v = (v << 8) >> 8;      // 24 bits, sign extended
      } else if (bytes_per_sample == 4) {
        v = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                      ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24));
        v = v >> 16;            // 32 bits, kept as the top 16
      } else {
        v = (int32_t)(s[0] << 8);
      }
      // The LEFT channel is the one the analysis wants. A stereo stream
      // carries the right channel after it, so the left one is simply the
      // first sample of the frame.
      uac_ring_push((int16_t)v);
    }
    uac_samples += frames;
  }

  if (uac_ready) {
    xQueueSend(uac_ready, nullptr, 0);
  }
}

// Arm one URB. Called from the poll loop, never from the callback.
static bool uac_arm_urb(int idx) {
  if (!uac_urb[idx] || uac_urb_armed[idx] || !uac_dev.ready) return false;

  usb_transfer_t* t = uac_urb[idx];
  t->device_handle = uac_dev.dev;
  t->bEndpointAddress = uac_dev.in_ep;
  t->num_bytes = UAC_URB_PKTS * uac_dev.in_mps;
  for (int i = 0; i < UAC_URB_PKTS; i++) {
    t->isoc_packet_desc[i].num_bytes = uac_dev.in_mps;
    t->isoc_packet_desc[i].actual_num_bytes = 0;
    t->isoc_packet_desc[i].status = USB_TRANSFER_STATUS_COMPLETED;
  }
  t->callback = uac_iso_cb;
  t->context = (void*)(intptr_t)(idx + 1);
  uac_urb_gen[idx] = uac_gen;

  esp_err_t err = usb_host_transfer_submit(t);
  if (err != ESP_OK) {
    UAC_LOG("iso: submit of URB %d failed: %s", idx, esp_err_to_name(err));
    return false;
  }
  uac_urb_armed[idx] = true;
  return true;
}

// ---------------------------------------------------------------------------
// Opening and closing the device
// ---------------------------------------------------------------------------

static void uac_close_device() {
  // Bump the generation first: any completion callback still on its way
  // belongs to the device that is going away, and must not touch the state
  // of the next one.
  uac_gen = uac_gen + 1;

  // The transfers are NOT freed here: they may still be in flight, and the
  // HCD will call their completion callbacks after the device is gone. They
  // are detached instead - the same thing the deck client does.
  for (int i = 0; i < UAC_URB_PKTS; i++) {
    if (uac_urb[i]) uac_urb[i]->device_handle = nullptr;
    uac_urb_armed[i] = false;
  }
  if (uac_ctrl_xfer) uac_ctrl_xfer->device_handle = nullptr;
  uac_ctrl_busy = false;
  uac_rate_sent = false;

  if (uac_dev.dev) {
    if (uac_dev.as_iface != 0xFF) {
      usb_host_interface_release(uac_client, uac_dev.dev, uac_dev.as_iface);
    }
    usb_host_device_close(uac_client, uac_dev.dev);
  }
  uac_dev = UacDevice();
  uac_stage = UAC_STAGE_IDLE;
  uac_head = 0;
  uac_tail = 0;
  uac_phase = 0.0f;
  uac_step = 1.0f;
  UAC_LOG("device closed");
}

// Try to bring a device up as an audio source. Returns true when the
// device has been claimed.
static bool uac_open_and_configure(uint8_t dev_addr) {
  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(uac_client, dev_addr, &dev);
  if (err != ESP_OK) {
    // Somebody else owns it (the MSC client opens every device it sees).
    // Not our business - the MIDI and HID clients have the same problem and
    // simply let the device go - but it is worth saying so: a device that
    // this client CANNOT open is a device we can never claim, and the log
    // is the only place that shows up.
    UAC_LOG("device %u: open failed: %s (another client has it)", dev_addr,
            esp_err_to_name(err));
    return false;
  }

  const usb_device_desc_t* dev_desc = nullptr;
  if (usb_host_get_device_descriptor(dev, &dev_desc) != ESP_OK || !dev_desc) {
    usb_host_device_close(uac_client, dev);
    return false;
  }

  usb_device_info_t dev_info = {};
  if (usb_host_device_info(dev, &dev_info) != ESP_OK) dev_info = {};

  UAC_LOG("device %u: VID=0x%04X PID=0x%04X bcdUSB=%u.%02u class 0x%02X speed %s",
          dev_addr, dev_desc->idVendor, dev_desc->idProduct,
          dev_desc->bcdUSB >> 8, dev_desc->bcdUSB & 0xFF,
          dev_desc->bDeviceClass, uac_speed_str(dev_info.speed));

  const usb_config_desc_t* cfg = nullptr;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !cfg) {
    UAC_LOG("device %u: no configuration descriptor", dev_addr);
    usb_host_device_close(uac_client, dev);
    return false;
  }

  // The dump goes first, and it goes for EVERY device: if the device turns
  // out not to be an audio device, the dump is what says so.
  uac_dump_descriptors(cfg);

  UacCandidate cand[8];
  int n = uac_collect_candidates(cfg, cand, 8);
  if (n == 0) {
    UAC_LOG("device %u: no AudioStreaming interface - not an audio device, releasing", dev_addr);
    usb_host_device_close(uac_client, dev);
    return false;
  }

  // Pick the alternate setting to use.
  //
  // The rules, in order:
  //  - at most two channels. The analysis is mono and takes the LEFT
  //    channel; a surround stream is not something this client can make
  //    sense of, so it is refused rather than half-understood.
  //  - the endpoint must not be a high-bandwidth one. This IDF's USB Host
  //    stack programs one transaction per service period and does not
  //    implement the "additional transactions" field of a high-bandwidth
  //    endpoint, so such an endpoint cannot be driven correctly.
  //  - 48 kHz, because the analysis's band centres are absolute frequencies
  //    and a 44.1 kHz stream would put every band on the wrong axis.
  int best = -1;
  for (int i = 0; i < n; i++) {
    UacCandidate* c = &cand[i];
    if (!c->valid || c->in_ep == 0) continue;
    if (c->channels < 1 || c->channels > 2) {
      UAC_LOG("  alt %u: %u channels - refused (the analysis is mono and takes "
              "the left channel)", c->alt, c->channels);
      continue;
    }
    if (c->mult != 0) {
      UAC_LOG("  alt %u: the endpoint is high-bandwidth (%u extra transactions "
              "per microframe) - refused: this USB Host stack drives one "
              "transaction per service period", c->alt, c->mult);
      continue;
    }
    if (c->mps > UAC_MAX_MPS) {
      UAC_LOG("  alt %u: the endpoint's maximum packet is %u bytes - refused "
              "(the limit here is %u)", c->alt, c->mps, UAC_MAX_MPS);
      continue;
    }
    best = i;
    break;
  }
  if (best < 0) {
    UAC_LOG("device %u: no usable alternate setting - releasing", dev_addr);
    usb_host_device_close(uac_client, dev);
    return false;
  }

  UacCandidate* c = &cand[best];

  // The rate.
  //
  // UAC1 does not put the rate in the descriptor; it is implied by the
  // endpoint's packet size and interval together with the format: the
  // device sends wMaxPacketSize bytes every bInterval milliseconds.
  //
  //   rate = wMaxPacketSize * 1000 / (bInterval * bytes per frame)
  //
  // UAC2 has no implied rate at all - the host sets it.
  int bytes_per_sample = c->bits / 8;
  if (bytes_per_sample < 1) bytes_per_sample = 1;
  int frame_bytes = c->channels * bytes_per_sample;
  if (frame_bytes < 1) frame_bytes = 1;

  if (c->uac2) {
    c->rate = 48000;
  } else {
    c->rate = (uint32_t)((uint64_t)c->mps * 1000ULL /
                          ((uint64_t)c->interval * (uint64_t)frame_bytes));
  }

  UAC_LOG("  chosen: interface %u alt %u ep 0x%02X mps %u interval %u -> %u ch, %u bit, %u Hz%s",
          c->iface, c->alt, c->in_ep, c->mps, c->interval,
          c->channels, c->bits, c->rate, c->uac2 ? " (UAC2)" : "");

  if (c->rate != 48000) {
    UAC_LOG("  the device offers %u Hz, not 48000 Hz - refused: the analysis' "
            "band centres are absolute frequencies", c->rate);
    usb_host_device_close(uac_client, dev);
    return false;
  }

  // Claim the interface.
  err = usb_host_interface_claim(uac_client, dev, c->iface, c->alt);
  if (err != ESP_OK) {
    UAC_LOG("  interface_claim(iface=%u, alt=%u) failed: %s",
            c->iface, c->alt, esp_err_to_name(err));
    usb_host_device_close(uac_client, dev);
    return false;
  }

  uac_dev.dev = dev;
  uac_dev.addr = dev_addr;
  uac_dev.vid = dev_desc->idVendor;
  uac_dev.pid = dev_desc->idProduct;
  uac_dev.as_iface = c->iface;
  uac_dev.in_ep = c->in_ep;
  uac_dev.in_mps = c->mps;
  uac_dev.in_interval = c->interval;
  uac_dev.channels = c->channels;
  uac_dev.bits = c->bits;
  uac_dev.rate = c->rate;
  uac_dev.uac2 = c->uac2;
  uac_dev.ready = true;

  // The stream starts from the poll loop, not from here: a URB may not be
  // submitted while the pipe is still handing over the enumeration's
  // transfers. A UAC2 device is told its rate first.
  uac_stage = uac_dev.uac2 ? UAC_STAGE_RATE : UAC_STAGE_ARM;

  return true;
}

// ---------------------------------------------------------------------------
// The client
// ---------------------------------------------------------------------------

static void uac_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg) {
  (void)arg;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    // Every device is announced, whatever it is. The line says so, because
    // "the device did not appear in the log" and "the device appeared and
    // was refused" are very different problems.
    UAC_LOG("a device appeared at address %u", msg->new_dev.address);
    uac_open_and_configure(msg->new_dev.address);
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (uac_dev.dev && uac_dev.dev == msg->dev_gone.dev_hdl) {
      UAC_LOG("the audio device went away");
      uac_close_device();
    }
  }
}

void uac_usb_init(void) {
  if (!uac_ring) {
    // Internal RAM: the USB task writes it and the AR task reads it, and
    // the samples are on the hot path.
    uac_ring = (int16_t*)heap_caps_malloc(UAC_RING_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!uac_ring) {
      UAC_LOG("the sample ring could not be allocated - no USB audio");
      return;
    }
  }
  if (!uac_ready) {
    uac_ready = xQueueCreate(1, sizeof(uint8_t));
    if (!uac_ready) {
      UAC_LOG("the wake-up queue could not be created - no USB audio");
      return;
    }
  }

  // The isochronous URBs, allocated once for the biggest endpoint this
  // client will drive and reused for every device. The buffer is internal
  // DMA memory: the DWC cannot read PSRAM on this build
  // (CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM is off), and
  // usb_host_transfer_alloc() picks the capabilities itself.
  size_t buf_bytes = (size_t)UAC_URB_PKTS * UAC_MAX_MPS;
  for (int i = 0; i < UAC_URB_PKTS; i++) {
    if (usb_host_transfer_alloc(buf_bytes, UAC_URB_PKTS, &uac_urb[i]) != ESP_OK) {
      UAC_LOG("the isochronous transfers could not be allocated - no USB audio");
      uac_urb[i] = nullptr;
      return;
    }
  }
  if (usb_host_transfer_alloc(UAC_CTRL_BUF, 0, &uac_ctrl_xfer) != ESP_OK) {
    UAC_LOG("the control transfer could not be allocated - a UAC2 device will "
            "keep its default rate");
    uac_ctrl_xfer = nullptr;
  }

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;      // "Set this to false for now" on this IDF
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = uac_client_event_cb;
  cfg.async.callback_arg = nullptr;
  esp_err_t err = usb_host_client_register(&cfg, &uac_client);
  if (err != ESP_OK) {
    UAC_LOG("client registration failed: %s", esp_err_to_name(err));
    uac_client = nullptr;
    return;
  }
  UAC_LOG("client ready");
}

void uac_usb_poll(void) {
  if (!uac_client) return;
  usb_host_client_handle_events(uac_client, 0);

  // What does the host actually see?
  //
  // The client only ever hears about a device through its NEW_DEV event,
  // and an event can be missed - a device that enumerated before this client
  // registered, or one the enumeration filter dropped. The address list is
  // the host's own view of what is plugged in, so it is the way to tell
  // "the device never reached the bus" apart from "the device reached the
  // bus and this client did not hear about it".
  //
  // This runs whether or not an audio device has been claimed: the answer
  // is most interesting when there is none.
  {
    static uint32_t lastSurvey = 0;
    uint32_t now = millis();
    if (now - lastSurvey >= 2000) {
      lastSurvey = now;
      uint8_t addrs[8];
      int num = 0;
      if (usb_host_device_addr_list_fill(8, addrs, &num) == ESP_OK && num > 0) {
        UAC_LOG("the host sees %d device(s):", num);
        for (int i = 0; i < num; i++) {
          UAC_LOG("   address %u", addrs[i]);
        }
      } else {
        UAC_LOG("the host sees no devices at all");
      }
    }
  }

  if (!uac_dev.ready) return;

  // The start-up sequence: a UAC2 device is told its rate first, and the
  // stream only starts once that request has been answered - otherwise the
  // first packets would arrive at the device's default rate.
  if (uac_stage == UAC_STAGE_RATE) {
    if (!uac_rate_sent) {
      uac_rate_sent = true;
      if (uac_ctrl_xfer && !uac_set_cur(uac_dev.as_iface, uac_dev.rate)) {
        UAC_LOG("  the device did not take the sample rate - it may still be "
                "at its default");
      }
      return;                             // wait for the request to land
    }
    if (uac_ctrl_busy) return;            // the request is still on its way
    uac_stage = UAC_STAGE_ARM;
  }

  // Arm the URBs that have finished. This is deliberately NOT done in the
  // completion callback: a URB may not be submitted while the pipe is still
  // handing the previous one over.
  for (int i = 0; i < UAC_URB_PKTS; i++) {
    if (!uac_urb_armed[i]) uac_arm_urb(i);
  }

  // Cross-check the rate the descriptor implied against the rate the samples
  // actually arrive at.
  //
  // A UAC1 device does not put its sample rate in the descriptor: it is
  // implied by the endpoint's packet size and interval, and that inference
  // is only as good as the device filling its packets exactly. So the rate
  // is measured, and if the two disagree the bench gets told rather than
  // being left with a spectrum on the wrong frequency axis.
  {
    static uint32_t lastMs = 0, lastSamples = 0;
    uint32_t now = millis();
    if (lastMs == 0) { lastMs = now; lastSamples = uac_samples; }
    else if ((now - lastMs) >= 2000) {
      uint32_t ds = uac_samples - lastSamples;
      uint32_t dt = now - lastMs;
      uint32_t measured = (uint32_t)((uint64_t)ds * 1000ULL / (dt ? dt : 1));
      int diff = (int)measured - (int)uac_dev.rate;
      if (diff < 0) diff = -diff;
      if (measured && uac_dev.rate && (uint32_t)diff * 200ULL > uac_dev.rate) {
        UAC_LOG("the samples are arriving at %u Hz, not the %u Hz the endpoint "
                "implies - the spectrum would be on the wrong axis",
                measured, uac_dev.rate);
      }
      lastMs = now;
      lastSamples = uac_samples;
    }
  }
}

// ---------------------------------------------------------------------------
// The seam towards the AudioReactive usermod
// ---------------------------------------------------------------------------

bool uac_usb_connected(void) {
  return uac_dev.ready;
}

uint32_t uac_usb_sample_rate(void) {
  return uac_dev.ready ? uac_dev.rate : 0;
}

uint32_t uac_usb_channels(void) {
  return uac_dev.ready ? uac_dev.channels : 0;
}

uint32_t uac_usb_bits_per_sample(void) {
  return uac_dev.ready ? uac_dev.bits : 0;
}

// Read one analysis frame's worth of samples.
//
// The reader takes exactly `n` samples out of the stream and hands back
// exactly `n` - the analysis assumes each frame is hopSamples of audio, and
// the tempo estimator works in frames rather than in wall-clock time, so
// this has to be exact.
//
// The two clocks differ: the device's sample clock and the board's own frame
// clock are different crystals, typically 100 ppm apart. Left alone, the
// ring would fill or empty within a quarter of an hour. So the reader
// interpolates and the step is trimmed to hold the ring near its target -
// a level that is too high means the device is running fast, and the step
// then takes slightly more input per output to catch up.
bool uac_usb_read_samples(float* dst, uint16_t n, uint32_t timeout_ms) {
  if (!dst || n == 0) return false;

  if (!uac_dev.ready || !uac_ring) {
    // No device (or the client has not been set up yet - the USB task and
    // the analysis task start independently). The frame is still paced, so
    // the analysis keeps its cadence and the tempo estimator does not run
    // away on an empty stream.
    uint32_t frame_ms = (uint32_t)((uint64_t)n * 1000ULL / 48000ULL);
    if (frame_ms < 1) frame_ms = 1;
    vTaskDelay(pdMS_TO_TICKS(frame_ms));
    memset(dst, 0, sizeof(float) * n);
    return false;
  }

  // Wait for the frame's worth of samples. The producer wakes this task as
  // soon as it has put something in the ring.
  uint32_t deadline = millis() + timeout_ms;
  while ((uac_head - uac_tail) < n) {
    if (!uac_dev.ready) break;
    if ((int32_t)(millis() - deadline) >= 0) break;
    if (uac_ready) xQueueReceive(uac_ready, nullptr, pdMS_TO_TICKS(1));
    else vTaskDelay(1);
  }

  // Produce the frame.
  uint32_t t = uac_tail;
  uint32_t h = uac_head;
  float step = uac_step;
  uint16_t produced = 0;

  for (uint16_t i = 0; i < n; i++) {
    if (t >= h) {
      // The device stopped talking. The rest of the frame is silence.
      memset(dst + i, 0, sizeof(float) * (n - i));
      uac_underruns = uac_underruns + 1;
      break;
    }
    int16_t s0 = uac_ring[t & UAC_RING_MASK];
    int16_t s1 = (t + 1 < h) ? uac_ring[(t + 1) & UAC_RING_MASK] : s0;
    dst[i] = (float)s0 / 32768.0f;
    if (uac_phase > 0.0f) {
      dst[i] += uac_phase * ((float)s1 - (float)s0) / 32768.0f;
    }
    produced++;

    uac_phase += step;
    while (uac_phase >= 1.0f) {
      uac_phase -= 1.0f;
      t++;
    }
  }
  uac_tail = t;

  // Trim the step so the ring stays near its target. One sample of error
  // is one part per million, which is the right order for two crystals.
  int32_t level = (int32_t)(uac_head - uac_tail);
  float err = (float)(level - (int32_t)UAC_RING_TARGET);
  step = 1.0f + err * 1e-6f;
  if (step < 0.990f) step = 0.990f;
  if (step > 1.010f) step = 1.010f;
  uac_step = step;

  return produced == n;
}

void uac_usb_get_stats(uint32_t* skipped, uint32_t* overruns, uint32_t* underruns, uint32_t* urbs) {
  if (skipped)   *skipped   = uac_skipped;
  if (overruns)  *overruns  = uac_overruns;
  if (underruns) *underruns = uac_underruns;
  if (urbs)      *urbs      = uac_urbs;
}

#endif // USERMOD_USBAUDIO_USB
