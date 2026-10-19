// deck_usb_host.cpp
// USB Host client for the Soomfon CN003 / Mirabox N4-family deck on ESP32-P4.
//
// NOT a standalone translation unit. Like midi_usb_host.cpp, this file is
// #include'd directly into wled.cpp inside `#ifdef USERMOD_DECK_USB`, and
// every definition below is `static` so there is no ODR risk and nothing gets
// stranded by -fdata-sections / --gc-sections. It must NOT be added to any
// build_src_filter, and it must not gain non-static symbols.
//
// Responsibilities:
//  - Register as an ESP-IDF USB Host client alongside the MIDI client, with a
//    VID/PID filter so the two never fight over the same device.
//  - Walk the configuration descriptor, find the vendor-class HID interface
//    (usage page 0xFF00) and claim its interrupt IN/OUT endpoints.
//  - Parse 512-byte input reports and post button/encoder events to the deck
//    usermod via the app queue.
//  - Drain an OUT ring buffer of complete protocol packets and submit them as
//    interrupt-OUT transfers, plus a periodic CONNECT keepalive.
//
// Build: -D USERMOD_DECK_USB on the P4 env (esp32p4_8MB_troyhacks).
// Phase 0: transport bring-up. Input parsing is intentionally permissive and
// everything is logged so the real report layout can be confirmed on hardware
// before the usermod relies on any field beyond index/state.

#include "wled.h"

#ifdef USERMOD_DECK_USB

#include "deck_protocol.h"
#include "../usermods/usermod_v3_midi/usb_host_messages.h"

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "freertos/ringbuf.h"

#define DECK_LOG(fmt, ...)   do { if (canUseSerial()) Serial.printf("[DECK] " fmt "\n", ##__VA_ARGS__); } while (0)
#define DECK_DEBUG(fmt, ...) DEBUG_PRINTF("[DECK] " fmt "\n", ##__VA_ARGS__)

// Set to 1 to dump the full configuration descriptor on connect. Off by
// default — heavy logging inside the brief CONFIGURED window has previously
// broken transfer delivery on this P4 EV board.
#ifndef DECK_DUMP_DESCRIPTORS
#define DECK_DUMP_DESCRIPTORS 0
#endif

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------

// Soomfon CN003 — the unit this was written against.
static constexpr uint16_t SOOMFON_VID = 0x1500;
static constexpr uint16_t CN003_PID   = 0x3002;

// The rest of the N4 family, for reference. Only the CN003 is claimed today;
// add entries here once the descriptor walk is confirmed good enough for each.
struct KnownDevice { uint16_t vid, pid; const char* name; };
static const KnownDevice kKnownDevices[] = {
  { 0x1500, 0x3002, "Soomfon CN003" },
  { 0x1500, 0x3007, "Streonor S10 Pro" },
  { 0x0300, 0x3006, "Ajazz AKP05" },
  { 0x0300, 0x3004, "Ajazz AKP05E" },
  { 0x6602, 0x1001, "Mirabox N4" },
  { 0x6603, 0x1007, "Mirabox N4E" },
  { 0x5548, 0x1008, "Mirabox N4 Pro" },
  { 0x5548, 0x1021, "Mirabox N4 Pro E" },
};

static const char* device_lookup(uint16_t vid, uint16_t pid) {
  for (const auto& d : kKnownDevices) {
    if (d.vid == vid && d.pid == pid) return d.name;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static constexpr uint16_t IN_XFER_SIZE  = DeckProto::IN_REPORT_LEN;   // 512
static constexpr uint16_t OUT_XFER_SIZE = DeckProto::MAX_OUT_BYTES;   // > one image chunk

// OUT ring buffer. Sized for a full 15-key image push: 15 BAT headers plus
// their chunk streams plus a trailing STP comfortably exceeds 8 KB, so a full
// repaint can be queued in one go without blocking the caller.
static constexpr uint32_t OUT_RB_BYTES = 16384;
static uint8_t           deck_out_rb_storage[OUT_RB_BYTES] __attribute__((aligned(4)));
static StaticRingbuffer_t deck_out_rb_struct;
static RingbufHandle_t    deck_out_rb = nullptr;

// The device drops the session if CONNECT is not seen roughly every 10 s.
static constexpr uint32_t KEEPALIVE_MS = 9000;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static usb_host_client_handle_t deck_client  = nullptr;
static usb_device_handle_t     deck_dev_hdl = nullptr;
static uint8_t  deck_in_ep  = 0;
static uint8_t  deck_out_ep = 0;
static uint8_t  deck_iface  = 0;
static uint16_t deck_vid_v  = 0;
static uint16_t deck_pid_v  = 0;
static char     deck_name[24] = {0};

static usb_transfer_t* deck_in_xfer  = nullptr;
static usb_transfer_t* deck_out_xfer = nullptr;

// Set from the IN callback, acted on in deck_usb_poll(). Re-submitting from
// inside the callback runs within usb_host_client_handle_events(), where the
// DWC pipe may not yet accept a new URB.
static volatile bool in_resubmit_needed  = false;
// Set only when a transfer did NOT complete successfully. On a healthy URB the
// pipe's data toggle has advanced and must be left alone; clearing it would
// desync host and device and silently drop every following packet.
static volatile bool in_error_recovery_needed = false;

static volatile bool out_in_flight = false;
static uint32_t     last_keepalive_ms = 0;

// Forward declarations
static void deck_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void deck_in_transfer_cb(usb_transfer_t* xfer);
static void deck_out_transfer_cb(usb_transfer_t* xfer);
static esp_err_t deck_open_and_configure(uint8_t dev_addr);
static void      deck_close_device(void);
static void      deck_send_keepalive(void);

// ---------------------------------------------------------------------------
// deck_usb_init — called once from usb_task in wled.cpp
// ---------------------------------------------------------------------------
static void deck_usb_init(void) {
  if (deck_client) return;

  deck_out_rb = xRingbufferCreateStatic(OUT_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                        deck_out_rb_storage, &deck_out_rb_struct);
  if (!deck_out_rb) {
    DECK_LOG("init: OUT ring buffer alloc failed");
    return;
  }

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = deck_client_event_cb;
  cfg.async.callback_arg = nullptr;

  esp_err_t err = usb_host_client_register(&cfg, &deck_client);
  if (err != ESP_OK) {
    DECK_LOG("init: usb_host_client_register failed: %s", esp_err_to_name(err));
    deck_client = nullptr;
    return;
  }

  err = usb_host_transfer_alloc(IN_XFER_SIZE, 0, &deck_in_xfer);
  if (err != ESP_OK) {
    DECK_LOG("init: IN transfer alloc failed: %s", esp_err_to_name(err));
    deck_in_xfer = nullptr;
  } else {
    deck_in_xfer->callback = deck_in_transfer_cb;
    deck_in_xfer->context = nullptr;
  }

  err = usb_host_transfer_alloc(OUT_XFER_SIZE, 0, &deck_out_xfer);
  if (err != ESP_OK) {
    DECK_LOG("init: OUT transfer alloc failed: %s", esp_err_to_name(err));
    deck_out_xfer = nullptr;
  } else {
    deck_out_xfer->callback = deck_out_transfer_cb;
    deck_out_xfer->context = nullptr;
  }

  DECK_LOG("init: USB Host client ready");
}

// ---------------------------------------------------------------------------
// deck_usb_poll
// ---------------------------------------------------------------------------
static void deck_usb_poll(void) {
  if (!deck_client) return;

  usb_host_client_handle_events(deck_client, 0);

  // Re-submit a completed IN transfer here rather than in the callback, so the
  // library has fully settled the pipe state first.
  if (in_resubmit_needed && deck_dev_hdl && deck_in_xfer) {
    in_resubmit_needed = false;
    usb_transfer_t* x = deck_in_xfer;
    if (x->device_handle) {
      if (in_error_recovery_needed) {
        // The pipe is HALTED after an error and the toggle has already been
        // reset, so this is the only case where a flush is safe.
        usb_host_endpoint_halt(x->device_handle, x->bEndpointAddress);
        usb_host_endpoint_flush(x->device_handle, x->bEndpointAddress);
        usb_host_endpoint_clear(x->device_handle, x->bEndpointAddress);
        in_error_recovery_needed = false;
      }
      x->num_bytes = IN_XFER_SIZE;
      esp_err_t err = usb_host_transfer_submit(x);
      if (err != ESP_OK) {
        in_resubmit_needed = true;
        in_error_recovery_needed = true;
        DECK_DEBUG("IN re-submit failed: %s", esp_err_to_name(err));
      }
    }
  }

  // Keepalive: the device times out and drops the session without it.
  if (deck_dev_hdl && deck_out_ep) {
    uint32_t now = millis();
    if ((uint32_t)(now - last_keepalive_ms) >= KEEPALIVE_MS) {
      last_keepalive_ms = now;
      deck_send_keepalive();
    }
  }

  // OUT path: drain the ring buffer into a single in-flight transfer. ESP-IDF
  // permits only one URB per endpoint at a time, so the completion callback
  // releases the slot for the next one.
  if (deck_dev_hdl && deck_out_ep && deck_out_xfer && !out_in_flight && deck_out_rb) {
    size_t got = 0;
    const uint8_t* data = (const uint8_t*)xRingbufferReceiveUpTo(
      deck_out_rb, &got, 0, OUT_XFER_SIZE);
    if (data) {
      // Packs shorter than a full image chunk are still sent at their true
      // length; only image chunks pad out to the full wire size.
      memcpy(deck_out_xfer->data_buffer, data, got);
      vRingbufferReturnItem(deck_out_rb, (void*)data);
      deck_out_xfer->num_bytes = got;
      deck_out_xfer->device_handle = deck_dev_hdl;
      deck_out_xfer->bEndpointAddress = deck_out_ep;
      out_in_flight = true;
      esp_err_t err = usb_host_transfer_submit(deck_out_xfer);
      if (err != ESP_OK) {
        out_in_flight = false;
        DECK_DEBUG("OUT submit failed: %s", esp_err_to_name(err));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// deck_out_queue
// ---------------------------------------------------------------------------
static bool deck_out_queue(const uint8_t* data, uint16_t len) {
  if (!deck_dev_hdl || !deck_out_ep || !deck_out_rb || !data || !len) return false;
  BaseType_t ok = xRingbufferSend(deck_out_rb, data, len, 0);
  if (ok != pdTRUE) {
    DECK_DEBUG("OUT queue full, dropped %u bytes", len);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Query helpers (used by the usermod in later phases, and by /json/info)
// ---------------------------------------------------------------------------
static bool deck_is_connected(void) { return deck_dev_hdl != nullptr; }
static uint16_t deck_vid(void) { return deck_vid_v; }
static uint16_t deck_pid(void) { return deck_pid_v; }
static const char* deck_device_name(void) { return deck_name[0] ? deck_name : "disconnected"; }

// ---------------------------------------------------------------------------
// Keepalive
// ---------------------------------------------------------------------------
static void deck_send_keepalive(void) {
  uint8_t buf[DeckProto::MAX_CMD_LEN];
  size_t n = DeckProto::connect(buf, sizeof(buf));
  if (n) deck_out_queue(buf, n);
}

// ---------------------------------------------------------------------------
// Client event callback
// ---------------------------------------------------------------------------
static void deck_client_event_cb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
  if (!msg) return;

  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    DECK_LOG("event: NEW_DEV addr=%d", msg->new_dev.address);
    if (deck_open_and_configure(msg->new_dev.address) == ESP_OK) {
      app_message_t m = {};
      m.id = app_message_t::APP_DECK_DEVICE_CONNECTED;
      m.data.deck_device_info.vid = deck_vid_v;
      m.data.deck_device_info.pid = deck_pid_v;
      strncpy(m.data.deck_device_info.name, deck_name,
              sizeof(m.data.deck_device_info.name) - 1);
      m.data.deck_device_info.name[sizeof(m.data.deck_device_info.name) - 1] = '\0';
      xQueueSend(app_queue, &m, 0);
    } else {
      DECK_LOG("event: NEW_DEV addr=%d not claimed (other client or not a deck)",
               msg->new_dev.address);
    }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    DECK_LOG("event: DEV_GONE");
    deck_close_device();
    app_message_t m = {};
    m.id = app_message_t::APP_DECK_DEVICE_DISCONNECTED;
    xQueueSend(app_queue, &m, 0);
  }
}

// ---------------------------------------------------------------------------
// IN transfer callback
//
// Phase 0: post the raw index/state to the queue and log the whole report, so
// the real layout can be confirmed against the device. Field interpretation
// beyond index/state belongs to the usermod once verified.
// ---------------------------------------------------------------------------
static void deck_in_transfer_cb(usb_transfer_t* xfer) {
  if (!xfer) return;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    const uint8_t* p = xfer->data_buffer;
    uint16_t n = xfer->actual_num_bytes;

    if (DeckProto::inIsAck(p, n)) {
      uint8_t index = p[DeckProto::IN_INDEX_OFF];
      uint8_t state = p[DeckProto::IN_STATE_OFF];

      app_message_t m = {};
      m.id = app_message_t::APP_DECK_PACKET;
      m.data.deck.index = index;
      m.data.deck.state = state;
      m.data.deck.bytes = n;
      memcpy(m.data.deck.report, p, (n < DECK_REPORT_SNAPSHOT) ? n : DECK_REPORT_SNAPSHOT);
      xQueueSend(app_queue, &m, 0);

      DECK_LOG("IN: index=%u state=0x%02X len=%u", index, state, n);
    } else {
      DECK_DEBUG("IN: non-ACK report, %u bytes, ignored", n);
    }

    // Healthy completion — leave the data toggle and endpoint alone.
    in_error_recovery_needed = false;
  } else {
    DECK_DEBUG("IN callback: status=%d bytes=%d (recover in poll)",
               (int)xfer->status, (int)xfer->actual_num_bytes);
    in_error_recovery_needed = true;
  }

  in_resubmit_needed = true;
}

// ---------------------------------------------------------------------------
// OUT transfer callback
// ---------------------------------------------------------------------------
static void deck_out_transfer_cb(usb_transfer_t* /*xfer*/) {
  out_in_flight = false;
}

// ---------------------------------------------------------------------------
// Descriptor walk + claim
// ---------------------------------------------------------------------------
static esp_err_t deck_open_and_configure(uint8_t dev_addr) {
  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(deck_client, dev_addr, &dev);
  if (err != ESP_OK) {
    DECK_LOG("open: device_open(addr=%d) failed: %s (probably owned by MSC/MIDI)",
             dev_addr, esp_err_to_name(err));
    return err;
  }

  const usb_device_desc_t* dev_desc = nullptr;
  err = usb_host_get_device_descriptor(dev, &dev_desc);
  if (err != ESP_OK || !dev_desc) {
    DECK_LOG("open: get_device_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(deck_client, dev);
    return err;
  }
  deck_vid_v = dev_desc->idVendor;
  deck_pid_v = dev_desc->idProduct;

  // VID/PID filter. Without this the deck client would open every device the
  // host sees and starve the MSC and MIDI clients.
  if (deck_vid_v != SOOMFON_VID ||
      (deck_pid_v != CN003_PID && device_lookup(deck_vid_v, deck_pid_v) == nullptr)) {
    DECK_LOG("open: VID=0x%04X PID=0x%04X is not a known deck — releasing",
             deck_vid_v, deck_pid_v);
    usb_host_device_close(deck_client, dev);
    deck_vid_v = 0;
    deck_pid_v = 0;
    return ESP_FAIL;
  }

  const char* known = device_lookup(deck_vid_v, deck_pid_v);
  if (known) {
    strncpy(deck_name, known, sizeof(deck_name) - 1);
    deck_name[sizeof(deck_name) - 1] = '\0';
  } else {
    deck_name[0] = '\0';
  }

  DECK_LOG("open: VID=0x%04X PID=0x%04X (%s) — claimed as deck",
           deck_vid_v, deck_pid_v, deck_device_name());

  const usb_config_desc_t* cfg_desc = nullptr;
  err = usb_host_get_active_config_descriptor(dev, &cfg_desc);
  if (err != ESP_OK || !cfg_desc) {
    DECK_LOG("open: get_active_config_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(deck_client, dev);
    return ESP_FAIL;
  }

#if DECK_DUMP_DESCRIPTORS
  usb_print_device_descriptor(dev_desc);
  usb_print_config_descriptor(cfg_desc, nullptr);
#endif

  // Find the deck's HID interface and its interrupt endpoints. These devices
  // present usage page 0xFF00; depending on firmware that arrives as a
  // vendor-class interface (0xFF) or as HID (0x03) with a vendor usage page.
  // A composite deck may also expose HID interfaces we don't want (media keys
  // and the like), so score the candidates and keep the best one: a vendor
  // interface that is bidirectional beats everything else.
  int   claimed_iface = -1;
  int   best_score    = 0;
  uint8_t in_ep = 0, out_ep = 0;

  for (uint8_t iface_num = 0; iface_num < 8; iface_num++) {
    int iface_off = 0;
    const usb_intf_desc_t* iface =
      usb_parse_interface_descriptor(cfg_desc, iface_num, 0, &iface_off);
    if (!iface) break;

    bool vendor = (iface->bInterfaceClass == 0xFF);
    bool hid    = (iface->bInterfaceClass == 0x03);
    if (!vendor && !hid) continue;

    uint8_t this_in = 0, this_out = 0;
    for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
      int ep_off = iface_off;
      const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
        iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
      if (!ep) continue;
      uint8_t attr = ep->bmAttributes & 0x03;
      if (attr != 3) continue;               // interrupt endpoints only
      if ((ep->bEndpointAddress & 0x80) && !this_in) this_in = ep->bEndpointAddress;
      else if (!(ep->bEndpointAddress & 0x80) && !this_out) this_out = ep->bEndpointAddress;
    }

    DECK_LOG("open:   iface %u class=0x%02X subclass=0x%02X protocol=0x%02X "
             "eps=%u IN=0x%02X OUT=0x%02X",
             iface_num, iface->bInterfaceClass, iface->bInterfaceSubClass,
             iface->bInterfaceProtocol, iface->bNumEndpoints, this_in, this_out);

    if (!this_in) continue;                  // cannot work without input

    int score = (vendor ? 4 : 0) + (this_out ? 2 : 0);
    if (score > best_score) {
      best_score = score;
      claimed_iface = iface_num;
      in_ep = this_in;
      out_ep = this_out;
    }
  }

  if (claimed_iface < 0 || in_ep == 0) {
    DECK_LOG("open: no interface with an interrupt IN endpoint");
    usb_host_device_close(deck_client, dev);
    return ESP_FAIL;
  }

  if (out_ep == 0) {
    DECK_LOG("open: WARNING no interrupt OUT endpoint — input only, no LED control");
  }

  err = usb_host_interface_claim(deck_client, dev, (uint8_t)claimed_iface, 0);
  if (err != ESP_OK) {
    DECK_LOG("open: interface_claim(iface=%d) failed: %s",
             claimed_iface, esp_err_to_name(err));
    usb_host_device_close(deck_client, dev);
    return err;
  }

  deck_dev_hdl = dev;
  deck_in_ep  = in_ep;
  deck_out_ep = out_ep;
  deck_iface  = (uint8_t)claimed_iface;

  DECK_LOG("open: CLAIMED iface=%d IN=0x%02X OUT=0x%02X",
           claimed_iface, in_ep, out_ep);

  // Bring the device up. DIS resets the display, then set a modest keypad
  // brightness and paint each knob a distinct colour so the four rings are
  // visibly wired to the right index.
  if (out_ep) {
    uint8_t buf[DeckProto::MAX_OUT_BYTES];

    size_t n = DeckProto::dis(buf, sizeof(buf));
    if (n) deck_out_queue(buf, n);

    n = DeckProto::lig(buf, sizeof(buf), 50);
    if (n) deck_out_queue(buf, n);

    const uint8_t test_colors[12] = {
      255, 0,   0,     // knob 0 — red
      0,   255, 0,     // knob 1 — green
      0,   0,   255,   // knob 2 — blue
      255, 255, 0,     // knob 3 — yellow
    };
    n = DeckProto::setlb(buf, sizeof(buf), test_colors, 4);
    if (n) deck_out_queue(buf, n);

    n = DeckProto::stp(buf, sizeof(buf));
    if (n) deck_out_queue(buf, n);
  }

  // One IN transfer in flight; the callback re-submits the same one.
  if (deck_in_xfer) {
    deck_in_xfer->device_handle = dev;
    deck_in_xfer->bEndpointAddress = in_ep;
    deck_in_xfer->num_bytes = IN_XFER_SIZE;
    err = usb_host_transfer_submit(deck_in_xfer);
    if (err != ESP_OK) {
      DECK_LOG("open: initial IN submit failed: %s", esp_err_to_name(err));
    } else {
      DECK_LOG("open: 1 IN transfer submitted");
    }
  }

  last_keepalive_ms = millis();
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// deck_close_device
// ---------------------------------------------------------------------------
static void deck_close_device(void) {
  if (!deck_dev_hdl) return;

  // Note: the "red knobs on exit" shutdown sequence (DeckProto::shutdown_cmd)
  // needs the device to still be present, so it belongs on the reboot path,
  // not here — by the time DEV_GONE fires the pipe is already gone.

  usb_host_interface_release(deck_client, deck_dev_hdl, deck_iface);
  if (deck_in_xfer)  deck_in_xfer->device_handle = nullptr;
  if (deck_out_xfer) deck_out_xfer->device_handle = nullptr;
  out_in_flight = false;
  usb_host_device_close(deck_client, deck_dev_hdl);

  deck_dev_hdl = nullptr;
  deck_in_ep = 0;
  deck_out_ep = 0;
  deck_iface = 0;
  deck_vid_v = 0;
  deck_pid_v = 0;
  deck_name[0] = '\0';
}

#endif  // USERMOD_DECK_USB
