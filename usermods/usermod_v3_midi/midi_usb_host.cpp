// midi_usb_host.cpp
// USB-MIDI Host client for the ESP32-P4 EV board.
//
// Responsibilities:
//  - Register as an ESP-IDF USB Host client that coexists with the
//    MSC client (midi_open_and_configure VID/PID filter keeps the two
//    from fighting over the same device).
//  - Submit IN transfers and parse USB-MIDI event packets into
//    status/data1/data2 triplets for the usermod to handle.
//  - Drain an OUT ring buffer (midi_out_queue()) and submit bulk-OUT
//    URBs for LED feedback back to the controller.
//
// Build: -D USERMOD_MIDI_USB on the P4 env (esp32p4_8MB_troyhacks).
// Targets: pioarduino + framework-arduinoespressif32 (ESP-IDF v5 USB Host).

#include "wled.h"
#include "../usermods/usermod_v3_midi/midi_usb_host.h"
#include "../usermods/usermod_v3_midi/usb_host_messages.h"
#include "../usermods/usermod_v3_midi/usermod_v3_midi.h"

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "freertos/ringbuf.h"

extern MidiUsermod* midiUsermodPtr;

#define MIDI_LOG(fmt, ...) do { if (canUseSerial()) Serial.printf("[MIDI] " fmt "\n", ##__VA_ARGS__); } while (0)
#define MIDI_DEBUG(fmt, ...) DEBUG_PRINTF("[MIDI] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define NUM_TRANSFERS        4
#define TRANSFER_SIZE        64

// Per-client VID/PID filter — only these VIDs (or the specific VID/PID
// table below) are accepted by the MIDI client. Everything else is
// released for the MSC client (or whatever else the framework wants
// to claim it). This prevents the two clients from fighting over the
// same device when both are running.
static const uint16_t kMidiVendorIds[] = {
  0x09E8, // Akai
  0x4353, // Donner
  0x04D9, // Donner (ITE Tech shared)
  0x0944, // Korg
  0x17CC, // Native Instruments
  0x1C75, // Arturia
  0x1235, // Novation
  0x1234, // Generic MIDI class
  0x0763, // M-Audio
  0x1584, // Yamaha
};

static const struct { uint16_t vid; uint16_t pid; const char* name; } kMidiDevices[] = {
  { 0x09E8, 0x004F, "Akai APC Mini Mk2" },
  { 0x4353, 0x4B4D, "Donner StarryPad Mini" },
  { 0x04D9, 0xE000, "Donner StarryPad" },
};

// Devices that are NOT MIDI controllers, even when their vendor is in the
// list above. The Traktor Kontrol F1 is a HID device (class 3) that NI's
// driver turns into MIDI on the PC; on the P4 the HID control-surface
// client (usermod_v3_hidctl) claims it. Racing it here would mean one of
// the two clients grabbing the device and the other failing to open it.
static const struct { uint16_t vid; uint16_t pid; const char* name; } kNotMidiDevices[] = {
  { 0x17CC, 0x1120, "Traktor Kontrol F1 (HID)" },
};

static bool is_not_midi_device(uint16_t vid, uint16_t pid) {
  for (const auto& d : kNotMidiDevices) {
    if (d.vid == vid && d.pid == pid) return true;
  }
  return false;
}

static bool is_known_midi_vendor(uint16_t vid) {
  for (uint16_t v : kMidiVendorIds) if (v == vid) return true;
  return false;
}

static const char* midi_device_lookup(uint16_t vid, uint16_t pid) {
  for (const auto& d : kMidiDevices) {
    if (d.vid == vid && d.pid == pid) return d.name;
  }
  switch (vid) {
  case 0x09E8: return "Akai (unknown PID)";
  case 0x4353: return "Donner (unknown PID)";
  case 0x04D9: return "Donner (unknown PID)";
  case 0x0944: return "Korg (unknown PID)";
  case 0x17CC: return "Native Instruments (unknown PID)";
  case 0x1C75: return "Arturia (unknown PID)";
  case 0x1235: return "Novation (unknown PID)";
  case 0x1234: return "Generic MIDI (unknown PID)";
  default:     return "Unknown MIDI controller";
  }
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static usb_host_client_handle_t midi_client = nullptr;
static usb_device_handle_t     midi_dev_hdl = nullptr;
static uint8_t                 midi_in_ep = 0;
static uint8_t                 midi_out_ep = 0;
static uint8_t                 midi_iface = 0;
static uint8_t                 midi_cur_addr = 0;
static uint16_t                midi_vendor = 0;
static uint16_t                midi_product = 0;

static usb_transfer_t* midi_in_xfer[NUM_TRANSFERS] = { nullptr };

// Set by the IN callback when the transfer is ready to be re-submitted.
// We don't re-submit from inside the callback — that runs from within
// usb_host_client_handle_events, and the DWC pipe may still be transitioning
// to HALTED (on error) or its internal state may not yet accept a new URB.
// Doing it from the poll loop gives the LIB handler a full cycle to settle
// and lets us run halt/flush/clear recovery before resubmitting.
static volatile bool midi_in_resubmit_needed = false;
// Set by the IN callback when the transfer did NOT complete successfully.
// On a successful completion we leave the endpoint alone — calling
// usb_host_endpoint_clear on a healthy pipe resets the host's data toggle
// to DATA0 while the device's toggle has advanced to DATA1, causing every
// subsequent packet to be silently dropped due to a sequence mismatch.
// halt+flush+clear is ONLY safe (and required) when the previous URB ended
// in an error state, because in that case the DWC pipe is HALTED and the
// toggle would have been reset anyway.
static volatile bool midi_in_error_recovery_needed = false;

// OUT path state.
static usb_transfer_t* midi_out_xfer = nullptr;
static volatile bool   midi_out_in_flight = false;
// OUT ring buffer. Holds USB-MIDI event packets (4 bytes each).
// A full controller repaint queues ~150 packets (80 wipe + 64 pads +
// tracks + scenes), so 512 bytes (128 packets) was too small and
// dropped late packets — most noticeably the post-power-on scene 8
// send, which would get dropped before the OUT thread could drain.
// 2048 bytes (512 packets) gives comfortable headroom.
static uint8_t          out_rb_storage[2048]      __attribute__((aligned(4)));
static StaticRingbuffer_t out_rb_struct;
static RingbufHandle_t    midi_out_rb = nullptr;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void midi_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void midi_in_transfer_cb(usb_transfer_t* xfer);
static void midi_out_transfer_cb(usb_transfer_t* xfer);
static esp_err_t midi_open_and_configure(uint8_t dev_addr);
static void      midi_close_device(void);

// ---------------------------------------------------------------------------
// midi_usb_init — called once from usb_task after msc_host_install()
// ---------------------------------------------------------------------------
void midi_usb_init(void) {
  if (midi_client) return;

  midi_out_rb = xRingbufferCreateStatic(
    512, RINGBUF_TYPE_BYTEBUF,
    out_rb_storage, &out_rb_struct);
  if (!midi_out_rb) {
    MIDI_LOG("init: OUT ring buffer alloc failed");
    return;
  }

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = midi_client_event_cb;
  cfg.async.callback_arg = nullptr;

  esp_err_t err = usb_host_client_register(&cfg, &midi_client);
  if (err != ESP_OK) {
    MIDI_LOG("init: usb_host_client_register failed: %s", esp_err_to_name(err));
    midi_client = nullptr;
    return;
  }

  for (int i = 0; i < NUM_TRANSFERS; i++) {
    err = usb_host_transfer_alloc(TRANSFER_SIZE, 0, &midi_in_xfer[i]);
    if (err != ESP_OK) {
      MIDI_LOG("init: usb_host_transfer_alloc[%d] failed: %s", i, esp_err_to_name(err));
      midi_in_xfer[i] = nullptr;
      continue;
    }
    midi_in_xfer[i]->context = nullptr;
    midi_in_xfer[i]->device_handle = nullptr;
    midi_in_xfer[i]->callback = midi_in_transfer_cb;
    midi_in_xfer[i]->num_bytes = TRANSFER_SIZE;
  }

  err = usb_host_transfer_alloc(TRANSFER_SIZE, 0, &midi_out_xfer);
  if (err != ESP_OK) {
    MIDI_LOG("init: usb_host_transfer_alloc OUT failed: %s", esp_err_to_name(err));
    midi_out_xfer = nullptr;
  } else {
    midi_out_xfer->callback = midi_out_transfer_cb;
    midi_out_xfer->context = nullptr;
  }

  MIDI_LOG("init: USB Host client ready");
}

// ---------------------------------------------------------------------------
// midi_usb_poll — called each iteration of usb_task
// ---------------------------------------------------------------------------
void midi_usb_poll(void) {
  if (!midi_client) return;

  // Drain client events (transfers complete, new-device events, etc.)
  usb_host_client_handle_events(midi_client, 0);

  // Re-submit any IN transfer that completed (or errored) on the previous
  // event. Doing this here, outside the callback, lets the LIB handler finish
  // its state transitions and lets us call halt/flush/clear cleanly if the
  // pipe is in HALTED state due to a DWC error.
  if (midi_in_resubmit_needed && midi_dev_hdl) {
    midi_in_resubmit_needed = false;
    usb_transfer_t* x = midi_in_xfer[0];
    if (x && x->device_handle) {
      // ONLY reset the pipe if the previous URB actually errored. On a
      // successful URB the DWC pipe is in ACTIVE state with the data toggle
      // advanced; an unconditional usb_host_endpoint_clear here would reset
      // the host's toggle to DATA0, putting it out of sync with the device
      // and causing all subsequent packets to be silently dropped.
      if (midi_in_error_recovery_needed) {
        usb_host_endpoint_halt(x->device_handle, x->bEndpointAddress);
        usb_host_endpoint_flush(x->device_handle, x->bEndpointAddress);
        usb_host_endpoint_clear(x->device_handle, x->bEndpointAddress);
        midi_in_error_recovery_needed = false;
      }
      x->num_bytes = TRANSFER_SIZE;
      esp_err_t err = usb_host_transfer_submit(x);
      if (err != ESP_OK) {
        // Couldn't resubmit yet — flag for retry and ensure we do a
        // halt+flush+clear on the next attempt (pipe is likely HALTED).
        midi_in_resubmit_needed = true;
        midi_in_error_recovery_needed = true;
        MIDI_DEBUG("IN re-submit failed: %s", esp_err_to_name(err));
      }
    }
  }

  // OUT path: drain ring buffer and submit bulk-OUT URBs to the controller.
  // Single in-flight URB (same pattern as IN — ESP-IDF allows only one
  // in-flight URB per endpoint). midi_out_transfer_cb clears midi_out_in_flight
  // when the URB completes, allowing the next one to be submitted.
  if (midi_dev_hdl && midi_out_ep && midi_out_xfer && !midi_out_in_flight && midi_out_rb) {
    size_t got = 0;
    const uint8_t* data = (const uint8_t*)xRingbufferReceiveUpTo(
      midi_out_rb, &got, 0, TRANSFER_SIZE);
    if (data) {
      if (got >= 4) {
        memcpy(midi_out_xfer->data_buffer, data, got);
        vRingbufferReturnItem(midi_out_rb, (void*)data);
        midi_out_xfer->num_bytes = got;
        midi_out_xfer->device_handle = midi_dev_hdl;
        midi_out_xfer->bEndpointAddress = midi_out_ep;
        midi_out_in_flight = true;
        esp_err_t err = usb_host_transfer_submit(midi_out_xfer);
        if (err != ESP_OK) {
          midi_out_in_flight = false;
          MIDI_DEBUG("OUT submit failed: %s", esp_err_to_name(err));
        }
      } else {
        vRingbufferReturnItem(midi_out_rb, (void*)data);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// midi_out_queue — push a 3-byte MIDI message onto the OUT ring buffer for
// transmission to the controller. midi_usb_poll() drains the buffer and
// submits bulk-OUT URBs. Returns false if the device isn't open or the
// ring buffer rejects the send.
// ---------------------------------------------------------------------------
bool midi_out_queue(uint8_t status, uint8_t d1, uint8_t d2) {
  if (!midi_dev_hdl || !midi_out_ep || !midi_out_rb) return false;
  // USB-MIDI event packet: 4 bytes.
  //   Byte 0: [Cable Number (4 bits, =0)][Code Index Number (4 bits)]
  //   Byte 1: MIDI status
  //   Byte 2: MIDI data 1
  //   Byte 3: MIDI data 2
  // CIN mapping per USB-MIDI 1.0 spec table 4-1:
  //   0x8=NoteOff, 0x9=NoteOn, 0xA=PolyKeyPress, 0xB=CC,
  //   0xC=ProgramChange, 0xD=ChannelPressure, 0xE=PitchBend, 0xF=SysEx
  uint8_t cin;
  switch (status & 0xF0) {
    case 0x80: cin = 0x08; break;  // NoteOff
    case 0x90: cin = 0x09; break;  // NoteOn
    case 0xA0: cin = 0x0A; break;  // PolyKeyPress
    case 0xB0: cin = 0x0B; break;  // CC
    case 0xC0: cin = 0x0C; break;  // ProgramChange
    case 0xD0: cin = 0x0D; break;  // ChannelPressure
    case 0xE0: cin = 0x0E; break;  // PitchBend
    default:   cin = 0x0F; break;  // System messages — caller should bypass this
  }
  uint8_t pkt[4] = { cin, status, d1, d2 };

  BaseType_t ok = xRingbufferSend(midi_out_rb, pkt, sizeof(pkt), 0);
  if (ok != pdTRUE) {
    MIDI_DEBUG("OUT queue full, dropped status=0x%02X d1=%u d2=%u", status, d1, d2);
    return false;
  }
  MIDI_LOG("OUT queue: status=0x%02X d1=%u d2=%u", status, d1, d2);
  return true;
}

// ---------------------------------------------------------------------------
// Client event callback
// ---------------------------------------------------------------------------
static void midi_client_event_cb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
  if (!msg) return;
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    MIDI_LOG("event: NEW_DEV addr=%d", msg->new_dev.address);
    if (midi_open_and_configure(msg->new_dev.address) == ESP_OK) {
      // Publish the device descriptor on the app_queue. wled.cpp
      // consumes the APP_MIDI_DEVICE_CONNECTED message and publishes
      // a UsbDeviceChanged event. The usermod's onEvent handler does
      // the rest: sets the local state, latches the active preset
      // (via onPreStateChange), and triggers the repaint.
      app_message_t m = {};
      m.id = app_message_t::APP_MIDI_DEVICE_CONNECTED;
      m.data.midi_device_info.vid = midi_vendor;
      m.data.midi_device_info.pid = midi_product;
      {
        const char* nm = midi_device_lookup(midi_vendor, midi_product);
        if (nm) {
          strncpy(m.data.midi_device_info.name, nm,
                  sizeof(m.data.midi_device_info.name) - 1);
          m.data.midi_device_info.name[sizeof(m.data.midi_device_info.name) - 1] = '\0';
        } else {
          m.data.midi_device_info.name[0] = '\0';
        }
      }
      xQueueSend(app_queue, &m, 0);
      // Single delayed "settled" repaint: a one-shot FreeRTOS task
      // waits ~1.5s for the boot state to settle, then forces a full
      // repaint. Uses a static ctx buffer (no heap, no race with the
      // SD-card task).
      if (midiUsermodPtr) {
        struct RepaintCtx { MidiUsermod* ptr; };
        static RepaintCtx s_repaint_ctx;
        s_repaint_ctx.ptr = midiUsermodPtr;
        xTaskCreate(
          [](void* arg) {
            auto* c = (RepaintCtx*)arg;
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (c->ptr) {
              c->ptr->invalidateAllLedDedup();
              c->ptr->onStateChange(CALL_MODE_BUTTON);
            }
            vTaskDelete(NULL);
          },
          "midi_repaint", 2048, &s_repaint_ctx, 1, nullptr);
      }
    } else {
      MIDI_LOG("event: NEW_DEV addr=%d configure failed (likely MSC or unknown device)", msg->new_dev.address);
    }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    MIDI_LOG("event: DEV_GONE");
    // wled.cpp consumes APP_MIDI_DEVICE_DISCONNECTED and publishes a
    // UsbDeviceChanged event (connected=false); the usermod's
    // onEvent handler resets its state.
    midi_close_device();
    app_message_t m = {};
    m.id = app_message_t::APP_MIDI_DEVICE_DISCONNECTED;
    xQueueSend(app_queue, &m, 0);
  }
}

// ---------------------------------------------------------------------------
// IN transfer callback — parses USB-MIDI event packets and dispatches
// them to the usermod, which drives WLED state and queues LED
// feedback back through midi_out_queue().
// ---------------------------------------------------------------------------
static void midi_in_transfer_cb(usb_transfer_t* xfer) {
  if (!xfer) return;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    const uint8_t* p = xfer->data_buffer;
    int n = xfer->actual_num_bytes;
    for (int i = 0; i + 4 <= n; i += 4) {
      uint8_t cin = p[i] & 0x0F;
      uint8_t status = p[i + 1];
      uint8_t d1 = p[i + 2];
      uint8_t d2 = p[i + 3];
      if (cin < 0x8 || cin > 0xE) continue;
      if (midiUsermodPtr) {
        midiUsermodPtr->handleIncomingMidi(status, d1, d2);
      }
      MIDI_LOG("IN: status=0x%02X d1=%u d2=%u", status, d1, d2);
    }
    // Successful completion — leave the data toggle alone, do NOT reset
    // the endpoint. usb_host_endpoint_clear would reset the host toggle
    // to DATA0 while the device has already moved to DATA1, causing a
    // mismatch that silently drops the next packet.
    midi_in_error_recovery_needed = false;
  } else {
    // Non-COMPLETED status (error / cancelled / no device). The DWC HCD
    // has already put the pipe in HALTED; midi_usb_poll() will halt+flush
    // +clear and resubmit on the next iteration to recover.
    MIDI_DEBUG("IN callback: status=%d bytes=%d (will recover in poll)",
      (int)xfer->status, (int)xfer->actual_num_bytes);
    midi_in_error_recovery_needed = true;
  }

  // Defer the re-submit to midi_usb_poll (not from inside the callback).
  midi_in_resubmit_needed = true;
}

// OUT transfer callback — clears the in-flight flag so the poll loop
// submits the next queued packet.
static void midi_out_transfer_cb(usb_transfer_t* xfer) {
  (void)xfer;
  midi_out_in_flight = false;
}

// ---------------------------------------------------------------------------
// Descriptor walking + per-client VID/PID filter.
//
// The filter is what lets the MIDI and MSC clients coexist on the same
// controller without fighting over every device. We open the device just
// to read its descriptor, and if the VID isn't a known MIDI vendor (or
// the exact VID/PID isn't in the MIDI table), we close the device
// immediately and let the MSC client (or whatever else) have it.
// ---------------------------------------------------------------------------
static esp_err_t midi_open_and_configure(uint8_t dev_addr) {
  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(midi_client, dev_addr, &dev);
  if (err != ESP_OK) {
    MIDI_LOG("configure: device_open(addr=%d) failed: %s (probably owned by another client — e.g., MSC)",
      dev_addr, esp_err_to_name(err));
    return err;
  }

  const usb_device_desc_t* dev_desc = nullptr;
  err = usb_host_get_device_descriptor(dev, &dev_desc);
  if (err != ESP_OK) {
    MIDI_LOG("configure: get_device_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return err;
  }
  midi_vendor = dev_desc->idVendor;
  midi_product = dev_desc->idProduct;

  // Per-client VID/PID filter: only accept known MIDI vendors (or exact
  // VID/PID in the known table). This prevents the MSC and MIDI clients
  // from fighting over the same device.
  if (is_not_midi_device(midi_vendor, midi_product)) {
    MIDI_LOG("configure: VID=0x%04X PID=0x%04X (%s) — not a MIDI device, releasing",
             midi_vendor, midi_product, midi_device_lookup(midi_vendor, midi_product));
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }
  if (!is_known_midi_vendor(midi_vendor)) {
    MIDI_LOG("configure: VID=0x%04X not in MIDI vendor list — releasing for other clients",
      midi_vendor);
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }

  MIDI_LOG("configure: VID=0x%04X PID=0x%04X (%s) — MIDI vendor match",
    midi_vendor, midi_product,
    midi_device_lookup(midi_vendor, midi_product));

  const usb_config_desc_t* cfg_desc = nullptr;
  err = usb_host_get_active_config_descriptor(dev, &cfg_desc);
  if (err != ESP_OK || !cfg_desc) {
    MIDI_LOG("configure: get_active_config_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }

  int  claimed_iface = -1;
  uint8_t in_ep = 0, out_ep = 0;
  for (uint8_t iface_num = 0; iface_num < 8; iface_num++) {
    int iface_off = 0;
    const usb_intf_desc_t* iface = usb_parse_interface_descriptor(cfg_desc, iface_num, 0, &iface_off);
    if (!iface) break;
    if (iface->bInterfaceClass == 0x01 && iface->bInterfaceSubClass == 0x03) {
      claimed_iface = iface_num;
      for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
        int ep_off = iface_off;
        const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
          iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
        if (!ep) continue;
        if ((ep->bmAttributes & 0x03) != 2) continue;
        if ((ep->bEndpointAddress & 0x80) && !in_ep)  in_ep = ep->bEndpointAddress;
        else if (!(ep->bEndpointAddress & 0x80) && !out_ep) out_ep = ep->bEndpointAddress;
      }
      break;
    }
  }

  if (claimed_iface < 0 || in_ep == 0) {
    MIDI_LOG("configure: no usable MIDI endpoints");
    usb_host_device_close(midi_client, dev);
    return ESP_FAIL;
  }

  err = usb_host_interface_claim(midi_client, dev, (uint8_t)claimed_iface, 0);
  if (err != ESP_OK) {
    MIDI_LOG("configure: interface_claim(iface=%d) failed: %s", claimed_iface, esp_err_to_name(err));
    usb_host_device_close(midi_client, dev);
    return err;
  }

  midi_dev_hdl = dev;
  midi_in_ep = in_ep;
  midi_out_ep = out_ep;
  midi_iface = (uint8_t)claimed_iface;
  midi_cur_addr = dev_addr;

  MIDI_LOG("configure: CLAIMED iface=%d IN=0x%02X OUT=0x%02X",
    claimed_iface, in_ep, out_ep);

  // ESP-IDF USB Host only allows ONE transfer in flight per endpoint. The
  // earlier NUM_TRANSFERS=4 design (TinyUSB-style) caused Enqueue URB
  // ESP_ERR_INVALID_STATE on the 2nd+ submits — the HCD rejected them
  // because the pipe was already occupied by the first submission. We
  // submit a single transfer here; the callback re-submits the same one.
  usb_transfer_t* x = midi_in_xfer[0];
  if (x) {
    x->device_handle = dev;
    x->bEndpointAddress = in_ep;
    x->num_bytes = TRANSFER_SIZE;
    err = usb_host_transfer_submit(x);
    if (err != ESP_OK) {
      MIDI_LOG("configure: initial IN submit failed: %s", esp_err_to_name(err));
    }
  }
  MIDI_LOG("configure: 1 IN transfer submitted");
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Close device
// ---------------------------------------------------------------------------
static void midi_close_device(void) {
  if (!midi_dev_hdl) return;
  usb_host_interface_release(midi_client, midi_dev_hdl, midi_iface);
  for (int i = 0; i < NUM_TRANSFERS; i++) {
    usb_transfer_t* x = midi_in_xfer[i];
    if (x && x->device_handle == midi_dev_hdl) x->device_handle = nullptr;
  }
  if (midi_out_xfer) midi_out_xfer->device_handle = nullptr;
  midi_out_in_flight = false;
  usb_host_device_close(midi_client, midi_dev_hdl);
  midi_dev_hdl = nullptr;
  midi_in_ep = 0;
  midi_out_ep = 0;
  midi_iface = 0;
  midi_cur_addr = 0;
  midi_vendor = 0;
  midi_product = 0;
}