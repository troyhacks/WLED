// hid_usb_host.cpp
//
// USB Host client for generic HID control surfaces (ESP32-P4 EV board).
//
// Responsibilities:
//  - Register as an ESP-IDF USB Host client.
//  - When a device appears, ask the usermod whether one of its profiles
//    wants it (VID/PID match). If not, release the device immediately so
//    the MSC / MIDI / deck clients can have it.
//  - Claim the HID interface (USB class 3) and its interrupt IN endpoint,
//    then submit IN transfers and post the raw reports to the app queue.
//
// The re-submit happens in hid_usb_poll(), not in the transfer callback:
// the library finishes its state transitions inside the callback, and
// resetting the pipe there would put the data toggle out of sync with the
// device (the same arrangement the MIDI client uses).
//
// Build: -D USERMOD_HIDCTL_USB on the P4 env.

#include "wled.h"
#include "../usermods/usermod_v3_hidctl/hid_usb_host.h"
#include "../usermods/usermod_v3_midi/usb_host_messages.h"
#include "../usermods/usermod_v3_hidctl/usermod_v3_hidctl.h"
#include "../usermods/usermod_v3_hidctl/hid_dwc_probe.h"

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

extern HidctlUsermod* hidctlUsermodPtr;

#define HIDCTL_LOG(fmt, ...) do { if (canUseSerial()) Serial.printf("[HIDCTL] " fmt "\n", ##__VA_ARGS__); } while (0)
#define HIDCTL_DEBUG(fmt, ...) DEBUG_PRINTF("[HIDCTL] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// One device on the bus
// ---------------------------------------------------------------------------
//
// Everything the host knows about a device lives here, and the host keeps
// a LIST of them: the board has a hub, so several HID devices can be
// plugged in at once, and they are not the same kind of device. The F1
// needs an 81-byte LED report and keeps its report ID; the CN003 speaks
// the "CRT" command protocol, drops its report ID, and needs a
// 1025-byte packet. The framing is per device, not per host, or the
// second device to be plugged in would change how the first one is
// talked to.
//
// The transfers carry the device in their context, so a callback always
// knows which device it is about.
struct HidDevice {
  bool     used = false;
  uint8_t  addr = 0;
  usb_device_handle_t handle = nullptr;
  uint16_t vid = 0;
  uint16_t pid = 0;
  uint8_t  iface = 0;
  uint8_t  in_ep = 0;
  uint8_t  out_ep = 0;
  uint16_t in_mps = 0;
  uint16_t out_mps = 0;
  uint8_t  link_speed = 0;        // 0 = unknown, 1 = full, 2 = high
  // True for a device whose reports are always exactly one maximum packet
  // long: the OpenDeck-family decks. A shorter packet is then a
  // zero-length one, whatever the driver says its length is.
  bool     in_exact_mps = false;
  // How many bytes the interrupt IN transfer asks for. It has to be a
  // whole number of maximum packets; asking for more than one leaves the
  // device's shorter packet to end the transfer.
  uint16_t in_xfer_len = 0;        // 0 = one maximum packet
  // Force the IN pipe to be torn down and armed again.
  bool     in_force_reset = false;
  // The IN transfer is armed from the main loop rather than from the
  // client's event callback.
  bool     in_arm_pending = false;
  volatile bool in_resubmit_needed = false;

  usb_transfer_t* in_xfer = nullptr;
  // The output path: one SET_REPORT control transfer at a time.
  usb_transfer_t* ctrl_xfer = nullptr;
  // A separate transfer for the one-off GET_DESCRIPTOR of the report
  // descriptor (its own callback, and a bigger buffer).
  usb_transfer_t* desc_xfer = nullptr;
  // A transfer of its own for the class requests: if the device does not
  // answer them the transfer never completes, and it must not then block
  // the rest of the control pipe.
  usb_transfer_t* class_xfer = nullptr;
  usb_transfer_t* out_xfer = nullptr;

  // The framing: how this device's packets go on the wire.
  bool     out_via_control = false;
  bool     out_drop_leading = false;
  bool     out_shift = false;
  bool     out_zlp = false;
  uint16_t out_cap = 0;

  // The outgoing queue.
  //
  // The producer copies a packet in and returns; the sender - the one
  // task that owns the USB stack - takes it out and submits it. The queue
  // is a ring of fixed slots, each holding one packet: the deck's packets
  // are 1024 bytes and the F1's are 81, so the slot size is the device's.
  uint8_t*  out_q = nullptr;      // the slots, in PSRAM
  uint16_t out_q_slots = 0;       // how many there are
  uint16_t out_q_stride = 0;      // the bytes per slot
  uint16_t out_q_head = 0;        // the next free slot
  uint16_t out_q_tail = 0;        // the next slot to send
  uint16_t out_q_count = 0;       // the slots in use
  SemaphoreHandle_t out_q_lock = nullptr;
  // The recovery: a pipe that has wedged is halted, flushed and cleared,
  // and if it does not come back the interface is taken and claimed
  // again, which gives the device a new pipe. The steps are run one per
  // pass of the poll, because the flush's cancellation has to be
  // delivered before the interface can be released.
  uint8_t out_recovery = 0;       // 0 = none, 1 = halt, 2 = flush, ...
  // The last transfer did not go out.
  volatile bool out_failed = false;

  // The OUT transfer's state.
  volatile bool   out_in_flight = false;
  volatile bool   out_zlp_pending = false;
  uint16_t        out_total = 0;
  uint16_t        out_sent = 0;
  volatile uint32_t out_started_ms = 0;
  volatile int8_t   out_submit_error = 0;
  volatile uint32_t out_stuck = 0;
  volatile uint32_t out_count = 0;      // OUT transfers completed
  volatile uint32_t out_bytes = 0;      // what the host thinks it sent
  volatile uint32_t ctrl_out_count = 0; // output reports over the pipe
  volatile uint32_t out_refused_no_device = 0;
  volatile uint32_t out_refused_too_long = 0;
  volatile uint32_t out_refused_busy = 0;
  volatile uint32_t out_refused_submit = 0;
  volatile int8_t  last_out_status = -1;
  uint8_t   out_first[32] = {};
  uint8_t   out_first_len = 0;

  // The control pipe's state.
  volatile bool ctrl_in_flight = false;
  volatile bool ctrl_pending = false;
  volatile bool ctrl_want_readback = false;
  uint8_t   readback[HIDCTL_READBACK_BYTES] = {};
  uint8_t   readback_len = 0;
  // The last report that went out on the OUT endpoint.
  uint8_t   last_out_report[HIDCTL_READBACK_BYTES] = {};
  uint8_t   last_out_len = 0;
  uint16_t  desc_desc_wvalue = 0;
  volatile bool desc_in_flight = false;
  volatile bool want_descriptors = false;

  // The class requests: the sequence a host that reads the encoder sends.
  volatile int8_t  class_status = -1;    // -1 = none sent, 4 = stalled
  uint8_t   class_data[72] = {};
  uint8_t   class_data_len = 0;
  bool      class_in_flight = false;
  bool      class_wanted = false;
  bool      class_enabled = true;
  bool      class_vendor_request = true;
  uint8_t   class_stage = 0;
  uint32_t  class_requests_at = 0;
  bool      class_requests_done = false;
  // The F1's 63-byte feature report (report ID 0xD0). The device declares
  // it and never sends it on its own; it answers when it is asked.
  uint8_t   feature_data[72] = {};
  uint8_t   feature_len = 0;
  bool      want_feature_reports = false;

  // The SET_IDLE rate in milliseconds: 0 = report only when the state
  // changes (what a PC asks for), and anything else = report that often
  // whether or not anything happens.
  uint16_t  idle_rate_ms = 0;
  bool      idle_rate_low_byte = false;
  bool      idle_rate_changed = false;

  // The counters.
  volatile uint32_t in_report_count = 0;
  volatile uint32_t in_zlp_count = 0;
  volatile uint32_t in_error_count = 0;
  volatile uint32_t reports_ok = 0;
  volatile int8_t  last_report_status = -1;

  // The configuration descriptor, as the device sent it. The parse walks
  // it, but the raw bytes settle every question about it - how many
  // interfaces there really are, and how many endpoints each one has.
  uint8_t   config_desc[256] = {};
  uint16_t  config_desc_len = 0;
  // What the device's configuration descriptor says about its interfaces.
  HidctlIfaceInfo ifaces[HIDCTL_MAX_IFACES] = {};
  uint8_t   iface_count = 0;
  // The interface the usermod does not want.
  int8_t    other_iface = -1;
  bool      other_claimed = false;
};

// How many devices the host holds at once. The board has a hub, so a
// keyboard and a mouse can be on it as well as the control surfaces.
static constexpr uint8_t HIDCTL_MAX_DEVICES = 4;
static HidDevice hid_dev[HIDCTL_MAX_DEVICES];

// The device being worked on.
//
// Everything here is single-threaded: the USB host's callbacks are
// dispatched from usb_host_client_handle_events(), which the main loop
// calls, and the transfers are submitted from there too. So one index is
// enough to say whose device the code below is about.
static uint8_t HIDCTL_D = 0;
// The device an index names. An index that is not a device (0xFF means
// "none") must not be used as one: it would walk off the end of the array.
static inline HidDevice* hid_dev_of(uint8_t d) {
  return &hid_dev[d < HIDCTL_MAX_DEVICES ? d : 0];
}
// The device being worked on. An index that is not a device (0xFF means
// "none") must not be used as one: it would walk off the end of the array.
#define D (&hid_dev[HIDCTL_D < HIDCTL_MAX_DEVICES ? HIDCTL_D : 0])

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static usb_host_client_handle_t hid_client = nullptr;

// ---------------------------------------------------------------------------
// The wire log
// ---------------------------------------------------------------------------
// Every report the device sends, and every zero-length packet that follows
// one, so the traffic can be examined afterwards. Only reports that DIFFER
// from the one before are kept - the wheel and the knobs sit still most of
// the time - and each entry says how many packets came in between.
static constexpr uint8_t HIDCTL_LOG_ENTRIES = 48;
//
// The log lives in PSRAM, not in the internal SRAM.
//
// Forty-eight reports of 1024 bytes is 49 KB, and on this board that is
// not a rounding error: the internal SRAM is where the USB host's own
// allocations come from, and the log sat right below the heap's high
// start, so every byte of it was a byte the USB stack could not have.
// With the deck plugged in, the host could not even allocate the
// interrupt endpoint for the second device ("EP Alloc error:
// ESP_ERR_NO_MEM") and the deck was never claimed.
//
// The log is written by the transfer callback (the USB task's context,
// not an ISR) and read by the web server, so PSRAM is safe for it: only
// a DMA engine needs the cache told about it.
static HidLogEntry* hid_log = nullptr;
static uint8_t*  hid_log_last = nullptr;
static uint8_t  hid_log_count = 0;
static uint8_t  hid_log_next = 0;
static uint16_t hid_log_skipped = 0;
static uint8_t  hid_log_last_len = 0;
static bool     hid_log_have_last = false;
static uint32_t hid_log_reports = 0;
static uint32_t hid_log_zlps = 0;

// The log's memory. It is taken once, at init, and never freed.
static void hid_log_alloc(void) {
  if (hid_log) return;
  const size_t entries = (size_t)HIDCTL_LOG_ENTRIES * sizeof(HidLogEntry);
  hid_log = (HidLogEntry*)heap_caps_malloc(entries, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  hid_log_last = (uint8_t*)heap_caps_malloc(HIDCTL_MAX_REPORT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!hid_log || !hid_log_last) {
    HIDCTL_LOG("init: no room in PSRAM for the wire log");
    if (hid_log) { free(hid_log); hid_log = nullptr; }
    if (hid_log_last) { free(hid_log_last); hid_log_last = nullptr; }
  }
}

// Record a report that came in, or a zero-length packet.
static void hid_log_event(const uint8_t* data, uint16_t len, bool zero_length) {
  if (zero_length) {
    hid_log_skipped++;
    hid_log_zlps++;
    return;
  }
  hid_log_reports++;
  uint8_t n = (len > HIDCTL_MAX_REPORT) ? HIDCTL_MAX_REPORT : (uint8_t)len;
  if (hid_log_have_last && n == hid_log_last_len && hid_log_last &&
      memcmp(data, hid_log_last, n) == 0) {
    hid_log_skipped++;
    return;                       // the same report again
  }
  if (!hid_log) return;
  HidLogEntry* e = &hid_log[hid_log_next];
  e->skipped = hid_log_skipped;
  e->len = n;
  memcpy(e->data, data, n);
  hid_log_skipped = 0;
  hid_log_next = (uint8_t)((hid_log_next + 1) % HIDCTL_LOG_ENTRIES);
  if (hid_log_count < HIDCTL_LOG_ENTRIES) hid_log_count++;
  memcpy(hid_log_last, data, n);
  hid_log_last_len = n;
  hid_log_have_last = true;
}

// The log in chronological order: index 0 is the oldest entry kept.
// The status and length of the last sixteen IN completions, and how many
// completions have come back with a status that is not COMPLETED (those
// are the ones that tear the endpoint down).
static uint8_t  hid_in_status_ring[16];
static uint16_t hid_in_len_ring[16];
static uint8_t  hid_in_status_next = 0;
static volatile uint32_t hid_in_error_count = 0;

const HidLogEntry* hid_usb_log(uint8_t index) {
  if (!hid_log || index >= hid_log_count) return nullptr;
  return &hid_log[(hid_log_next + HIDCTL_LOG_ENTRIES - hid_log_count + index) % HIDCTL_LOG_ENTRIES];
}
uint8_t hid_usb_log_count(void) { return hid_log_count; }
uint32_t hid_usb_log_reports(void) { return hid_log_reports; }

// The status and length of the last sixteen IN completions, oldest first,
// and the count of completions that were not COMPLETED.
const uint8_t* hid_usb_in_statuses(uint8_t d, uint8_t* count, uint16_t** lengths) {
  HidDevice* dev = hid_dev_of(d);
  static uint16_t lens[16];
  uint8_t n = 0;
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t idx = (uint8_t)((hid_in_status_next + i) % 16);
    lens[i] = hid_in_len_ring[idx];
  }
  *lengths = lens;
  *count = 16;
  return hid_in_status_ring;
}
uint32_t hid_usb_in_errors(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return hid_in_error_count; }
uint32_t hid_usb_log_zlps(void) { return hid_log_zlps; }

// The stage of the class-request sequence: 0 = not started, 1 = the vendor
// request is due, 2 = SET_IDLE is due, 3 = done.
// The feature reports are asked for only when they are wanted: a PC's HID
// stack never asks for them.
// The SET_IDLE rate in milliseconds: 0 = report only when the state
// changes (what a PC asks for), and anything else = report that often
// whether or not anything happens.

void hid_usb_set_idle_rate(uint8_t d, uint16_t ms) {
  HidDevice* dev = hid_dev_of(d);
  dev->idle_rate_ms = ms;
  dev->idle_rate_changed = true;
}
uint16_t hid_usb_get_idle_rate(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->idle_rate_ms; }
// The vendor request 0xDD. The Traktor F1 answers it; the deck stalls it,
// and a stall on the control pipe is not something this host clears well.
// The class requests are on unless something turns them off. They go in the
// order a host that reads the encoder sends them (usb4.pcapng), and
// SET_IDLE goes FIRST there - it used to go last, and the encoder never
// reported.
// When to send the class requests, and whether they have been sent.
// The data a device-to-host control transfer brought back.
// The last report that went out on the OUT endpoint.
// The interface's OUT endpoint, if it has one (the F1's does: 0x01).
// What the device's configuration descriptor says about its interfaces.

const HidctlIfaceInfo* hid_usb_interfaces(uint8_t d, uint8_t* count) {
  HidDevice* dev = hid_dev_of(d);
  if (count) *count = dev->iface_count;
  return dev->ifaces;
}

int8_t hid_usb_last_report_status(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->last_report_status; }
uint32_t hid_usb_reports_ok(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->reports_ok; }
int8_t hid_usb_last_out_status(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->last_out_status; }
const uint8_t* hid_usb_readback(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = dev->readback_len;
  return dev->readback;
}

static uint8_t hid_report_desc[256] = {};      // the device's own report descriptor
static uint16_t hid_report_desc_len = 0;
// A device that appeared while another client was holding it.
static uint8_t hid_retry_addr = 0;
static uint32_t hid_retry_at = 0;
static bool hid_open_blocked = false;    // it was held, not a real fault
static uint8_t hid_retry_count = 0;
static volatile bool hid_in_error_recovery_needed = false;
// True for a device whose reports are always exactly one maximum packet
// long: the OpenDeck-family decks. A shorter packet is then a zero-length
// one, whatever the driver says its length is.
// How many bytes the interrupt IN transfer asks for. It has to be a
// whole number of maximum packets; asking for more than one leaves the
// device's shorter packet to end the transfer.
// Force the IN pipe to be torn down and armed again. A channel that
// came up in the wrong state - a data toggle out of step, say - never
// delivers, and this is the only way to make the host re-arm it.
// The IN transfer is armed from the main loop rather than from the
// client's event callback.

void hid_usb_set_in_xfer_len(uint8_t d, uint16_t n) {
  HidDevice* dev = hid_dev_of(d); dev->in_xfer_len = n; }
uint16_t hid_usb_get_in_xfer_len(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->in_xfer_len; }
void hid_usb_reset_in_pipe(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); dev->in_force_reset = true; }

// The SET_IDLE rate goes in the high byte of wValue and the report ID in
// the low one, which is what the HID specification says. This device is
// not a conformant HID stack, so the other way round is worth a try: the
// rate in the low byte.

void hid_usb_set_idle_rate_low_byte(uint8_t d, bool on) {
  HidDevice* dev = hid_dev_of(d); dev->idle_rate_low_byte = on; }
bool hid_usb_get_idle_rate_low_byte(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->idle_rate_low_byte; }

// Set the data toggle of the interrupt IN channel.
//
// The driver only writes the PID for control pipes, so a pipe that has
// never transferred keeps whatever the channel object had. If the
// device's toggle is out of step with the host's, the device ignores
// every token and nothing ever arrives - and nothing else can change
// it.
bool hid_usb_set_in_pid(uint8_t d, uint8_t pid) {
  HidDevice* dev = hid_dev_of(d);
  if (!dev->handle || !dev->in_ep) return false;
  return hidctlDwcSetInPid(dev->addr, (uint8_t)(dev->in_ep & 0x0F), pid);
}

// Ask the device for its firmware, the way the reference does: a
// GET_REPORT for the feature report with the ID 1, into a twenty-byte
// buffer. The deck's own answer to a vendor request was five bytes, so
// it does answer requests - this is the one the reference uses.
bool hid_usb_read_firmware(uint8_t d) {
  HidDevice* dev = hid_dev_of(d);
  if (!dev->handle) return false;
  dev->class_stage = 11;         // a stage of its own, below
  dev->class_wanted = true;
  return true;
}


static void hid_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void hid_in_transfer_cb(usb_transfer_t* xfer);
static void hid_ctrl_transfer_cb(usb_transfer_t* xfer);
static void hid_desc_transfer_cb(usb_transfer_t* xfer);
static void hid_class_transfer_cb(usb_transfer_t* xfer) {
  D->class_in_flight = false;
  if (xfer) {
    D->class_status = (int8_t)xfer->status;
    // Whatever came back with it (a vendor request answers with a byte).
    int n = (int)xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if (n > 0) {
      if (n > (int)sizeof(D->class_data)) n = (int)sizeof(D->class_data);
      memcpy(D->class_data, xfer->data_buffer + sizeof(usb_setup_packet_t), n);
      D->class_data_len = (uint8_t)n;
      // The feature report is 63 bytes - keep it where it cannot be
      // overwritten by the next request.
      if (n >= 32) {
        memcpy(D->feature_data, D->class_data, n);
        D->feature_len = (uint8_t)n;
      }
    }
    HIDCTL_LOG("class request answered: status=%d", (int)xfer->status);
  }
}
static bool hid_send_class_request(uint8_t bRequest, uint16_t wValue);
static bool hid_send_class_request_ex(uint8_t bmRequestType, uint8_t bRequest,
                                      uint16_t wValue, uint16_t wIndex,
                                      uint16_t wLength);

// The device's other interface (the F1's second one, class 0xFE). A PC's
// HID stack leaves it alone; we claimed it on a hunch, and the hunch was
// never tested. It can be released and claimed again at runtime.
// Off by default: a PC's HID stack leaves the F1's second interface alone.
static bool    hid_claim_other = false;

static void hid_claim_other_interfaces(usb_device_handle_t dev, uint8_t skip_iface) {
  if (!hid_claim_other) return;
  const usb_config_desc_t* cfg_desc = nullptr;
  if (usb_host_get_active_config_descriptor(dev, &cfg_desc) != ESP_OK || !cfg_desc) return;
  for (uint8_t other = 0; other < 8; other++) {
    if (other == skip_iface) continue;
    int ooff = 0;
    const usb_intf_desc_t* oifd =
        usb_parse_interface_descriptor(cfg_desc, other, 0, &ooff);
    if (!oifd) break;
    esp_err_t cerr = usb_host_interface_claim(hid_client, dev, other, 0);
    if (cerr == ESP_OK) {
      D->other_iface = other;
      D->other_claimed = true;
    }
    HIDCTL_LOG("configure: also claimed interface %u (class 0x%02X) -> %s",
               other, oifd->bInterfaceClass, cerr == ESP_OK ? "ok" : "failed");
  }
}

// Ask for the feature reports too (or stop asking).
void hid_usb_set_feature_reports(uint8_t d, bool want) {
  HidDevice* dev = hid_dev_of(d); dev->want_feature_reports = want; }
void hid_usb_set_class_vendor_request(uint8_t d, bool want) {
  HidDevice* dev = hid_dev_of(d); dev->class_vendor_request = want; }
bool hid_usb_get_class_vendor_request(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->class_vendor_request; }

// Send the wake-up packet again: {0xA0, 0x00, 0x00} on the OUT
// endpoint. It goes out once when the device is opened, and can be sent
// again from the web API.
void hid_usb_send_wakeup(uint8_t d) {
  HidDevice* dev = hid_dev_of(d);
  static const uint8_t wakeup[3] = {0xA0, 0x00, 0x00};
  if (!dev->out_xfer) return;
  hid_usb_queue_out(d, wakeup, sizeof(wakeup));
  HIDCTL_LOG("sent the wake-up packet A0 00 00");
}

// Claim or release the device's other interface.
void hid_usb_set_other_interface(uint8_t d, bool claim) {
  HidDevice* dev = hid_dev_of(d);
  if (!dev->handle) return;
  hid_claim_other = claim;
  if (!claim) {
    if (dev->other_claimed && dev->other_iface != 0xFF) {
      esp_err_t err = usb_host_interface_release(hid_client, dev->handle, dev->other_iface);
      HIDCTL_LOG("released interface %u -> %s", dev->other_iface, esp_err_to_name(err));
      dev->other_claimed = false;
      dev->other_iface = 0xFF;
    }
    return;
  }
  if (!dev->other_claimed) hid_claim_other_interfaces(dev->handle, dev->iface);
}

uint8_t hid_usb_other_iface(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->other_iface; }
bool    hid_usb_other_claimed(uint8_t d) { HidDevice* dev = hid_dev_of(d); return dev->other_claimed; }

// The F1's feature report, if the device answered the request for it.
const uint8_t* hid_usb_feature_data(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = dev->feature_len;
  return dev->feature_data;
}
static void hid_out_transfer_cb(usb_transfer_t* xfer);
static bool hid_out_send_next(uint8_t d);
static bool hid_out_send_next_piece(uint8_t d);
static void hid_out_watchdog(void);
static void hid_poll_device(void);
static esp_err_t hid_open_and_configure(uint8_t dev_addr);
static void      hid_close_device(void);

// ---------------------------------------------------------------------------
// hid_usb_init
// ---------------------------------------------------------------------------
void hid_usb_init(void) {
  if (hid_client) return;

  // The wire log, in PSRAM. It has to be taken before anything is
  // recorded, and before the USB host's own allocations: the log used to
  // sit in the internal heap, right below where the heap's high start
  // was, and every byte of it was a byte the USB stack could not have.
  hid_log_alloc();

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = hid_client_event_cb;
  cfg.async.callback_arg = nullptr;

  esp_err_t err = usb_host_client_register(&cfg, &hid_client);
  if (err != ESP_OK) {
    HIDCTL_LOG("init: usb_host_client_register failed: %s", esp_err_to_name(err));
    hid_client = nullptr;
    return;
  }

  HIDCTL_LOG("init: USB Host client ready");
}

// The transfers a device needs. They are per device - two devices on the
// bus cannot share one - and they are taken when a device opens, not
// once at init.
//
// Every transfer carries the device in its context, so its callback
// always knows which device it is about.
static bool hid_alloc_transfers(void) {
  esp_err_t err;

  err = usb_host_transfer_alloc(HIDCTL_MAX_REPORT, 0, &D->in_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("open: usb_host_transfer_alloc failed: %s", esp_err_to_name(err));
    D->in_xfer = nullptr;
    return false;
  }
  D->in_xfer->callback = hid_in_transfer_cb;
  D->in_xfer->context = D;

  // The control transfer for the output report (LEDs, display). The buffer
  // holds the 8-byte setup packet plus the report.
  err = usb_host_transfer_alloc(8 + HIDCTL_MAX_OUTPUT + 1, 0, &D->ctrl_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("open: usb_host_transfer_alloc(control) failed: %s", esp_err_to_name(err));
    D->ctrl_xfer = nullptr;
    return false;
  }
  D->ctrl_xfer->callback = hid_ctrl_transfer_cb;
  D->ctrl_xfer->context = D;

  // The class request transfer (setup packet plus the bytes that come
  // back). Without this there is no buffer to send a class request in -
  // which is why SET_IDLE was never sent at all. It has to hold the
  // F1's 63-byte feature report too.
  err = usb_host_transfer_alloc(8 + 72, 0, &D->class_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("open: usb_host_transfer_alloc(class) failed: %s", esp_err_to_name(err));
    D->class_xfer = nullptr;
  } else {
    D->class_xfer->callback = hid_class_transfer_cb;
    D->class_xfer->context = D;
  }

  // The report descriptor transfer (setup packet + up to 512 bytes).
  err = usb_host_transfer_alloc(8 + 512, 0, &D->desc_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("open: usb_host_transfer_alloc(descriptor) failed: %s", esp_err_to_name(err));
    D->desc_xfer = nullptr;
  } else {
    D->desc_xfer->callback = hid_desc_transfer_cb;
    D->desc_xfer->context = D;
  }
  return true;
}

// ---------------------------------------------------------------------------
// hid_usb_poll
// ---------------------------------------------------------------------------
void hid_usb_poll(void) {
  if (!hid_client) return;

  usb_host_client_handle_events(hid_client, 0);

  // A device that was held by another client when it appeared: try again.
  // Only that case - a device that opens and then fails to be claimed is
  // a real fault, and trying it again only leaks channels.
  if (hid_retry_addr && hid_retry_count < 20 && millis() - hid_retry_at > 500) {
    hid_retry_at = millis();
    hid_retry_count++;
    if (hid_open_and_configure(hid_retry_addr) == ESP_OK) {
      HIDCTL_LOG("event: the device at addr %u was released - claimed",
                 hid_retry_addr);
      hid_retry_addr = 0;
      hid_retry_count = 0;
      HIDCTL_LOG("connect (retry): device %u, VID=0x%04X PID=0x%04X",
                 (unsigned)HIDCTL_D, (unsigned)D->vid, (unsigned)D->pid);
      const uint8_t dev = HIDCTL_D;
      const uint16_t vid = D->vid;
      const uint16_t pid = D->pid;
      HIDCTL_D = dev;
      if (hidctlUsermodPtr) hidctlUsermodPtr->onDeviceConnected(dev, vid, pid);
    }
  }

  // Each device in turn. The board has a hub, so a control surface, a
  // keyboard and a mouse can all be on the bus at once, and each of them
  // has its own transfers and its own framing.
  for (uint8_t i = 0; i < HIDCTL_MAX_DEVICES; i++) {
    if (!hid_dev[i].used) continue;
    HIDCTL_D = i;
    hid_poll_device();
  }
}

// ---------------------------------------------------------------------------
// The work for one device
// ---------------------------------------------------------------------------
//
// The class requests, the IN transfer's arming, and the watchdog on a
// transfer that never finishes. It is one device's worth of work, and it
// runs for each device in turn.
static void hid_poll_device(void) {
  // A transfer that never finishes would block every later packet.
  hid_out_watchdog();

  // The recovery, when a pipe has wedged. One step per pass: the flush's
  // cancellation has to be delivered before the interface can be released,
  // and the release has to have happened before the pipe can be claimed
  // again.
  if (D->out_recovery) {
    switch (D->out_recovery) {
      case 1: {   // halt: the pipe stops moving
        esp_err_t err = usb_host_endpoint_halt(D->handle, D->out_ep);
        HIDCTL_LOG("OUT endpoint (device %u): halt -> %s",
                   (unsigned)HIDCTL_D, esp_err_to_name(err));
        D->out_recovery = 2;
        break;
      }
      case 2: {   // flush: the queued transfer is cancelled
        esp_err_t err = usb_host_endpoint_flush(D->handle, D->out_ep);
        HIDCTL_LOG("OUT endpoint (device %u): flush -> %s",
                   (unsigned)HIDCTL_D, esp_err_to_name(err));
        D->out_recovery = 3;
        break;
      }
      case 3: {   // clear: the only thing that makes the pipe usable again
        esp_err_t err = usb_host_endpoint_clear(D->handle, D->out_ep);
        HIDCTL_LOG("OUT endpoint (device %u): clear -> %s",
                   (unsigned)HIDCTL_D, esp_err_to_name(err));
        if (err == ESP_OK) {
          // The pipe is idle again. The transfers are re-pointed at it and
          // the queue is left alone: the packets in it are still the right
          // ones.
          D->out_failed = false;
          D->out_recovery = 0;
          D->out_xfer->device_handle = D->handle;
          D->out_xfer->bEndpointAddress = D->out_ep;
          D->out_xfer->num_bytes = 0;
          D->out_in_flight = false;
          D->out_started_ms = 0;
        } else {
          // The pipe is still halted. The interface is taken and claimed
          // again, which gives the device a new pipe.
          D->out_recovery = 4;
        }
        break;
      }
      case 4: {   // reclaim: take the interface and claim it again
        if (D->out_in_flight) break;    // the cancellation has not arrived yet
        esp_err_t err = usb_host_interface_release(hid_client, D->handle, D->iface);
        if (err == ESP_OK) {
          err = usb_host_interface_claim(hid_client, D->handle, D->iface, 0);
        }
        HIDCTL_LOG("OUT endpoint (device %u): reclaim -> %s",
                   (unsigned)HIDCTL_D, esp_err_to_name(err));
        if (err == ESP_OK) {
          D->out_failed = false;
          D->out_recovery = 0;
          D->out_xfer->device_handle = D->handle;
          D->out_xfer->bEndpointAddress = D->out_ep;
          D->in_xfer->device_handle = D->handle;
          D->in_arm_pending = true;
        } else {
          // The interface cannot be taken. The device is closed and let
          // go: it re-enumerates, and the connect path opens it again with
          // fresh pipes.
          HIDCTL_LOG("OUT endpoint (device %u): the interface cannot be "
                     "taken, the device is reopened", (unsigned)HIDCTL_D);
          D->out_recovery = 5;
        }
        break;
      }
      case 5: {   // reopen: close the device and let it come back
        HIDCTL_LOG("OUT endpoint (device %u): the device is reopened",
                   (unsigned)HIDCTL_D);
        D->out_recovery = 0;
        hid_close_device();
        if (hidctlUsermodPtr) hidctlUsermodPtr->onDeviceDisconnected(HIDCTL_D);
        return;
      }
      default:
        D->out_recovery = 0;
        break;
    }
    return;
  }

  // The OUT pipe: the rest of a packet first, and then the next queued
  // packet, when the pipe is free. This is the ONLY place a transfer for
  // the OUT endpoint is submitted.
  if (!D->out_failed) {
    if (!hid_out_send_next_piece(HIDCTL_D)) hid_out_send_next(HIDCTL_D);
  }

  // ONE control transfer at a time. The control pipe is a single pipe: a
  // second transfer started while the first is in flight confuses both
  // ends.
  const bool pipe_free = !D->desc_in_flight && !D->class_in_flight;

  // The class requests, in the order the capture of a host that reads the
  // encoder shows (usb4.pcapng):
  //
  //   1  SET_IDLE          (0x21, 0x0A)  <- the FIRST thing it sends
  //   2  GET_DESCRIPTOR    the HID report descriptor
  //   3  GET_STATUS
  //   4  GET_DESCRIPTOR    the string descriptors
  //   5  the vendor request 0xDD
  //
  // The two feature reports the F1 declares but never sends (0xD0, 0xF0)
  // come last and only if they are asked for - a PC's HID stack never asks
  // for them.
  //
  // SET_IDLE goes first because that is where the capture has it. It used
  // to go last, and the encoder never reported.
  // A new SET_IDLE rate: the request goes out again, and the SET_IDLE is
  // the first thing in the sequence.
  if (D->idle_rate_changed && pipe_free) {
    D->idle_rate_changed = false;
    D->class_stage = 1;
    D->class_wanted = true;
  }
  if (D->class_wanted && D->class_stage == 1 && pipe_free) {
    D->class_stage = 2;
    // SET_IDLE. The rate is in the high byte of wValue: zero means
    // "report only when the state changes", which is what a PC's HID
    // stack asks for. A rate makes the device report whether or not
    // anything happens, which is how a device with a broken input path
    // can be told apart from a device nobody is touching.
    hid_send_class_request_ex(0x21, 0x0A,
                              D->idle_rate_low_byte
                                  ? (uint16_t)D->idle_rate_ms
                                  : (uint16_t)(D->idle_rate_ms << 8),
                              D->iface, 0);
  } else if (D->want_descriptors && pipe_free) {
    // The descriptors go out once the control pipe is free.
    D->want_descriptors = false;
    hid_usb_get_descriptor(HIDCTL_D, 0x80, 0x0200, 256, 0);   // the configuration
  } else if (D->class_wanted && D->class_stage >= 2 && pipe_free) {
    switch (D->class_stage) {
      case 2:
        D->class_stage = 3;
        // GET_STATUS
        hid_send_class_request_ex(0x80, 0x00, 0x0000, 0x0000, 2);
        break;
      case 3:
        D->class_stage = 4;
        // The string descriptors - the serial number among them. A PC's
        // HID stack reads them; the F1's teardown notes mention the serial
        // number as the only handshaking there is.
        // 255 would not fit the class transfer's buffer (8 + 72 bytes);
        // a string descriptor is a few bytes at most.
        hid_send_class_request_ex(0x80, 0x06, 0x0300, 0x0400, 64);
        break;
      case 4:
        D->class_stage = 5;
        hid_send_class_request_ex(0x80, 0x06, 0x0301, 0x0400, 64);
        break;
      case 5:
        D->class_stage = 6;
        hid_send_class_request_ex(0x80, 0x06, 0x0302, 0x0400, 64);
        break;
      case 6:
        D->class_stage = 7;
        hid_send_class_request_ex(0x80, 0x06, 0x0303, 0x0400, 64);
        break;
      case 7:
        D->class_stage = 8;
        // The vendor request a PC's HID stack makes.
        if (D->class_vendor_request) {
          hid_send_class_request_ex(0xC0, 0xDD, 0x0000, 0x0000, 1);
        }
        break;
      case 8:
        D->class_stage = 9;
        // GET_REPORT, feature report 0xD0: 63 bytes of device state.
        // A PC's HID stack never asks for these, so they are off by
        // default - they are diagnostics, and they are the one host-side
        // request the working PC does not make.
        if (D->want_feature_reports) {
          hid_send_class_request_ex(0xA1, 0x01, (uint16_t)((3 << 8) | 0xD0), D->iface, 63);
        }
        break;
      case 9:
        D->class_stage = 10;
        // GET_REPORT, feature report 0xF0: the knob and fader positions.
        if (D->want_feature_reports) {
          hid_send_class_request_ex(0xA1, 0x01, (uint16_t)((3 << 8) | 0xF0), D->iface, 8);
        }
        break;
      case 11:
        // The reference asks for the firmware with a GET_REPORT for the
        // feature report 1, into a twenty-byte buffer.
        D->class_stage = 12;
        hid_send_class_request_ex(0xA1, 0x01, 0x0301, D->iface, 20);
        break;
      default:
        break;
    }
  }

  // The IN transfer is armed from here and not from the event callback:
  // the previous session found that arming it from the callback stopped
  // the reports arriving.
  if (D->in_arm_pending && D->handle && D->in_xfer) {
    D->in_arm_pending = false;
    usb_transfer_t* x = D->in_xfer;
    x->num_bytes = D->in_xfer_len ? D->in_xfer_len
                                   : (D->in_mps ? D->in_mps : HIDCTL_MAX_REPORT);
    esp_err_t err = usb_host_transfer_submit(x);
    if (err != ESP_OK) {
      D->in_arm_pending = true;
      HIDCTL_LOG("IN: the transfer would not arm: %s", esp_err_to_name(err));
    } else {
      HIDCTL_LOG("IN: the transfer is armed for %u bytes", (unsigned)x->num_bytes);
    }
  }

  // A forced re-arm: the transfer is flushed (which cancels it, and the
  // callback is what clears the driver's in-flight mark) and then armed
  // again. NOT halted and NOT cleared: a periodic channel must not be
  // halted, and clearing the endpoint resets the data toggle, which
  // leaves the host out of step with the device.
  if (D->in_force_reset && D->handle && D->in_xfer) {
    D->in_force_reset = false;
    usb_host_endpoint_flush(D->handle, D->in_xfer->bEndpointAddress);
    HIDCTL_LOG("IN: the transfer is flushed and will be armed again");
  }

  // The IN transfer is re-armed when it completes.
  if (D->in_resubmit_needed && D->handle && D->in_xfer) {
    D->in_resubmit_needed = false;
    usb_transfer_t* x = D->in_xfer;
    if (x->device_handle) {
      x->num_bytes = D->in_xfer_len ? D->in_xfer_len
                                     : (D->in_mps ? D->in_mps : HIDCTL_MAX_REPORT);
      esp_err_t err = usb_host_transfer_submit(x);
      if (err != ESP_OK) {
        D->in_resubmit_needed = true;
        if (err == ESP_ERR_NOT_FINISHED) {
          // The transfer object is still marked in flight: the driver
          // refuses a second submit until the first one has reported.
          // That is not a fault and it needs no recovery - the callback
          // will come (or not) and the poll will try again.
          HIDCTL_DEBUG("IN re-submit: the transfer is still in flight");
        } else {
          // This one is worth a line: a submit that is refused means the
          // pipe is stuck, and the recovery path will keep trying.
          HIDCTL_LOG("IN re-submit failed: %s", esp_err_to_name(err));
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Client event callback
// ---------------------------------------------------------------------------
static void hid_client_event_cb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
  if (!msg) return;

  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    HIDCTL_LOG("event: NEW_DEV addr=%d", msg->new_dev.address);
    if (hid_open_and_configure(msg->new_dev.address) != ESP_OK) {
      if (hid_open_blocked) {
        // Another client (the MSC one opens every device to see if it is a
        // mass storage) still has it. Remember it and try again from the
        // main loop.
        HIDCTL_LOG("event: NEW_DEV addr=%d is held by another client - will retry",
                   msg->new_dev.address);
        hid_retry_addr = msg->new_dev.address;
        hid_retry_at = millis();
        hid_retry_count = 0;
      } else {
        HIDCTL_LOG("event: NEW_DEV addr=%d would not open - giving up",
                   msg->new_dev.address);
      }
      return;
    }
    {
      HIDCTL_LOG("connect: device %u, VID=0x%04X PID=0x%04X",
                 (unsigned)HIDCTL_D, (unsigned)D->vid, (unsigned)D->pid);
      // The usermod is told here and now, and not through the app queue:
      // the queue is five deep and the F1 fills it with reports fifty
      // times a second, so a connect message can be dropped - and a
      // control surface that is never told about is a control surface
      // that does not work. This callback runs in the main loop's task,
      // which is the task that owns the usermod.
      const uint8_t dev = HIDCTL_D;
      const uint16_t vid = D->vid;
      const uint16_t pid = D->pid;
      HIDCTL_D = dev;
      if (hidctlUsermodPtr) hidctlUsermodPtr->onDeviceConnected(dev, vid, pid);
    }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    // Which device went away? The event carries its handle, and every
    // device here has its own.
    uint8_t gone = 0xFF;
    for (uint8_t i = 0; i < HIDCTL_MAX_DEVICES; i++) {
      if (hid_dev[i].used && hid_dev[i].handle == msg->dev_gone.dev_hdl) {
        gone = i;
        break;
      }
    }
    if (gone == 0xFF) {
      HIDCTL_LOG("event: DEV_GONE for a device that is not ours");
      return;
    }
    HIDCTL_D = gone;
    HIDCTL_LOG("event: DEV_GONE (device %u)", (unsigned)gone);
    hid_close_device();
    // The usermod is told here and now, for the same reason as the
    // connect: a message can be dropped, and a surface that is not told
    // about stays on the bus for ever.
    if (hidctlUsermodPtr) hidctlUsermodPtr->onDeviceDisconnected(gone);
  }
}

// ---------------------------------------------------------------------------
// IN transfer callback — post the raw report
// ---------------------------------------------------------------------------
static void hid_in_transfer_cb(usb_transfer_t* xfer) {
  // Which device this transfer is about.
  if (xfer && xfer->context) {
    HidDevice* dev = (HidDevice*)xfer->context;
    HIDCTL_D = (uint8_t)(dev - hid_dev);
  }
  if (!xfer) return;

  // Every completion, logged: its status and its length. The zero-length
  // packet the F1 sends after a report may well arrive with a status
  // that is not COMPLETED, and a status that is not COMPLETED takes the
  // recovery path - which tears the endpoint down.
  {
    static uint8_t last[16];
    static uint8_t lastn = 0;
    last[lastn] = (uint8_t)xfer->status;
    lastn = (uint8_t)((lastn + 1) % 16);
    hid_in_status_ring[hid_in_status_next] = (uint8_t)xfer->status;
    hid_in_len_ring[hid_in_status_next] = (uint16_t)xfer->actual_num_bytes;
    hid_in_status_next = (uint8_t)((hid_in_status_next + 1) % 16);
    (void)last;
    // The first few completions are worth a line each: whether the
    // interrupt IN pipe completes at all is the question the deck's input
    // depends on, and a silent pipe looks exactly like a quiet device.
    static uint8_t logged = 0;
    if (logged < 8) {
      logged++;
      HIDCTL_LOG("IN completion %u (device %u): status=%d bytes=%u",
                 (unsigned)logged, (unsigned)HIDCTL_D, (int)xfer->status,
                 (unsigned)xfer->actual_num_bytes);
    }
  }
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED &&
      xfer->status != USB_TRANSFER_STATUS_CANCELED &&
      xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
    // A CANCELLED transfer is the result of our own flush, not a fault
    // of the device: counting it as an error sends the recovery path
    // round again, and the pipe never comes back.
    hid_in_error_count++;
  }

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    uint16_t n = (uint16_t)xfer->actual_num_bytes;
    // IDF v5.5 computes the byte count of a short interrupt IN packet -
    // and a zero-length packet is a short one - as (received + mps - 2),
    // so an idle zero-length packet arrives as mps-2 bytes. The deck's
    // reports are exactly one packet long, so anything shorter than the
    // maximum packet is not a report. (Only when the transfer asks for
    // one packet: asking for more changes the arithmetic, and the report
    // comes back with a length that is not the packet's.)
    if (D->in_exact_mps && !D->in_xfer_len && n != D->in_mps) {
      D->in_zlp_count++;
      hid_log_event(nullptr, 0, true);
      // A zero-length packet is the device saying it has nothing to say.
      // It is worth one line: on this deck it is the only thing that
      // arrives until a key is touched.
      static uint32_t zlp_lines = 0;
      if (zlp_lines < 4) {
        zlp_lines++;
        HIDCTL_LOG("IN: a zero-length packet (the device has nothing to say)");
      }
      hid_in_error_recovery_needed = false;
      D->in_resubmit_needed = true;
      return;
    }
    if (n > 0) {
      if (n > HIDCTL_MAX_REPORT) n = HIDCTL_MAX_REPORT;
      D->in_report_count++;
      // A report whose length differs from the last one is worth knowing
      // about: the HID stacks pad every report up to the report's size, but
      // the raw host sees what the device really sent - and the F1 may send
      // its wheel's position in a short packet of its own.
      {
        static uint16_t lastLen = 0;
        if (n != lastLen) {
          lastLen = n;
          HIDCTL_LOG("report length %u:", n);
          for (uint16_t i = 0; i < n && i < 12; i++) {
            HIDCTL_LOG(" %02X", xfer->data_buffer[i]);
          }
          HIDCTL_LOG("");
        }
      }
      app_message_t m = {};
      m.id = app_message_t::APP_HIDCTL_PACKET;
      m.data.hidctl.device = HIDCTL_D;
      m.data.hidctl.len = n;
      memcpy(m.data.hidctl.data, xfer->data_buffer, n);
      if (app_queue) xQueueSend(app_queue, &m, 0);
      hid_log_event(xfer->data_buffer, n, false);
    } else {
      // A ZERO-LENGTH PACKET is not an error. The device sends one to
      // end a transfer it did not fill, and the F1 sends one after every
      // report. Treating it as a failure tore the interrupt endpoint
      // down - halt, flush, clear - fifty times a second, which is a
      // hard reset of the device's endpoint state.
      D->in_zlp_count++;
      hid_log_event(nullptr, 0, true);
    }
    hid_in_error_recovery_needed = false;
  } else {
    // The pipe is HALTED after an error; hid_usb_poll() recovers it.
    // A CANCELLED transfer is our own flush coming back, and a device
    // that has gone away is not something to recover from: either one
    // would send the recovery path round for ever.
    HIDCTL_DEBUG("IN callback: status=%d bytes=%d (recover in poll)",
                 (int)xfer->status, (int)xfer->actual_num_bytes);
    if (xfer->status != USB_TRANSFER_STATUS_CANCELED &&
        xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
      hid_in_error_recovery_needed = true;
    }
  }

  D->in_resubmit_needed = true;
}

// ---------------------------------------------------------------------------
// Descriptor walking
// ---------------------------------------------------------------------------
static esp_err_t hid_open_and_configure(uint8_t dev_addr) {
  // Which device slot this one takes. The board has a hub, so a control
  // surface, a keyboard and a mouse can all be on the bus at once.
  int8_t slot = -1;
  for (uint8_t i = 0; i < HIDCTL_MAX_DEVICES; i++) {
    if (!hid_dev[i].used) { slot = (int8_t)i; break; }
  }
  if (slot < 0) {
    HIDCTL_LOG("configure: no room for another device");
    return ESP_ERR_NO_MEM;
  }
  HIDCTL_D = (uint8_t)slot;

  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(hid_client, dev_addr, &dev);
  if (err != ESP_OK) {
    HIDCTL_DEBUG("configure: device_open(addr=%d) failed: %s (owned elsewhere?)",
                 dev_addr, esp_err_to_name(err));
    // Another client has it. That is worth trying again: the other client
    // releases it when it finds the device is not for it.
    hid_open_blocked = true;
    return err;
  }
  hid_open_blocked = false;

  const usb_device_desc_t* dev_desc = nullptr;
  err = usb_host_get_device_descriptor(dev, &dev_desc);
  if (err != ESP_OK) {
    usb_host_device_close(hid_client, dev);
    return err;
  }
  D->vid  = dev_desc->idVendor;
  D->pid = dev_desc->idProduct;

  // How fast is the link? A full-speed link carries at most 64 bytes in an
  // interrupt packet, so a device whose endpoints claim 512 and 1024 can
  // only work at high speed - and a host that has quietly dropped to full
  // speed would explain a device that enumerates and then says nothing.
  {
    usb_device_info_t dev_info = {};
    if (usb_host_device_info(dev, &dev_info) == ESP_OK) {
      D->link_speed = (uint8_t)dev_info.speed;
    }
  }

  // Does any profile want this device? If not, let it go.
  uint8_t profile = 0xFF;
  if (hidctlUsermodPtr) profile = hidctlUsermodPtr->matchDevice(D->vid, D->pid);
  if (profile == 0xFF) {
    HIDCTL_DEBUG("configure: VID=0x%04X PID=0x%04X — no profile wants it, releasing",
                 D->vid, D->pid);
    usb_host_device_close(hid_client, dev);
    return ESP_FAIL;
  }
  HIDCTL_LOG("configure: VID=0x%04X PID=0x%04X — profile %u wants it",
             D->vid, D->pid, profile);

  // How the packets go on the wire is a property of the device, and it
  // has to be set before anything is sent.
  //
  // The deck's report ID byte is not on the wire - a PC's HID stack
  // strips it, and the device drops any frame whose first three bytes
  // are not "CRT". The report is 1024 bytes, the endpoint's maximum
  // packet is 1024, and the reference sends exactly that with no
  // terminator: the zero-length packet in a Windows capture is the
  // Windows HID stack flushing its report, not part of the protocol.
  //
  // The deck also stalls the vendor request 0xDD, which the F1 answers,
  // and a stall on the control pipe is not something this host clears
  // well. The reference only asks for that report off Windows, so it is
  // not asked for here either.
  if (hidctlUsermodPtr && hidctlUsermodPtr->profileIsOpendeck(profile)) {
    hid_usb_set_out_drop_leading(HIDCTL_D, true);
    hid_usb_set_out_shift(HIDCTL_D, false);
    hid_usb_set_out_zlp(HIDCTL_D, false);
    hid_usb_set_class_vendor_request(HIDCTL_D, false);
    D->in_exact_mps = true;
    HIDCTL_LOG("configure: deck framing: no report ID on the wire, "
               "1024-byte report, no terminator");
  } else {
    // A device that is not a deck gets the plain framing back: the
    // report-ID byte stays where it is.
    hid_usb_set_out_drop_leading(HIDCTL_D, false);
    hid_usb_set_out_shift(HIDCTL_D, false);
    hid_usb_set_out_zlp(HIDCTL_D, false);
    hid_usb_set_class_vendor_request(HIDCTL_D, true);
    D->in_exact_mps = false;
  }

  const usb_config_desc_t* cfg_desc = nullptr;
  err = usb_host_get_active_config_descriptor(dev, &cfg_desc);
  if (err != ESP_OK || !cfg_desc) {
    HIDCTL_LOG("configure: get_active_config_descriptor failed: %s", esp_err_to_name(err));
    usb_host_device_close(hid_client, dev);
    return ESP_FAIL;
  }

  // Find the HID interface (class 3) and its interrupt IN endpoint.
  int    claimed_iface = -1;
  uint8_t in_ep = 0;
  uint16_t in_mps = 0;
  uint8_t out_ep = 0;
  uint16_t out_mps = 0;

  for (uint8_t iface_num = 0; iface_num < 8; iface_num++) {
    int iface_off = 0;
    const usb_intf_desc_t* iface =
        usb_parse_interface_descriptor(cfg_desc, iface_num, 0, &iface_off);
    if (!iface) break;
    if (iface->bInterfaceClass != 0x03) continue;   // HID
    for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
      int ep_off = iface_off;
      const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
          iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
      if (!ep) continue;
      if (ep->bEndpointAddress & 0x80) {               // IN
        if ((ep->bmAttributes & 0x03) == 3 && !in_ep) {  // interrupt
          in_ep = ep->bEndpointAddress;
          in_mps = ep->wMaxPacketSize & 0x7FF;          // strip the mult bit
        }
      } else {                                          // OUT
        if (!out_ep) {
          out_ep = ep->bEndpointAddress;
          out_mps = ep->wMaxPacketSize & 0x7FF;
        }
      }
    }
    claimed_iface = iface_num;
    break;
  }

  if (claimed_iface < 0 || in_ep == 0) {
    HIDCTL_LOG("configure: no HID interface with an interrupt IN endpoint");
    usb_host_device_close(hid_client, dev);
    return ESP_FAIL;
  }

  err = usb_host_interface_claim(hid_client, dev, (uint8_t)claimed_iface, 0);
  if (err != ESP_OK) {
    HIDCTL_LOG("configure: interface_claim(iface=%d) failed: %s",
               claimed_iface, esp_err_to_name(err));
    usb_host_device_close(hid_client, dev);
    return ESP_FAIL;
  }

  D->handle  = dev;
  D->used    = true;
  D->in_ep    = in_ep;
  D->iface    = (uint8_t)claimed_iface;
  D->addr = dev_addr;
  D->in_mps   = (in_mps > HIDCTL_MAX_REPORT) ? HIDCTL_MAX_REPORT : in_mps;
  if (D->in_mps == 0) D->in_mps = HIDCTL_MAX_REPORT;

  HIDCTL_LOG("configure: CLAIMED iface=%d IN=0x%02X MPS=%u OUT=0x%02X MPS=%u",
             claimed_iface, in_ep, (unsigned)D->in_mps, out_ep, (unsigned)out_mps);

  // The OUT endpoint, if the interface has one. Some controllers take
  // their LED data here rather than over the control pipe.
  D->out_ep = out_ep;
  D->out_mps = out_mps;
  // The queue's slot size depends on the endpoint's packet size, so it is
  // set once the endpoint is known.
  if (!D->in_xfer) {
    // The transfers this device needs. Without them the device is
    // claimed but can do nothing.
    if (!hid_alloc_transfers()) {
      usb_host_device_close(hid_client, dev);
      D->used = false;
      return ESP_FAIL;
    }
  }
  // The outgoing queue, in PSRAM: the internal heap is nearly full and
  // the deck's packets are 1024 bytes each. The slot size is the device's
  // packet size, and there are enough slots for several pictures to be
  // waiting - which is what stops them going out one per pass of the
  // main loop.
  if (!D->out_q) {
    const uint16_t stride = (uint16_t)(2 + (out_mps > HIDCTL_MAX_LED ? out_mps : HIDCTL_MAX_LED));
    // The queue holds several seconds of reports, so a burst does not
    // throw them away.
    const uint8_t slots = 64;
    D->out_q = (uint8_t*)heap_caps_aligned_alloc(16, (size_t)stride * slots,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (D->out_q) {
      D->out_q_slots = slots;
      D->out_q_stride = stride;
      D->out_q_lock = xSemaphoreCreateMutex();
    } else {
      HIDCTL_LOG("open: the outgoing queue would not fit in PSRAM");
    }
  }
  if (out_ep && !D->out_xfer) {
    err = usb_host_transfer_alloc(HIDCTL_MAX_OUTPUT, 0, &D->out_xfer);
    if (err == ESP_OK) {
      D->out_xfer->callback = hid_out_transfer_cb;
      D->out_xfer->context = D;
    } else {
      HIDCTL_LOG("configure: OUT transfer alloc failed: %s", esp_err_to_name(err));
      D->out_xfer = nullptr;
    }
  }

  // The wake-up packet. Other hosts' drivers send one three-byte packet
  // on the OUT endpoint when they open the device - {0xA0, 0x00, 0x00} -
  // but the capture of a host that reads the encoder (usb4.pcapng) shows
  // no OUT packet at all. So it is NOT sent when the device is opened; it
  // can still be sent by hand from the web API.
  //   POST /json/state  {"HIDCTL":{"a0":true}}

  // Log every interface of the configuration, so it is clear what else the
  // device offers (the F1 has a second interface for the LEDs).
  D->iface_count = 0;
  for (uint8_t ifn = 0; ifn < 8; ifn++) {
    int ioff = 0;
    const usb_intf_desc_t* ifd =
        usb_parse_interface_descriptor(cfg_desc, ifn, 0, &ioff);
    if (!ifd) break;
    HidctlIfaceInfo* info = &D->ifaces[D->iface_count];
    info->num = ifn;
    info->cls = ifd->bInterfaceClass;
    info->sub = ifd->bInterfaceSubClass;
    info->proto = ifd->bInterfaceProtocol;
    info->ep_count = 0;
    for (uint8_t e = 0; e < ifd->bNumEndpoints; e++) {
      int eoff = ioff;
      const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
          ifd, e, cfg_desc->wTotalLength, &eoff);
      if (!ep) continue;
      if (info->ep_count < HIDCTL_MAX_IFACE_EPS) {
        info->ep_addr[info->ep_count] = ep->bEndpointAddress;
        info->ep_mps[info->ep_count] = ep->wMaxPacketSize;
        info->ep_attr[info->ep_count] = ep->bmAttributes;
        info->ep_count++;
      }
    }
    USER_PRINTF("[HIDCTL]   iface %u: class=0x%02X subclass=0x%02X proto=0x%02X eps=%u\n",
                ifn, ifd->bInterfaceClass, ifd->bInterfaceSubClass,
                ifd->bInterfaceProtocol, ifd->bNumEndpoints);
    for (uint8_t e = 0; e < info->ep_count; e++) {
      USER_PRINTF("[HIDCTL]     ep 0x%02X attr=0x%02X mps=%u\n",
                  info->ep_addr[e], info->ep_attr[e], (unsigned)info->ep_mps[e]);
    }
    D->iface_count++;
    if (D->iface_count >= HIDCTL_MAX_IFACES) break;
  }

  usb_transfer_t* x = D->in_xfer;
  if (x) {
    x->device_handle = dev;
    x->bEndpointAddress = in_ep;
    x->num_bytes = D->in_xfer_len ? D->in_xfer_len : D->in_mps;
    // The transfer is armed from the main loop, not from here: this runs
    // inside the client's event callback, and the previous session found
    // that a transfer armed from there never delivered.
    D->in_arm_pending = true;
  }
  HIDCTL_LOG("configure: the IN transfer will be armed from the main loop");

  // Claim the device's other interface as well. The F1 has a second
  // interface (class 0xFE) next to the HID one. A PC's HID stack leaves it
  // alone; we claimed it on a hunch that the wheel only reports to a host
  // that has claimed it - a hunch that was never tested, so it can be
  // turned off and on again from the web API.
  hid_claim_other_interfaces(dev, claimed_iface);

  // The descriptors of the whole configuration and of the other interface,
  // so what the device really offers can be seen.
  D->want_descriptors = true;
  // The class requests come after the descriptors, in the order a PC's HID
  // stack sends them - and SET_IDLE goes first among them.
  if (D->class_enabled) {
    D->class_stage = 1;
    D->class_wanted = true;
  }

  // The class requests a HID host sends once it owns the interface, a
  // moment later - the device is not ready the instant the interface is
  // claimed. They go on their own transfer: if the device does not answer
  // them the transfer never completes, and that must not then block the
  // control pipe.
  D->class_requests_at = millis() + 500;
  D->class_requests_done = false;
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Close device
// ---------------------------------------------------------------------------
static void hid_close_device(void) {
  // The transfers stay with the slot: they are freed only when the slot
  // is given up, and the next device to take the slot reuses them.
  if (!D->handle) { D->used = false; return; }
  usb_host_interface_release(hid_client, D->handle, D->iface);
  if (D->in_xfer) D->in_xfer->device_handle = nullptr;
  usb_host_device_close(hid_client, D->handle);
  if (D->out_q_lock) { vSemaphoreDelete(D->out_q_lock); D->out_q_lock = nullptr; }
  if (D->out_q) { free(D->out_q); D->out_q = nullptr; }
  D->out_q_slots = 0;
  D->out_q_count = 0;
  D->handle = nullptr;
  D->in_ep = 0;
  D->iface = 0;
  D->addr = 0;
  D->vid = 0;
  D->pid = 0;
  D->in_mps = 0;
  D->used = false;
}

bool hid_usb_connected(uint8_t d) {
  HidDevice* dev = hid_dev_of(d);
  return dev->handle != nullptr;
}

// ---------------------------------------------------------------------------
// The output report: LEDs and the display
// ---------------------------------------------------------------------------
// A HID SET_REPORT control transfer. The transfer buffer starts with the
// setup packet, then the report's DATA.
//
// bmRequestType 0x21 = host-to-device, class, interface
// bRequest      0x09 = SET_REPORT
// wValue        (0x02 << 8) | report_id  -> Output report, that ID
//
// IMPORTANT: the report ID does NOT go into the data. It goes in wValue,
// and the data stage carries only the report's bytes. Sending the ID in the
// data as well makes the device see a report one byte too long, and it
// answers with a STALL.
// ---------------------------------------------------------------------------
bool hid_usb_send_report(uint8_t d, uint8_t report_type, uint8_t report_id,
                         const uint8_t* data, uint16_t len) {
  HidDevice* dev = hid_dev_of(d);
  return hid_usb_send_setup(d, 0x21, 0x09, (uint16_t)(((report_type & 0x03) << 8) | report_id),
                            data, len);
}

// A control transfer with the setup packet spelled out. NI's own driver
// uses wValue = 0x0100 | reportID for its SET_REPORT, and 0x0200 |
// reportID for its GET_REPORT (a readback of the LED state), so the
// report-type field is not always what the HID spec would suggest.
bool hid_usb_send_setup(uint8_t d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        const uint8_t* data, uint16_t len) {
  HidDevice* dev = hid_dev_of(d);
  return hid_usb_send_setup_to(d, bmRequestType, bRequest, wValue, dev->iface, data, len);
}

// The same, but with an explicit interface index. NI's own driver sends its
// class requests to the device's DFU interface (interface 1), not to the
// HID one it reads the inputs from.
bool hid_usb_send_setup_to(uint8_t d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                           uint8_t wIndex, const uint8_t* data, uint16_t len) {
  HidDevice* dev = hid_dev_of(d);
  if (!dev->handle || !dev->ctrl_xfer || !data || len == 0) return false;
  if (len > HIDCTL_MAX_OUTPUT) return false;

  // One control transfer at a time.
  if (dev->ctrl_in_flight) {
    dev->ctrl_pending = true;
    return false;
  }

  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;
  setup.bRequest      = bRequest;
  setup.wValue        = wValue;
  setup.wIndex        = wIndex;
  setup.wLength       = len;

  memcpy(dev->ctrl_xfer->data_buffer, &setup, sizeof(setup));
  memcpy(dev->ctrl_xfer->data_buffer + sizeof(setup), data, len);
  dev->ctrl_xfer->num_bytes = sizeof(setup) + len;
  dev->ctrl_xfer->device_handle = dev->handle;

  dev->ctrl_in_flight = true;
  dev->ctrl_want_readback = (bmRequestType & 0x80) != 0;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, dev->ctrl_xfer);
  if (err != ESP_OK) {
    dev->ctrl_in_flight = false;
    HIDCTL_LOG("control transfer submit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool hid_usb_send_output_report(uint8_t d, uint8_t report_id, const uint8_t* data, uint16_t len) {
  HidDevice* dev = hid_dev_of(d);
  return hid_usb_send_report(d, 0x02, report_id, data, len);
}

// ---------------------------------------------------------------------------
// The interface's OUT endpoint
// ---------------------------------------------------------------------------
// Some controllers take their LED data on an OUT endpoint instead of over
// the control pipe. The F1's HID interface has one (0x01).
// Some devices only take their output report over the control pipe
// (SET_REPORT) and not over the interrupt OUT endpoint. Which one is
// used is a runtime choice, because it is a per-device thing and a build
// flag would cost a full rebuild to change.

void hid_usb_set_out_via_control(uint8_t d, bool on) {
  HidDevice* dev = hid_dev_of(d); dev->out_via_control = on; }

// The packet's first byte is a report ID. A host that has no report IDs
// (or one that strips it) sends the command from byte one, so the byte
// is optional.

// Move the report to the front of the packet and keep the packet's
// length, so the report is followed by one pad byte.
//
// The deck's report is 1024 bytes and its OUT endpoint's maximum packet
// is 1024, so the report fills a packet exactly and the transfer has to
// be ended by a short packet or the device never learns that it has the
// whole report. A zero-length packet would do it, but the DWC2's HAL
// asserts on one. A single pad byte is short as well.

void hid_usb_set_out_shift(uint8_t d, bool on) {
  HidDevice* dev = hid_dev_of(d); dev->out_shift = on; }
bool hid_usb_get_out_shift(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_shift; }

void hid_usb_set_out_drop_leading(uint8_t d, bool on) {
  HidDevice* dev = hid_dev_of(d); dev->out_drop_leading = on; }
bool hid_usb_get_out_drop_leading(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_drop_leading; }

// Cap on how many bytes of a packet go out. The deck's output report is
// 1024 bytes and its endpoint's maximum packet is 1024, so a 1025-byte
// packet goes out as two packets: 1024 and then a short one of a single
// byte. A Windows HID stack drops that trailing byte, so the device may
// only ever see 1024. The cap makes the same packet go out as one.

void hid_usb_set_out_cap(uint8_t d, uint16_t n) {
  HidDevice* dev = hid_dev_of(d); dev->out_cap = n; }
uint16_t hid_usb_get_out_cap(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_cap; }

// End the packet with a zero-length packet.
//
// The deck's report is 1024 bytes and its OUT endpoint's maximum packet
// is 1024, so a report of that size is not a short packet and the device
// cannot tell that it has the whole thing. A PC's HID stack sends a
// zero-length packet to end the transfer, and the capture of a host that
// drives the deck shows exactly that: 1024 bytes, then nothing, then the
// next report. Without it the device waits for the end of a report that
// never comes.

void hid_usb_set_out_zlp(uint8_t d, bool on) {
  HidDevice* dev = hid_dev_of(d); dev->out_zlp = on; }
bool hid_usb_get_out_zlp(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_zlp; }


bool hid_usb_get_out_via_control(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_via_control; }

// Queue one packet for a device's interrupt OUT endpoint.
//
// This is the producer's half: it copies the bytes and returns. It is safe
// from any task, and it never touches a transfer.
bool hid_usb_queue_out(uint8_t d, const uint8_t* data, uint16_t len) {
  HidDevice* dev = hid_dev_of(d);
  if (!dev->handle || !dev->out_xfer || !data || len == 0) { dev->out_refused_no_device++; return false; }
  // The framing is the device's, and it is applied here, so the bytes in
  // the queue are exactly the bytes that go on the wire.
  bool shifted = false;
  if (dev->out_drop_leading && len > 1) {
    if (dev->out_shift) {
      // The report moves to the front of the packet and the packet keeps
      // its length: the report is followed by one pad byte.
      shifted = true;
    } else {
      data++; len--;
    }
  }
  if (dev->out_via_control) {
    // Over the control pipe: the report ID goes in wValue and the data
    // has no ID in it.
    if (len < 2) return false;
    return hid_usb_send_output_report(d, data[0], data + 1, (uint16_t)(len - 1));
  }
  if (dev->out_cap && len > dev->out_cap) len = dev->out_cap;
  if (len > HIDCTL_MAX_OUTPUT) { dev->out_refused_too_long++; return false; }

  if (dev->out_q_lock) xSemaphoreTake(dev->out_q_lock, portMAX_DELAY);
  bool queued = false;
  if (len + 2 > dev->out_q_stride) {
    // The packet does not fit the slot. It would be written past the end
    // of it, which is how the board ended up crashing in a task that had
    // nothing to do with the deck.
    dev->out_refused_too_long++;
    if (dev->out_q_lock) xSemaphoreGive(dev->out_q_lock);
    HIDCTL_LOG("OUT endpoint (device %u): the packet is %u bytes and the "
               "slot holds %u", (unsigned)d, (unsigned)len,
               (unsigned)(dev->out_q_stride - 2));
    return false;
  }
  if (dev->out_q && dev->out_q_count < dev->out_q_slots) {
    uint8_t* slot = dev->out_q + (size_t)dev->out_q_head * dev->out_q_stride;
    if (shifted) {
      memcpy(slot + 2, data + 1, len - 1);
      slot[2 + len - 1] = 0x00;
    } else {
      memcpy(slot + 2, data, len);
    }
    slot[0] = (uint8_t)(len & 0xFF);
    slot[1] = (uint8_t)(len >> 8);
    dev->out_q_head = (uint16_t)((dev->out_q_head + 1) % dev->out_q_slots);
    dev->out_q_count++;
    queued = true;
    // A packet that ends exactly on the endpoint's packet size needs the
    // zero-length packet after it, or the device cannot tell that it has
    // the whole report. It goes into the queue as well, so the order on
    // the wire is the order they were queued in.
    if (dev->out_zlp && dev->out_mps && (len % dev->out_mps) == 0 &&
        dev->out_q_count < dev->out_q_slots) {
      uint8_t* z = dev->out_q + (size_t)dev->out_q_head * dev->out_q_stride;
      z[0] = 0; z[1] = 0;
      dev->out_q_head = (uint16_t)((dev->out_q_head + 1) % dev->out_q_slots);
      dev->out_q_count++;
    }
  } else {
    // The queue is full. The packet is dropped, and that is a lost frame:
    // it is counted, because that is how it is seen from outside.
    dev->out_refused_busy++;
  }
  if (dev->out_q_lock) xSemaphoreGive(dev->out_q_lock);
  if (!queued) {
    // The log is lossy and the console is read by a person: the first
    // drop and then every fiftieth are said, not every one.
    static uint16_t drops = 0;
    if ((drops++ % 50) == 0) {
      HIDCTL_LOG("OUT endpoint (device %u): the queue is full, a packet is "
                 "dropped (%u so far)", (unsigned)d, (unsigned)drops);
    }
    return false;
  }
  return true;
}

// The sender's half: take the next queued packet and submit it.
//
// This runs in the task that owns the USB stack, and nowhere else. The
// whole packet goes into one transfer - the controller splits it into the
// endpoint's packets itself - so there is no second piece to send from
// inside the completion callback, which is what the DWC2 HAL asserts on.
static bool hid_out_send_next(uint8_t d) {
  HidDevice* dev = hid_dev_of(d);
  if (dev->out_in_flight || !dev->out_xfer || !dev->out_q) return false;
  uint16_t len = 0;
  if (dev->out_q_lock) xSemaphoreTake(dev->out_q_lock, portMAX_DELAY);
  if (dev->out_q_count) {
    uint8_t* slot = dev->out_q + (size_t)dev->out_q_tail * dev->out_q_stride;
    len = (uint16_t)(slot[0] | (slot[1] << 8));
    // The copy is done under the lock: the slot is free once the count
    // drops, and the producer may take it.
    memcpy(dev->out_xfer->data_buffer, slot + 2, len);
    dev->out_q_tail = (uint16_t)((dev->out_q_tail + 1) % dev->out_q_slots);
    dev->out_q_count--;
  }
  if (dev->out_q_lock) xSemaphoreGive(dev->out_q_lock);
  if (!len) return false;

  // The packet goes out in pieces: the F1's 81-byte LED report on a
  // 64-byte endpoint never completes as one transfer. The first piece
  // goes now and the rest from the poll, one piece per pass.
  dev->out_total = len;
  dev->out_sent = 0;
  const uint16_t piece = dev->out_mps ? dev->out_mps : 64;
  dev->out_xfer->num_bytes = (piece < len) ? piece : len;
  dev->out_xfer->device_handle = dev->handle;
  dev->out_xfer->bEndpointAddress = dev->out_ep;

  // Keep the head of what went out, so it can be read over /json/ar when
  // something on the controller's display or its LEDs looks wrong.
  {
    uint16_t keep = len < HIDCTL_READBACK_BYTES ? len : HIDCTL_READBACK_BYTES;
    memcpy(dev->last_out_report, dev->out_xfer->data_buffer, keep);
    dev->last_out_len = (uint8_t)keep;
  }
  if (!dev->out_first_len) {
    uint16_t keep = len < sizeof(dev->out_first) ? len : sizeof(dev->out_first);
    memcpy(dev->out_first, dev->out_xfer->data_buffer, keep);
    dev->out_first_len = (uint8_t)keep;
  }

  dev->out_in_flight = true;
  // The deadline is armed HERE, on every submit. It used to be armed only
  // for the second piece of a packet, so the first piece - and the deck's
  // whole packet - could hang for ever with nothing watching it: the
  // device went quiet and every later packet was refused as "busy", with
  // no log line at all.
  dev->out_started_ms = millis();
  esp_err_t err = usb_host_transfer_submit(dev->out_xfer);
  if (err != ESP_OK) {
    dev->out_in_flight = false;
    dev->out_started_ms = 0;
    dev->out_submit_error = (int8_t)err;
    dev->out_refused_submit++;
    HIDCTL_LOG("OUT endpoint (device %u) submit failed: %s",
               (unsigned)d, esp_err_to_name(err));
    return false;
  }
  dev->out_submit_error = 0;
  dev->out_sent = dev->out_xfer->num_bytes;
  dev->out_bytes += dev->out_xfer->num_bytes;
  return true;
}

// The rest of a packet: the next piece, sent from the poll.
//
// It is not sent from the transfer's completion callback: a transfer
// submitted from inside the completion of another one is what the USB
// stack here does not survive - the DWC2 raises an error interrupt and
// the HAL asserts on it.
static bool hid_out_send_next_piece(uint8_t d) {
  HidDevice* dev = hid_dev_of(d);
  if (dev->out_in_flight || !dev->out_xfer) return false;
  if (dev->out_sent >= dev->out_total) return false;
  const uint16_t left = (uint16_t)(dev->out_total - dev->out_sent);
  const uint16_t piece = dev->out_mps ? dev->out_mps : 64;
  dev->out_xfer->num_bytes = (piece < left) ? piece : left;
  // The piece is already in the buffer; move it to the front.
  memmove(dev->out_xfer->data_buffer, dev->out_xfer->data_buffer + dev->out_sent,
          dev->out_xfer->num_bytes);
  dev->out_xfer->device_handle = dev->handle;
  dev->out_xfer->bEndpointAddress = dev->out_ep;
  dev->out_sent += dev->out_xfer->num_bytes;
  dev->out_in_flight = true;
  dev->out_started_ms = millis();
  esp_err_t err = usb_host_transfer_submit(dev->out_xfer);
  if (err != ESP_OK) {
    dev->out_in_flight = false;
    dev->out_started_ms = 0;
    dev->out_submit_error = (int8_t)err;
    dev->out_refused_submit++;
    HIDCTL_LOG("OUT endpoint (device %u) submit failed: %s",
               (unsigned)d, esp_err_to_name(err));
    return false;
  }
  dev->out_submit_error = 0;
  dev->out_bytes += dev->out_xfer->num_bytes;
  return true;
}

// End a packet with a zero-length packet.
//
// A device whose report is exactly the size of its endpoint's maximum
// packet cannot tell that it has the whole report: the packet is not a
// short one. A PC's HID stack ends the transfer with a zero-length
// packet, and the capture of a host that drives the deck shows it: a
// 1024-byte packet, then a zero-length one, then the next report.


// Send the next piece of a packet that goes out in pieces.


int8_t hid_usb_class_status(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->class_status; }

// The answer to the last class request, if it brought one back.
const uint8_t* hid_usb_class_data(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = dev->class_data_len;
  return dev->class_data;
}

// The stage of the class-request sequence: 0 = not started, 1 = the vendor
// request due, 2 = SET_IDLE due, 3 = done.
uint8_t hid_usb_class_stage(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->class_stage; }

// Ask for the class requests again - they can be sent by hand from the
// web API, so they can be tried again without a rebuild.
void hid_usb_request_class(uint8_t d) {
  HidDevice* dev = hid_dev_of(d);
  dev->class_stage = 1;
  dev->class_wanted = true;
  dev->class_enabled = true;
  dev->class_data_len = 0;
}

// Turn the class requests on or off. They are off by default: the F1
// stalls them, and a stalled control endpoint is not cleared here.
void hid_usb_set_class_requests(uint8_t d, bool want) {
  HidDevice* dev = hid_dev_of(d);
  dev->class_enabled = want;
  if (want) {
    hid_usb_request_class(d);
  } else {
    dev->class_wanted = false;
    dev->class_stage = 0;
  }
}

// The configuration descriptor, exactly as the device sent it.
const uint8_t* hid_usb_config_desc(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = (uint8_t)dev->config_desc_len;
  return dev->config_desc;
}

// The reports that came in, and the zero-length packets that followed
// them.
uint32_t hid_usb_report_count(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->in_report_count; }
uint32_t hid_usb_out_count(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_count; }
// Why the last OUT packet did not go out: the counts say which door it
// went out of.
uint32_t hid_usb_out_refused(uint8_t d, uint8_t which) {
  HidDevice* dev = hid_dev_of(d);
  switch (which) {
    case 0: return dev->out_refused_no_device;
    case 1: return dev->out_refused_too_long;
    case 2: return dev->out_refused_busy;
    case 3: return dev->out_refused_submit;
    default: return 0;
  }
}
int8_t hid_usb_out_submit_error(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_submit_error; }
uint32_t hid_usb_out_stuck(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_stuck; }
uint32_t hid_usb_ctrl_out_count(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->ctrl_out_count; }
const uint8_t* hid_usb_out_first(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = dev->out_first_len;
  return dev->out_first;
}
uint32_t hid_usb_out_bytes(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->out_bytes; }
// 0 = unknown, 1 = full speed, 2 = high speed.
uint8_t hid_usb_link_speed(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->link_speed; }
const uint8_t* hid_usb_report_desc(uint8_t d, uint16_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = hid_report_desc_len;
  return hid_report_desc;
}
// The head of the last packet that went out on the OUT endpoint.
const uint8_t* hid_usb_last_out_head(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = dev->last_out_len > 24 ? 24 : dev->last_out_len;
  return dev->last_out_report;
}
void hid_usb_out_watchdog(uint8_t d) {
  HIDCTL_D = d;
  hid_out_watchdog();
}
uint32_t hid_usb_zlp_count(uint8_t d) {
  HidDevice* dev = hid_dev_of(d); return dev->in_zlp_count; }

const uint8_t* hid_usb_last_out_report(uint8_t d, uint8_t* len) {
  HidDevice* dev = hid_dev_of(d);
  if (len) *len = dev->last_out_len;
  return dev->last_out_report;
}

static void hid_out_transfer_cb(usb_transfer_t* xfer) {
  // Which device this transfer is about.
  if (xfer && xfer->context) {
    HidDevice* dev = (HidDevice*)xfer->context;
    HIDCTL_D = (uint8_t)(dev - hid_dev);
  }
  if (xfer) {
    D->last_out_status = (int8_t)xfer->status;
    D->out_count++;
    // Only the failures are worth a line: the reports go out several times a
    // second and the console would drown in them.
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      HIDCTL_LOG("OUT endpoint (device %u) transfer status=%d, %d bytes",
                 (unsigned)HIDCTL_D, (int)xfer->status, (int)xfer->actual_num_bytes);
      // The packet did not go out. The pipe may be wedged, and the recovery
      // is run from the poll - one step per pass, because the flush's
      // cancellation has to be delivered before the interface can be
      // released.
      D->out_failed = true;
    }
  }
  D->out_in_flight = false;
  D->out_started_ms = 0;
  // NOTHING is submitted from here. A transfer submitted from inside the
  // completion of another one is what the USB stack here does not survive:
  // the DWC2 raises an error interrupt and the HAL asserts on it. The
  // next piece, and the next packet, go out from the poll instead.
}

// A transfer that never finishes would leave the OUT pipe stuck for ever
// and every later packet would be refused, so it is given a deadline.
#define HID_OUT_TIMEOUT_MS 2000

static void hid_out_watchdog(void) {
  if (!D->out_in_flight || !D->out_started_ms) return;
  if ((uint32_t)(millis() - D->out_started_ms) < HID_OUT_TIMEOUT_MS) return;
  D->out_stuck++;
  HIDCTL_LOG("OUT endpoint (device %u): the transfer did not finish in %u ms",
             (unsigned)HIDCTL_D, (unsigned)HID_OUT_TIMEOUT_MS);
  // The recovery is run from the poll, one step per pass. It used to be run
  // from here, and it threw away the result of every call: a clear that
  // failed left the pipe halted for ever, and every later submit then
  // failed with ESP_ERR_INVALID_STATE, so the device was gone until it was
  // unplugged.
  D->out_failed = true;
  D->out_recovery = 1;
}

static void hid_ctrl_transfer_cb(usb_transfer_t* xfer) {
  // Which device this transfer is about.
  if (xfer && xfer->context) {
    HidDevice* dev = (HidDevice*)xfer->context;
    HIDCTL_D = (uint8_t)(dev - hid_dev);
  }
  if (xfer && D->out_via_control) D->ctrl_out_count++;
  if (xfer) {
    D->last_report_status = (int8_t)xfer->status;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      HIDCTL_LOG("control transfer status=%d", (int)xfer->status);
    } else {
      D->reports_ok++;
      // A device-to-host transfer brings data back: that is the LED state
      // readback, so it is worth seeing.
      if (D->ctrl_want_readback && xfer->actual_num_bytes > 8) {
        int n = xfer->actual_num_bytes - 8;
        const uint8_t* d = xfer->data_buffer + 8;
        char line[128];
        int len = 0;
        len += snprintf(line + len, sizeof(line) - len, "readback %d bytes:", n);
        for (int i = 0; i < n && len < 100; i++) {
          len += snprintf(line + len, sizeof(line) - len, " %02X", d[i]);
        }
        USER_PRINTF("[HIDCTL] %s\n", line);
        // Keep the first bytes so they can be read over /json/ar.
        for (uint8_t i = 0; i < HIDCTL_READBACK_BYTES && i < n; i++) {
          D->readback[i] = d[i];
        }
        D->readback_len = (uint8_t)((n < HIDCTL_READBACK_BYTES) ? n : HIDCTL_READBACK_BYTES);
      }
    }
  }
  D->ctrl_in_flight = false;
  // If another paint asked while this one was in flight, the newer state
  // goes out on the next paint.
  D->ctrl_pending = false;
}

// ---------------------------------------------------------------------------
// The class requests a HID host sends after claiming an interface
// ---------------------------------------------------------------------------
// Windows' HID driver sends SET_PROTOCOL and SET_IDLE to the interface it
// claims. A device can wait for them - the F1 does not report its wheel's
// position until it has been told the host is there.
static bool hid_send_class_request(uint8_t bRequest, uint16_t wValue) {
  return hid_send_class_request_ex(0x21, bRequest, wValue, D->iface, 0);
}

// A class request, with every field of the setup packet spelled out.
// `bmRequestType` 0x21 is Host-to-device | Class | Interface; 0xC0 is
// Device-to-host | Class | Device. `wLength` bytes come back for a
// device-to-host request.
static bool hid_send_class_request_ex(uint8_t bmRequestType, uint8_t bRequest,
                                      uint16_t wValue, uint16_t wIndex,
                                      uint16_t wLength) {
  if (!D->handle || !D->class_xfer) return false;
  if (D->class_in_flight) return false;

  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;
  setup.bRequest      = bRequest;
  setup.wValue        = wValue;
  setup.wIndex        = wIndex;
  setup.wLength       = wLength;

  memcpy(D->class_xfer->data_buffer, &setup, sizeof(setup));
  // The transfer holds the setup packet AND the data that comes back.
  D->class_xfer->num_bytes = sizeof(setup) + wLength;
  D->class_xfer->device_handle = D->handle;

  D->class_in_flight = true;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, D->class_xfer);
  if (err != ESP_OK) {
    D->class_in_flight = false;
    HIDCTL_LOG("class request %02X submit failed: %s", bRequest, esp_err_to_name(err));
    return false;
  }
  HIDCTL_LOG("class request %02X (type %02X wValue %04X wIndex %u len %u) sent",
             bRequest, bmRequestType, wValue, wIndex, wLength);
  return true;
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
// The device itself knows what reports it has: the HID report descriptor
// says which report IDs and sizes the interface accepts. Fetching it is a
// standard GET_DESCRIPTOR (type 0x22) control transfer to the interface.
//
// `bmRequestType` is 0x80 for a device-type descriptor (the device
// descriptor) and 0x81 for an interface-type one (the HID report
// descriptor). `wValue` selects the descriptor: 0x0100 = device,
// 0x0200 = configuration, 0x2200 = HID report descriptor.
bool hid_usb_get_descriptor(uint8_t d, uint8_t bmRequestType, uint16_t wValue,
                            uint16_t wLength, uint8_t wIndex) {
  HidDevice* dev = hid_dev_of(d);
  if (!dev->handle || !dev->desc_xfer) return false;
  if (dev->desc_in_flight) return false;

  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;   // Device-to-host, Standard, Device or Interface
  setup.bRequest      = 0x06;   // GET_DESCRIPTOR
  setup.wValue        = wValue;
  setup.wIndex        = wIndex;
  setup.wLength       = wLength;

  memcpy(dev->desc_xfer->data_buffer, &setup, sizeof(setup));
  // The transfer has to be big enough for the setup packet AND the data
  // stage the device is going to send back.
  dev->desc_xfer->num_bytes = sizeof(setup) + wLength;
  dev->desc_xfer->device_handle = dev->handle;
  dev->desc_desc_wvalue = wValue;

  dev->desc_in_flight = true;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, dev->desc_xfer);
  if (err != ESP_OK) {
    dev->desc_in_flight = false;
    HIDCTL_LOG("GET_DESCRIPTOR(%04X) submit failed: %s", wValue, esp_err_to_name(err));
    return false;
  }
  return true;
}

static void hid_desc_transfer_cb(usb_transfer_t* xfer) {
  // Which device this transfer is about.
  if (xfer && xfer->context) {
    HidDevice* dev = (HidDevice*)xfer->context;
    HIDCTL_D = (uint8_t)(dev - hid_dev);
  }
  D->desc_in_flight = false;
  if (!xfer) return;
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    HIDCTL_LOG("GET_DESCRIPTOR(%04X) status=%d", D->desc_desc_wvalue, (int)xfer->status);
    return;
  }
  // The data follows the 8-byte setup packet.
  int n = xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
  if (n <= 0) {
    HIDCTL_LOG("GET_DESCRIPTOR(%04X): no data", D->desc_desc_wvalue);
    return;
  }
  const uint8_t* d = xfer->data_buffer + sizeof(usb_setup_packet_t);
  if (D->desc_desc_wvalue == 0x0200 && n <= (int)sizeof(D->config_desc)) {
    memcpy(D->config_desc, d, n);
    D->config_desc_len = (uint16_t)n;
  }
  if (D->desc_desc_wvalue == 0x2200) {
    // The device's own declaration of its reports: how big they are, which
    // report ID they carry and whether there is an OUTPUT report at all.
    if (n <= (int)sizeof(hid_report_desc)) {
      memcpy(hid_report_desc, d, n);
      hid_report_desc_len = (uint16_t)n;
    }
  }
  HIDCTL_LOG("descriptor %04X: %d bytes", D->desc_desc_wvalue, n);
  for (int i = 0; i < n; i += 16) {
    char line[96];
    int len = 0;
    len += snprintf(line + len, sizeof(line) - len, "  %04X: ", (unsigned)i);
    for (int j = i; j < i + 16 && j < n; j++) {
      len += snprintf(line + len, sizeof(line) - len, "%02X ", d[j]);
    }
    USER_PRINTF("%s\n", line);
  }
  // The report descriptor comes after the configuration: only one control
  // transfer can be in flight at a time, so it is asked for from here.
  // The interface index is 0 - the HID interface. A PC's HID stack asks
  // for it the same way; we had been asking interface 1, which is the
  // F1's MIDI interface.
  if (D->desc_desc_wvalue == 0x0200) {
    hid_usb_get_descriptor(HIDCTL_D, 0x81, 0x2200, 256, 0);
  }
}
