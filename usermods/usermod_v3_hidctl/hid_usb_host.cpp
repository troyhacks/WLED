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

#include <cstring>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

extern HidctlUsermod* hidctlUsermodPtr;

#define HIDCTL_LOG(fmt, ...) do { if (canUseSerial()) Serial.printf("[HIDCTL] " fmt "\n", ##__VA_ARGS__); } while (0)
#define HIDCTL_DEBUG(fmt, ...) DEBUG_PRINTF("[HIDCTL] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static usb_host_client_handle_t hid_client = nullptr;
static usb_device_handle_t     hid_dev_hdl = nullptr;
static uint8_t                 hid_in_ep = 0;
static uint8_t                 hid_iface = 0;
static uint8_t                 hid_cur_addr = 0;
static uint16_t                hid_vendor = 0;
static uint16_t                hid_product = 0;
static uint16_t                hid_in_mps = 0;

static usb_transfer_t* hid_in_xfer = nullptr;

// The output path: one SET_REPORT control transfer at a time.
static usb_transfer_t* hid_ctrl_xfer = nullptr;
// A separate transfer for the one-off GET_DESCRIPTOR of the report
// descriptor (its own callback, and a bigger buffer).
static usb_transfer_t* hid_desc_xfer = nullptr;
// A transfer of its own for the class requests: if the device does not
// answer them the transfer never completes, and it must not then block the
// rest of the control pipe.
static usb_transfer_t* hid_class_xfer = nullptr;
static uint16_t hid_desc_desc_wvalue = 0;
static volatile bool hid_ctrl_in_flight = false;
static volatile bool hid_ctrl_pending = false;
// The last report's outcome, published over /json/ar (the console is not
// always available).
static volatile int8_t  hid_last_report_status = -1;
static volatile bool   hid_want_descriptors = false;

// The configuration descriptor, as the device sent it. The parse walks it,
// but the raw bytes settle every question about it - how many interfaces
// there really are, and how many endpoints each one has.
static uint8_t  hid_config_desc[256] = {};
static uint16_t hid_config_desc_len = 0;

// How many reports came in, and how many zero-length packets followed
// them. The F1 sends a ZLP after every report.
static volatile uint32_t hid_in_report_count = 0;
static volatile uint32_t hid_in_zlp_count = 0;
// The status of the last class request (-1 = none sent, 4 = stalled).
static volatile int8_t  hid_class_status = -1;

// The answer to the last class request, if it brought one back.
static uint8_t hid_class_data[72] = {};
static uint8_t hid_class_data_len = 0;

// The F1's 63-byte feature report (report ID 0xD0). The device declares
// it and never sends it on its own; it answers when it is asked.
static uint8_t hid_feature_data[72] = {};
static uint8_t hid_feature_len = 0;

// ---------------------------------------------------------------------------
// The wire log
// ---------------------------------------------------------------------------
// Every report the device sends, and every zero-length packet that follows
// one, so the traffic can be examined afterwards. Only reports that DIFFER
// from the one before are kept - the wheel and the knobs sit still most of
// the time - and each entry says how many packets came in between.
static constexpr uint8_t HIDCTL_LOG_ENTRIES = 48;
static HidLogEntry hid_log[HIDCTL_LOG_ENTRIES];
static uint8_t  hid_log_count = 0;
static uint8_t  hid_log_next = 0;
static uint16_t hid_log_skipped = 0;
static uint8_t  hid_log_last[HIDCTL_MAX_REPORT];
static uint8_t  hid_log_last_len = 0;
static bool     hid_log_have_last = false;
static uint32_t hid_log_reports = 0;
static uint32_t hid_log_zlps = 0;

// Record a report that came in, or a zero-length packet.
static void hid_log_event(const uint8_t* data, uint16_t len, bool zero_length) {
  if (zero_length) {
    hid_log_skipped++;
    hid_log_zlps++;
    return;
  }
  hid_log_reports++;
  uint8_t n = (len > HIDCTL_MAX_REPORT) ? HIDCTL_MAX_REPORT : (uint8_t)len;
  if (hid_log_have_last && n == hid_log_last_len &&
      memcmp(data, hid_log_last, n) == 0) {
    hid_log_skipped++;
    return;                       // the same report again
  }
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
  if (index >= hid_log_count) return nullptr;
  return &hid_log[(hid_log_next + HIDCTL_LOG_ENTRIES - hid_log_count + index) % HIDCTL_LOG_ENTRIES];
}
uint8_t hid_usb_log_count(void) { return hid_log_count; }
uint32_t hid_usb_log_reports(void) { return hid_log_reports; }

// The status and length of the last sixteen IN completions, oldest first,
// and the count of completions that were not COMPLETED.
const uint8_t* hid_usb_in_statuses(uint8_t* count, uint16_t** lengths) {
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
uint32_t hid_usb_in_errors(void) { return hid_in_error_count; }
uint32_t hid_usb_log_zlps(void) { return hid_log_zlps; }

// The stage of the class-request sequence: 0 = not started, 1 = the vendor
// request is due, 2 = SET_IDLE is due, 3 = done.
// The feature reports are asked for only when they are wanted: a PC's HID
// stack never asks for them.
static bool hid_want_feature_reports = false;
static uint8_t hid_class_stage = 0;
static bool    hid_class_in_flight = false;
static bool    hid_class_wanted = false;
// The class requests are on unless something turns them off. They go in the
// order a host that reads the encoder sends them (usb4.pcapng), and
// SET_IDLE goes FIRST there - it used to go last, and the encoder never
// reported.
static bool    hid_class_enabled = true;
// When to send the class requests, and whether they have been sent.
static uint32_t        hid_class_requests_at = 0;
static bool            hid_class_requests_done = false;
static volatile bool   hid_desc_in_flight = false;
static volatile uint32_t hid_reports_ok = 0;
static volatile int8_t  hid_last_out_status = -1;
// The data a device-to-host control transfer brought back.
static volatile bool   hid_ctrl_want_readback = false;
static uint8_t  hid_readback[HIDCTL_READBACK_BYTES] = {};
static uint8_t  hid_readback_len = 0;
// The last report that went out on the OUT endpoint.
static uint8_t  hid_last_out_report[HIDCTL_READBACK_BYTES] = {};
static uint8_t  hid_last_out_len = 0;
// The interface's OUT endpoint, if it has one (the F1's does: 0x01).
static usb_transfer_t* hid_out_xfer = nullptr;
static uint8_t  hid_out_ep = 0;
static uint16_t hid_out_mps = 0;
static volatile bool hid_out_in_flight = false;
// What the device's configuration descriptor says about its interfaces.
static HidctlIfaceInfo hid_ifaces[HIDCTL_MAX_IFACES] = {};
static uint8_t hid_iface_count = 0;

const HidctlIfaceInfo* hid_usb_interfaces(uint8_t* count) {
  if (count) *count = hid_iface_count;
  return hid_ifaces;
}

int8_t hid_usb_last_report_status(void) { return hid_last_report_status; }
uint32_t hid_usb_reports_ok(void) { return hid_reports_ok; }
int8_t hid_usb_last_out_status(void) { return hid_last_out_status; }
const uint8_t* hid_usb_readback(uint8_t* len) {
  if (len) *len = hid_readback_len;
  return hid_readback;
}

static volatile bool hid_in_resubmit_needed = false;
static volatile bool hid_in_error_recovery_needed = false;


static void hid_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void hid_in_transfer_cb(usb_transfer_t* xfer);
static void hid_ctrl_transfer_cb(usb_transfer_t* xfer);
static void hid_desc_transfer_cb(usb_transfer_t* xfer);
static void hid_class_transfer_cb(usb_transfer_t* xfer) {
  hid_class_in_flight = false;
  if (xfer) {
    hid_class_status = (int8_t)xfer->status;
    // Whatever came back with it (a vendor request answers with a byte).
    int n = (int)xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if (n > 0) {
      if (n > (int)sizeof(hid_class_data)) n = (int)sizeof(hid_class_data);
      memcpy(hid_class_data, xfer->data_buffer + sizeof(usb_setup_packet_t), n);
      hid_class_data_len = (uint8_t)n;
      // The feature report is 63 bytes - keep it where it cannot be
      // overwritten by the next request.
      if (n >= 32) {
        memcpy(hid_feature_data, hid_class_data, n);
        hid_feature_len = (uint8_t)n;
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
static uint8_t hid_other_iface = 0xFF;
static bool    hid_other_claimed = false;
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
      hid_other_iface = other;
      hid_other_claimed = true;
    }
    HIDCTL_LOG("configure: also claimed interface %u (class 0x%02X) -> %s",
               other, oifd->bInterfaceClass, cerr == ESP_OK ? "ok" : "failed");
  }
}

// Ask for the feature reports too (or stop asking).
void hid_usb_set_feature_reports(bool want) { hid_want_feature_reports = want; }

// Send the wake-up packet again: {0xA0, 0x00, 0x00} on the OUT
// endpoint. It goes out once when the device is opened, and can be sent
// again from the web API.
void hid_usb_send_wakeup(void) {
  static const uint8_t wakeup[3] = {0xA0, 0x00, 0x00};
  if (!hid_out_xfer) return;
  hid_usb_send_out_endpoint(wakeup, sizeof(wakeup));
  HIDCTL_LOG("sent the wake-up packet A0 00 00");
}

// Claim or release the device's other interface.
void hid_usb_set_other_interface(bool claim) {
  if (!hid_dev_hdl) return;
  hid_claim_other = claim;
  if (!claim) {
    if (hid_other_claimed && hid_other_iface != 0xFF) {
      esp_err_t err = usb_host_interface_release(hid_client, hid_dev_hdl, hid_other_iface);
      HIDCTL_LOG("released interface %u -> %s", hid_other_iface, esp_err_to_name(err));
      hid_other_claimed = false;
      hid_other_iface = 0xFF;
    }
    return;
  }
  if (!hid_other_claimed) hid_claim_other_interfaces(hid_dev_hdl, hid_iface);
}

uint8_t hid_usb_other_iface(void) { return hid_other_iface; }
bool    hid_usb_other_claimed(void) { return hid_other_claimed; }

// The F1's feature report, if the device answered the request for it.
const uint8_t* hid_usb_feature_data(uint8_t* len) {
  if (len) *len = hid_feature_len;
  return hid_feature_data;
}
static void hid_out_transfer_cb(usb_transfer_t* xfer);
static esp_err_t hid_open_and_configure(uint8_t dev_addr);
static void      hid_close_device(void);

// ---------------------------------------------------------------------------
// hid_usb_init
// ---------------------------------------------------------------------------
void hid_usb_init(void) {
  if (hid_client) return;

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

  err = usb_host_transfer_alloc(HIDCTL_MAX_REPORT, 0, &hid_in_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("init: usb_host_transfer_alloc failed: %s", esp_err_to_name(err));
    hid_in_xfer = nullptr;
    return;
  }
  hid_in_xfer->callback = hid_in_transfer_cb;
  hid_in_xfer->context = nullptr;

  // The control transfer for the output report (LEDs, display). The buffer
  // holds the 8-byte setup packet plus the report.
  err = usb_host_transfer_alloc(8 + HIDCTL_MAX_OUTPUT + 1, 0, &hid_ctrl_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("init: usb_host_transfer_alloc(control) failed: %s", esp_err_to_name(err));
    hid_ctrl_xfer = nullptr;
    return;
  }
  hid_ctrl_xfer->callback = hid_ctrl_transfer_cb;
  hid_ctrl_xfer->context = nullptr;

  // The class request transfer (setup packet plus the bytes that come
  // back). Without this there is no buffer to send a class request in -
  // which is why SET_IDLE was never sent at all. It has to hold the
  // F1's 63-byte feature report too.
  err = usb_host_transfer_alloc(8 + 72, 0, &hid_class_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("init: usb_host_transfer_alloc(class) failed: %s", esp_err_to_name(err));
    hid_class_xfer = nullptr;
  } else {
    hid_class_xfer->callback = hid_class_transfer_cb;
    hid_class_xfer->context = nullptr;
  }

  // The report descriptor transfer (setup packet + up to 512 bytes).
  err = usb_host_transfer_alloc(8 + 512, 0, &hid_desc_xfer);
  if (err != ESP_OK) {
    HIDCTL_LOG("init: usb_host_transfer_alloc(descriptor) failed: %s", esp_err_to_name(err));
    hid_desc_xfer = nullptr;
  } else {
    hid_desc_xfer->callback = hid_desc_transfer_cb;
    hid_desc_xfer->context = nullptr;
  }

  HIDCTL_LOG("init: USB Host client ready");
}

// ---------------------------------------------------------------------------
// hid_usb_poll
// ---------------------------------------------------------------------------
void hid_usb_poll(void) {
  if (!hid_client) return;

  usb_host_client_handle_events(hid_client, 0);

  // ONE control transfer at a time. The control pipe is a single pipe: a
  // second transfer started while the first is in flight confuses both
  // ends.
  const bool pipe_free = !hid_desc_in_flight && !hid_class_in_flight;

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
  if (hid_class_wanted && hid_class_stage == 1 && pipe_free) {
    hid_class_stage = 2;
    hid_send_class_request_ex(0x21, 0x0A, 0x0000, hid_iface, 0);   // SET_IDLE
  } else if (hid_want_descriptors && pipe_free) {
    // The descriptors go out once the control pipe is free.
    hid_want_descriptors = false;
    hid_usb_get_descriptor(0x80, 0x0200, 256, 0);   // the configuration
  } else if (hid_class_wanted && hid_class_stage >= 2 && pipe_free) {
    switch (hid_class_stage) {
      case 2:
        hid_class_stage = 3;
        // GET_STATUS
        hid_send_class_request_ex(0x80, 0x00, 0x0000, 0x0000, 2);
        break;
      case 3:
        hid_class_stage = 4;
        // The string descriptors - the serial number among them. A PC's
        // HID stack reads them; the F1's teardown notes mention the serial
        // number as the only handshaking there is.
        // 255 would not fit the class transfer's buffer (8 + 72 bytes);
        // a string descriptor is a few bytes at most.
        hid_send_class_request_ex(0x80, 0x06, 0x0300, 0x0400, 64);
        break;
      case 4:
        hid_class_stage = 5;
        hid_send_class_request_ex(0x80, 0x06, 0x0301, 0x0400, 64);
        break;
      case 5:
        hid_class_stage = 6;
        hid_send_class_request_ex(0x80, 0x06, 0x0302, 0x0400, 64);
        break;
      case 6:
        hid_class_stage = 7;
        hid_send_class_request_ex(0x80, 0x06, 0x0303, 0x0400, 64);
        break;
      case 7:
        hid_class_stage = 8;
        // The vendor request a PC's HID stack makes.
        hid_send_class_request_ex(0xC0, 0xDD, 0x0000, 0x0000, 1);
        break;
      case 8:
        hid_class_stage = 9;
        // GET_REPORT, feature report 0xD0: 63 bytes of device state.
        // A PC's HID stack never asks for these, so they are off by
        // default - they are diagnostics, and they are the one host-side
        // request the working PC does not make.
        if (hid_want_feature_reports) {
          hid_send_class_request_ex(0xA1, 0x01, (uint16_t)((3 << 8) | 0xD0), hid_iface, 63);
        }
        break;
      case 9:
        hid_class_stage = 10;
        // GET_REPORT, feature report 0xF0: the knob and fader positions.
        if (hid_want_feature_reports) {
          hid_send_class_request_ex(0xA1, 0x01, (uint16_t)((3 << 8) | 0xF0), hid_iface, 8);
        }
        break;
      default:
        break;
    }
  }

  if (hid_in_resubmit_needed && hid_dev_hdl && hid_in_xfer) {
    hid_in_resubmit_needed = false;
    usb_transfer_t* x = hid_in_xfer;
    if (x->device_handle) {
      // Only reset the pipe after an error: on a clean completion the data
      // toggle has advanced and clearing the endpoint would desync us from
      // the device (every following packet would be dropped).
      if (hid_in_error_recovery_needed) {
        usb_host_endpoint_halt(x->device_handle, x->bEndpointAddress);
        usb_host_endpoint_flush(x->device_handle, x->bEndpointAddress);
        usb_host_endpoint_clear(x->device_handle, x->bEndpointAddress);
        hid_in_error_recovery_needed = false;
      }
      x->num_bytes = hid_in_mps ? hid_in_mps : HIDCTL_MAX_REPORT;
      esp_err_t err = usb_host_transfer_submit(x);
      if (err != ESP_OK) {
        hid_in_resubmit_needed = true;
        hid_in_error_recovery_needed = true;
        HIDCTL_DEBUG("IN re-submit failed: %s", esp_err_to_name(err));
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
    if (hid_open_and_configure(msg->new_dev.address) == ESP_OK) {
      app_message_t m = {};
      m.id = app_message_t::APP_HIDCTL_DEVICE_CONNECTED;
      m.data.hidctl_device_info.vid = hid_vendor;
      m.data.hidctl_device_info.pid = hid_product;
      strncpy(m.data.hidctl_device_info.name, "HID control surface",
              sizeof(m.data.hidctl_device_info.name) - 1);
      m.data.hidctl_device_info.name[sizeof(m.data.hidctl_device_info.name) - 1] = '\0';
      if (app_queue) xQueueSend(app_queue, &m, 0);
    }
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    HIDCTL_LOG("event: DEV_GONE");
    hid_close_device();
    app_message_t m = {};
    m.id = app_message_t::APP_HIDCTL_DEVICE_DISCONNECTED;
    if (app_queue) xQueueSend(app_queue, &m, 0);
  }
}

// ---------------------------------------------------------------------------
// IN transfer callback — post the raw report
// ---------------------------------------------------------------------------
static void hid_in_transfer_cb(usb_transfer_t* xfer) {
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
  }
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) hid_in_error_count++;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    uint16_t n = (uint16_t)xfer->actual_num_bytes;
    if (n > 0) {
      if (n > HIDCTL_MAX_REPORT) n = HIDCTL_MAX_REPORT;
      hid_in_report_count++;
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
      hid_in_zlp_count++;
      hid_log_event(nullptr, 0, true);
    }
    hid_in_error_recovery_needed = false;
  } else {
    // The pipe is HALTED after an error; hid_usb_poll() recovers it.
    HIDCTL_DEBUG("IN callback: status=%d bytes=%d (recover in poll)",
                 (int)xfer->status, (int)xfer->actual_num_bytes);
    hid_in_error_recovery_needed = true;
  }

  hid_in_resubmit_needed = true;
}

// ---------------------------------------------------------------------------
// Descriptor walking
// ---------------------------------------------------------------------------
static esp_err_t hid_open_and_configure(uint8_t dev_addr) {
  usb_device_handle_t dev = nullptr;
  esp_err_t err = usb_host_device_open(hid_client, dev_addr, &dev);
  if (err != ESP_OK) {
    HIDCTL_DEBUG("configure: device_open(addr=%d) failed: %s (owned elsewhere?)",
                 dev_addr, esp_err_to_name(err));
    return err;
  }

  const usb_device_desc_t* dev_desc = nullptr;
  err = usb_host_get_device_descriptor(dev, &dev_desc);
  if (err != ESP_OK) {
    usb_host_device_close(hid_client, dev);
    return err;
  }
  hid_vendor  = dev_desc->idVendor;
  hid_product = dev_desc->idProduct;

  // Does any profile want this device? If not, let it go.
  uint8_t profile = 0xFF;
  if (hidctlUsermodPtr) profile = hidctlUsermodPtr->matchDevice(hid_vendor, hid_product);
  if (profile == 0xFF) {
    HIDCTL_DEBUG("configure: VID=0x%04X PID=0x%04X — no profile wants it, releasing",
                 hid_vendor, hid_product);
    usb_host_device_close(hid_client, dev);
    return ESP_FAIL;
  }
  HIDCTL_LOG("configure: VID=0x%04X PID=0x%04X — profile %u wants it",
             hid_vendor, hid_product, profile);

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

  hid_dev_hdl  = dev;
  hid_in_ep    = in_ep;
  hid_iface    = (uint8_t)claimed_iface;
  hid_cur_addr = dev_addr;
  hid_in_mps   = (in_mps > HIDCTL_MAX_REPORT) ? HIDCTL_MAX_REPORT : in_mps;
  if (hid_in_mps == 0) hid_in_mps = HIDCTL_MAX_REPORT;

  HIDCTL_LOG("configure: CLAIMED iface=%d IN=0x%02X MPS=%u OUT=0x%02X MPS=%u",
             claimed_iface, in_ep, (unsigned)hid_in_mps, out_ep, (unsigned)out_mps);

  // The OUT endpoint, if the interface has one. Some controllers take
  // their LED data here rather than over the control pipe.
  hid_out_ep = out_ep;
  hid_out_mps = out_mps;
  if (out_ep && !hid_out_xfer) {
    err = usb_host_transfer_alloc(HIDCTL_MAX_OUTPUT, 0, &hid_out_xfer);
    if (err == ESP_OK) {
      hid_out_xfer->callback = hid_out_transfer_cb;
      hid_out_xfer->context = nullptr;
    } else {
      HIDCTL_LOG("configure: OUT transfer alloc failed: %s", esp_err_to_name(err));
      hid_out_xfer = nullptr;
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
  hid_iface_count = 0;
  for (uint8_t ifn = 0; ifn < 8; ifn++) {
    int ioff = 0;
    const usb_intf_desc_t* ifd =
        usb_parse_interface_descriptor(cfg_desc, ifn, 0, &ioff);
    if (!ifd) break;
    HidctlIfaceInfo* info = &hid_ifaces[hid_iface_count];
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
    hid_iface_count++;
    if (hid_iface_count >= HIDCTL_MAX_IFACES) break;
  }

  usb_transfer_t* x = hid_in_xfer;
  if (x) {
    x->device_handle = dev;
    x->bEndpointAddress = in_ep;
    x->num_bytes = hid_in_mps;
    err = usb_host_transfer_submit(x);
    if (err != ESP_OK) {
      HIDCTL_LOG("configure: initial IN submit failed: %s", esp_err_to_name(err));
    }
  }
  HIDCTL_LOG("configure: 1 IN transfer submitted");

  // Claim the device's other interface as well. The F1 has a second
  // interface (class 0xFE) next to the HID one. A PC's HID stack leaves it
  // alone; we claimed it on a hunch that the wheel only reports to a host
  // that has claimed it - a hunch that was never tested, so it can be
  // turned off and on again from the web API.
  hid_claim_other_interfaces(dev, claimed_iface);

  // The descriptors of the whole configuration and of the other interface,
  // so what the device really offers can be seen.
  hid_want_descriptors = true;
  // The class requests come after the descriptors, in the order a PC's HID
  // stack sends them - and SET_IDLE goes first among them.
  if (hid_class_enabled) {
    hid_class_stage = 1;
    hid_class_wanted = true;
  }

  // The class requests a HID host sends once it owns the interface, a
  // moment later - the device is not ready the instant the interface is
  // claimed. They go on their own transfer: if the device does not answer
  // them the transfer never completes, and that must not then block the
  // control pipe.
  hid_class_requests_at = millis() + 500;
  hid_class_requests_done = false;
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Close device
// ---------------------------------------------------------------------------
static void hid_close_device(void) {
  if (!hid_dev_hdl) return;
  usb_host_interface_release(hid_client, hid_dev_hdl, hid_iface);
  if (hid_in_xfer) hid_in_xfer->device_handle = nullptr;
  usb_host_device_close(hid_client, hid_dev_hdl);
  hid_dev_hdl = nullptr;
  hid_in_ep = 0;
  hid_iface = 0;
  hid_cur_addr = 0;
  hid_vendor = 0;
  hid_product = 0;
  hid_in_mps = 0;
}

bool hid_usb_connected(void) {
  return hid_dev_hdl != nullptr;
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
bool hid_usb_send_report(uint8_t report_type, uint8_t report_id,
                         const uint8_t* data, uint16_t len) {
  return hid_usb_send_setup(0x21, 0x09, (uint16_t)(((report_type & 0x03) << 8) | report_id),
                            data, len);
}

// A control transfer with the setup packet spelled out. NI's own driver
// uses wValue = 0x0100 | reportID for its SET_REPORT, and 0x0200 |
// reportID for its GET_REPORT (a readback of the LED state), so the
// report-type field is not always what the HID spec would suggest.
bool hid_usb_send_setup(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        const uint8_t* data, uint16_t len) {
  return hid_usb_send_setup_to(bmRequestType, bRequest, wValue, hid_iface, data, len);
}

// The same, but with an explicit interface index. NI's own driver sends its
// class requests to the device's DFU interface (interface 1), not to the
// HID one it reads the inputs from.
bool hid_usb_send_setup_to(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                           uint8_t wIndex, const uint8_t* data, uint16_t len) {
  if (!hid_dev_hdl || !hid_ctrl_xfer || !data || len == 0) return false;
  if (len > HIDCTL_MAX_OUTPUT) return false;

  // One control transfer at a time.
  if (hid_ctrl_in_flight) {
    hid_ctrl_pending = true;
    return false;
  }

  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;
  setup.bRequest      = bRequest;
  setup.wValue        = wValue;
  setup.wIndex        = wIndex;
  setup.wLength       = len;

  memcpy(hid_ctrl_xfer->data_buffer, &setup, sizeof(setup));
  memcpy(hid_ctrl_xfer->data_buffer + sizeof(setup), data, len);
  hid_ctrl_xfer->num_bytes = sizeof(setup) + len;
  hid_ctrl_xfer->device_handle = hid_dev_hdl;

  hid_ctrl_in_flight = true;
  hid_ctrl_want_readback = (bmRequestType & 0x80) != 0;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, hid_ctrl_xfer);
  if (err != ESP_OK) {
    hid_ctrl_in_flight = false;
    HIDCTL_LOG("control transfer submit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool hid_usb_send_output_report(uint8_t report_id, const uint8_t* data, uint16_t len) {
  return hid_usb_send_report(0x02, report_id, data, len);
}

// ---------------------------------------------------------------------------
// The interface's OUT endpoint
// ---------------------------------------------------------------------------
// Some controllers take their LED data on an OUT endpoint instead of over
// the control pipe. The F1's HID interface has one (0x01).
bool hid_usb_send_out_endpoint(const uint8_t* data, uint16_t len) {
  if (!hid_dev_hdl || !hid_out_xfer || !data || len == 0) return false;
  if (len > HIDCTL_MAX_OUTPUT) return false;
  if (hid_out_in_flight) return false;

  memcpy(hid_out_xfer->data_buffer, data, len);
  hid_out_xfer->num_bytes = len;
  hid_out_xfer->device_handle = hid_dev_hdl;
  hid_out_xfer->bEndpointAddress = hid_out_ep;

  // Keep a copy of what went out, so it can be read over /json/ar when
  // something on the controller's display or its LEDs looks wrong.
  if (len <= HIDCTL_READBACK_BYTES) {
    memcpy(hid_last_out_report, data, len);
    hid_last_out_len = (uint8_t)len;
  }

  hid_out_in_flight = true;
  esp_err_t err = usb_host_transfer_submit(hid_out_xfer);
  if (err != ESP_OK) {
    hid_out_in_flight = false;
    HIDCTL_LOG("OUT endpoint submit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

int8_t hid_usb_class_status(void) { return hid_class_status; }

// The answer to the last class request, if it brought one back.
const uint8_t* hid_usb_class_data(uint8_t* len) {
  if (len) *len = hid_class_data_len;
  return hid_class_data;
}

// The stage of the class-request sequence: 0 = not started, 1 = the vendor
// request due, 2 = SET_IDLE due, 3 = done.
uint8_t hid_usb_class_stage(void) { return hid_class_stage; }

// Ask for the class requests again - they can be sent by hand from the
// web API, so they can be tried again without a rebuild.
void hid_usb_request_class(void) {
  hid_class_stage = 1;
  hid_class_wanted = true;
  hid_class_enabled = true;
  hid_class_data_len = 0;
}

// Turn the class requests on or off. They are off by default: the F1
// stalls them, and a stalled control endpoint is not cleared here.
void hid_usb_set_class_requests(bool want) {
  hid_class_enabled = want;
  if (want) {
    hid_usb_request_class();
  } else {
    hid_class_wanted = false;
    hid_class_stage = 0;
  }
}

// The configuration descriptor, exactly as the device sent it.
const uint8_t* hid_usb_config_desc(uint8_t* len) {
  if (len) *len = (uint8_t)hid_config_desc_len;
  return hid_config_desc;
}

// The reports that came in, and the zero-length packets that followed
// them.
uint32_t hid_usb_report_count(void) { return hid_in_report_count; }
uint32_t hid_usb_zlp_count(void) { return hid_in_zlp_count; }

const uint8_t* hid_usb_last_out_report(uint8_t* len) {
  if (len) *len = hid_last_out_len;
  return hid_last_out_report;
}

static void hid_out_transfer_cb(usb_transfer_t* xfer) {
  if (xfer) {
    hid_last_out_status = (int8_t)xfer->status;
    // Only the failures are worth a line: the reports go out several times a
    // second and the console would drown in them.
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      HIDCTL_LOG("OUT endpoint transfer status=%d, %d bytes",
                 (int)xfer->status, (int)xfer->actual_num_bytes);
    }
  }
  hid_out_in_flight = false;
}

static void hid_ctrl_transfer_cb(usb_transfer_t* xfer) {
  if (xfer) {
    hid_last_report_status = (int8_t)xfer->status;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      HIDCTL_LOG("control transfer status=%d", (int)xfer->status);
    } else {
      hid_reports_ok++;
      // A device-to-host transfer brings data back: that is the LED state
      // readback, so it is worth seeing.
      if (hid_ctrl_want_readback && xfer->actual_num_bytes > 8) {
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
          hid_readback[i] = d[i];
        }
        hid_readback_len = (uint8_t)((n < HIDCTL_READBACK_BYTES) ? n : HIDCTL_READBACK_BYTES);
      }
    }
  }
  hid_ctrl_in_flight = false;
  // If another paint asked while this one was in flight, the newer state
  // goes out on the next paint.
  hid_ctrl_pending = false;
}

// ---------------------------------------------------------------------------
// The class requests a HID host sends after claiming an interface
// ---------------------------------------------------------------------------
// Windows' HID driver sends SET_PROTOCOL and SET_IDLE to the interface it
// claims. A device can wait for them - the F1 does not report its wheel's
// position until it has been told the host is there.
static bool hid_send_class_request(uint8_t bRequest, uint16_t wValue) {
  return hid_send_class_request_ex(0x21, bRequest, wValue, hid_iface, 0);
}

// A class request, with every field of the setup packet spelled out.
// `bmRequestType` 0x21 is Host-to-device | Class | Interface; 0xC0 is
// Device-to-host | Class | Device. `wLength` bytes come back for a
// device-to-host request.
static bool hid_send_class_request_ex(uint8_t bmRequestType, uint8_t bRequest,
                                      uint16_t wValue, uint16_t wIndex,
                                      uint16_t wLength) {
  if (!hid_dev_hdl || !hid_class_xfer) return false;
  if (hid_class_in_flight) return false;

  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;
  setup.bRequest      = bRequest;
  setup.wValue        = wValue;
  setup.wIndex        = wIndex;
  setup.wLength       = wLength;

  memcpy(hid_class_xfer->data_buffer, &setup, sizeof(setup));
  // The transfer holds the setup packet AND the data that comes back.
  hid_class_xfer->num_bytes = sizeof(setup) + wLength;
  hid_class_xfer->device_handle = hid_dev_hdl;

  hid_class_in_flight = true;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, hid_class_xfer);
  if (err != ESP_OK) {
    hid_class_in_flight = false;
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
bool hid_usb_get_descriptor(uint8_t bmRequestType, uint16_t wValue,
                            uint16_t wLength, uint8_t wIndex) {
  if (!hid_dev_hdl || !hid_desc_xfer) return false;
  if (hid_desc_in_flight) return false;

  usb_setup_packet_t setup = {};
  setup.bmRequestType = bmRequestType;   // Device-to-host, Standard, Device or Interface
  setup.bRequest      = 0x06;   // GET_DESCRIPTOR
  setup.wValue        = wValue;
  setup.wIndex        = wIndex;
  setup.wLength       = wLength;

  memcpy(hid_desc_xfer->data_buffer, &setup, sizeof(setup));
  // The transfer has to be big enough for the setup packet AND the data
  // stage the device is going to send back.
  hid_desc_xfer->num_bytes = sizeof(setup) + wLength;
  hid_desc_xfer->device_handle = hid_dev_hdl;
  hid_desc_desc_wvalue = wValue;

  hid_desc_in_flight = true;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, hid_desc_xfer);
  if (err != ESP_OK) {
    hid_desc_in_flight = false;
    HIDCTL_LOG("GET_DESCRIPTOR(%04X) submit failed: %s", wValue, esp_err_to_name(err));
    return false;
  }
  return true;
}

static void hid_desc_transfer_cb(usb_transfer_t* xfer) {
  hid_desc_in_flight = false;
  if (!xfer) return;
  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    HIDCTL_LOG("GET_DESCRIPTOR(%04X) status=%d", hid_desc_desc_wvalue, (int)xfer->status);
    return;
  }
  // The data follows the 8-byte setup packet.
  int n = xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
  if (n <= 0) {
    HIDCTL_LOG("GET_DESCRIPTOR(%04X): no data", hid_desc_desc_wvalue);
    return;
  }
  const uint8_t* d = xfer->data_buffer + sizeof(usb_setup_packet_t);
  if (hid_desc_desc_wvalue == 0x0200 && n <= (int)sizeof(hid_config_desc)) {
    memcpy(hid_config_desc, d, n);
    hid_config_desc_len = (uint16_t)n;
  }
  HIDCTL_LOG("descriptor %04X: %d bytes", hid_desc_desc_wvalue, n);
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
  if (hid_desc_desc_wvalue == 0x0200) {
    hid_usb_get_descriptor(0x81, 0x2200, 256, 0);
  }
}
