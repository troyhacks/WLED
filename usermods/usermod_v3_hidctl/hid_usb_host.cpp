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
static uint16_t hid_desc_desc_wvalue = 0;
static volatile bool hid_ctrl_in_flight = false;
static volatile bool hid_ctrl_pending = false;
// The last report's outcome, published over /json/ar (the console is not
// always available).
static volatile int8_t  hid_last_report_status = -1;
static volatile uint32_t hid_reports_ok = 0;
// What the device's configuration descriptor says about its interfaces.
static HidctlIfaceInfo hid_ifaces[HIDCTL_MAX_IFACES] = {};
static uint8_t hid_iface_count = 0;

const HidctlIfaceInfo* hid_usb_interfaces(uint8_t* count) {
  if (count) *count = hid_iface_count;
  return hid_ifaces;
}

int8_t hid_usb_last_report_status(void) { return hid_last_report_status; }
uint32_t hid_usb_reports_ok(void) { return hid_reports_ok; }

static volatile bool hid_in_resubmit_needed = false;
static volatile bool hid_in_error_recovery_needed = false;

static void hid_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void hid_in_transfer_cb(usb_transfer_t* xfer);
static void hid_ctrl_transfer_cb(usb_transfer_t* xfer);
static void hid_desc_transfer_cb(usb_transfer_t* xfer);
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

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    uint16_t n = (uint16_t)xfer->actual_num_bytes;
    if (n > HIDCTL_MAX_REPORT) n = HIDCTL_MAX_REPORT;
    app_message_t m = {};
    m.id = app_message_t::APP_HIDCTL_PACKET;
    m.data.hidctl.len = n;
    memcpy(m.data.hidctl.data, xfer->data_buffer, n);
    if (app_queue) xQueueSend(app_queue, &m, 0);
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
      if ((ep->bmAttributes & 0x03) != 3) continue;   // interrupt
      if (ep->bEndpointAddress & 0x80) {               // IN
        in_ep = ep->bEndpointAddress;
        in_mps = ep->wMaxPacketSize & 0x7FF;          // strip the mult bit
        break;
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

  HIDCTL_LOG("configure: CLAIMED iface=%d IN=0x%02X MPS=%u",
             claimed_iface, in_ep, (unsigned)hid_in_mps);

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
  if (!hid_dev_hdl || !hid_ctrl_xfer || !data || len == 0) return false;
  if (len > HIDCTL_MAX_OUTPUT) return false;

  // One control transfer at a time.
  if (hid_ctrl_in_flight) {
    hid_ctrl_pending = true;
    return false;
  }

  usb_setup_packet_t setup = {};
  setup.bmRequestType = 0x21;   // Host-to-device | Class | Interface
  setup.bRequest      = 0x09;   // SET_REPORT
  setup.wValue        = (uint16_t)(((report_type & 0x03) << 8) | report_id);
  setup.wIndex        = hid_iface;
  setup.wLength       = len;

  memcpy(hid_ctrl_xfer->data_buffer, &setup, sizeof(setup));
  memcpy(hid_ctrl_xfer->data_buffer + sizeof(setup), data, len);
  hid_ctrl_xfer->num_bytes = sizeof(setup) + len;
  hid_ctrl_xfer->device_handle = hid_dev_hdl;

  hid_ctrl_in_flight = true;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, hid_ctrl_xfer);
  if (err != ESP_OK) {
    hid_ctrl_in_flight = false;
    HIDCTL_LOG("SET_REPORT submit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool hid_usb_send_output_report(uint8_t report_id, const uint8_t* data, uint16_t len) {
  return hid_usb_send_report(0x02, report_id, data, len);
}

static void hid_ctrl_transfer_cb(usb_transfer_t* xfer) {
  if (xfer) {
    hid_last_report_status = (int8_t)xfer->status;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      HIDCTL_LOG("SET_REPORT transfer status=%d", (int)xfer->status);
    } else {
      hid_reports_ok++;
    }
  }
  hid_ctrl_in_flight = false;
  // If another paint asked while this one was in flight, the newer state
  // goes out on the next paint.
  hid_ctrl_pending = false;
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
  if (hid_ctrl_in_flight) return false;

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

  hid_ctrl_in_flight = true;
  esp_err_t err = usb_host_transfer_submit_control(hid_client, hid_desc_xfer);
  if (err != ESP_OK) {
    hid_ctrl_in_flight = false;
    HIDCTL_LOG("GET_DESCRIPTOR(%04X) submit failed: %s", wValue, esp_err_to_name(err));
    return false;
  }
  return true;
}

static void hid_desc_transfer_cb(usb_transfer_t* xfer) {
  hid_ctrl_in_flight = false;
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
}
