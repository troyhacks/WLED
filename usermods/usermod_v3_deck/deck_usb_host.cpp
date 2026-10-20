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

// Every deck line carries millis(). Without it the log interleaves with the
// IDF's own timestamped errors ("E (3940) USBH: Dev 2 EP 0 STALL") and there is
// no way to tell whether a control request was refused instantly or sat for
// seconds first — which is the difference between "device says no" and "device
// never answered". That distinction matters, so it is paid for on every line.
#define DECK_LOG(fmt, ...)   do { if (canUseSerial()) Serial.printf("[DECK %6lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); } while (0)
// Phase 0 bring-up wants these visible on the bench, and gating them behind
// WLED_DEBUG means turning on a lot of unrelated serial noise to see them. They
// go quiet once the transport is proven and the usermod grows real logging.
#define DECK_DEBUG(fmt, ...) do { if (canUseSerial()) Serial.printf("[DECK %6lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__); } while (0)

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

static const char* deck_speed_str(usb_speed_t s) {
  switch (s) {
    case USB_SPEED_LOW:  return "LOW";
    case USB_SPEED_FULL: return "FULL";
    case USB_SPEED_HIGH: return "HIGH";
    default:             return "?";
  }
}

static const char* device_lookup(uint16_t vid, uint16_t pid) {
  for (const auto& d : kKnownDevices) {
    if (d.vid == vid && d.pid == pid) return d.name;
  }
  return nullptr;
}

// Scans forward from an interface descriptor for the HID descriptor (type 0x21)
// and returns its bcdHID.
//
// Note what is deliberately NOT here: akp05 selects its interface by usage page
// 0xFF00 / usage 1, and it would be useful to compare against that — but usage
// page and usage are not in the configuration descriptor. They are properties of
// the parsed HID *report* descriptor, which Linux hidapi derives and which is
// only reachable here via a GET_DESCRIPTOR control transfer. Reading them out of
// the 0x21 descriptor would print plausible-looking nonsense.
static bool deck_find_hid_desc(const usb_config_desc_t* cfg, int intf_off,
                               uint16_t* bcd_hid) {
  const uint8_t* raw = (const uint8_t*)cfg;
  int i = intf_off;
  while (i + 1 < cfg->wTotalLength) {
    uint8_t len = raw[i];
    if (len < 2 || i + len > cfg->wTotalLength) break;
    uint8_t type = raw[i + 1];
    if (type == 0x21 && len >= 9) {
      // bLength, bDescriptorType, bcdHID (2 bytes, little endian)
      if (bcd_hid) *bcd_hid = (uint16_t)(raw[i + 2] | (raw[i + 3] << 8));
      return true;
    }
    if (type == 0x09) break;                  // start of the next interface
    i += len;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
// Equal to the IN endpoint's max packet size (measured: IN 0x82/512). ESP-IDF
// wants a non-control transfer's num_bytes to be an integer multiple of MPS,
// and one full maximum packet is the smallest value that satisfies that — so
// 512 is both the safest and the exact report size.
static constexpr uint16_t IN_XFER_SIZE  = DeckProto::IN_REPORT_LEN;   // 512
// Every OUT record is exactly CMD_WIRE bytes (see deck_out_queue), so this is
// also what keeps the ring buffer reads record-aligned.
static constexpr uint16_t OUT_XFER_SIZE = DeckProto::CMD_WIRE;        // 1025

// OUT ring buffer. Sized for a full 15-key image push: 15 BAT headers plus
// their chunk streams plus a trailing STP comfortably exceeds 8 KB, so a full
// repaint can be queued in one go without blocking the caller.
static constexpr uint32_t OUT_RB_BYTES = 16384;
static uint8_t           deck_out_rb_storage[OUT_RB_BYTES] __attribute__((aligned(4)));
static StaticRingbuffer_t deck_out_rb_struct;
static RingbufHandle_t    deck_out_rb = nullptr;

// Control-transfer scratch: 8 SETUP bytes followed by an optional data stage.
// Large enough to pull back a full HID report descriptor.
static constexpr uint16_t DECK_CTRL_BUF = 8 + 512;

// The device drops the session if CONNECT is not seen roughly every 10 s.
static constexpr uint32_t KEEPALIVE_MS = 9000;

// Bring-up watchdog. Until the first report ever arrives, an armed IN URB that
// has produced nothing for this long gets the pipe cleared and a fresh URB
// armed, so the bench log distinguishes "this URB is wedged" from "this endpoint
// never delivers" — and succeeds outright if a fresh arm does produce data.
// Disabled permanently once a report lands, and capped, so it cannot spam the
// console during normal idle operation.
static constexpr uint32_t IN_WATCHDOG_MS   = 5000;
static constexpr uint32_t IN_WATCHDOG_MAX  = 5;

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

// Set by deck_open_and_configure, serviced from deck_usb_poll(). The HID class
// requests below block until the device answers, and open_and_configure runs
// inside the async client's event callback where blocking is not safe.
static volatile bool deck_hid_init_pending = false;

static usb_transfer_t* deck_in_xfer  = nullptr;
static usb_transfer_t* deck_out_xfer = nullptr;
static usb_transfer_t* deck_ctrl_xfer = nullptr;

// Set from the IN callback, acted on in deck_usb_poll(). Re-submitting from
// inside the callback runs within usb_host_client_handle_events(), where the
// DWC pipe may not yet accept a new URB.
static volatile bool in_resubmit_needed  = false;
// Set only when a transfer did NOT complete successfully. On a healthy URB the
// pipe's data toggle has advanced and must be left alone; clearing it would
// desync host and device and silently drop every following packet.
static volatile bool in_error_recovery_needed = false;
// Set by deck_open_and_configure, serviced by deck_usb_poll(). See the comment
// there: the IN URB is deliberately armed from the main loop, not from the
// client's event callback.
static volatile bool in_arm_pending = false;
// True from the moment we ask for a halt until the cancelled URB comes back
// through the callback. usb_host_endpoint_halt() is asynchronous — it completes
// by cancelling the in-flight transfer and reporting that cancellation in the
// callback — so submitting again before then races the pipe and fails with
// ESP_ERR_NOT_FINISHED. See deck_in_arm().
static volatile bool in_halt_pending = false;

static volatile bool out_in_flight = false;
static uint32_t     last_keepalive_ms = 0;

// Phase 0 only: counts transfers that actually completed, so the bench log can
// tell "the device never sent anything" apart from "a report arrived but did not
// match the ACK filter". An interrupt IN endpoint NAKs when the device has
// nothing to say, so a pending transfer with no completions is normal at idle.
// True while a submitted IN URB has not yet come back. An interrupt IN pipe
// with no data pending simply NAKs and stays armed, so this is expected to sit
// at 1 almost all the time — it only reads 0 if a transfer is somehow not in
// flight, which is the failure worth catching.
static volatile bool     in_flight = false;
// Set on claim when there is an OUT pipe to initialize; cleared once the init
// packets have drained, at which point the IN pipes are armed. See the block in
// deck_open_and_configure() and the gate in deck_usb_poll().
static volatile bool     deck_waiting_for_init = false;
// How many OUT packets the init sequence queues. The IN pipes are armed once
// deck_out_completed reaches this, which is how "the ring buffer has drained"
// is checked without touching the buffer itself — this IDF version has no
// xRingbufferIsEmpty, and peeking with xRingbufferReceiveUpTo would CONSUME a
// queued packet when one is present, silently dropping a command.
static volatile uint32_t deck_init_packets_expected = 0;
static volatile uint32_t deck_in_completed = 0;
static volatile uint32_t deck_out_completed = 0;

// ---- Interface 1 / EP 0x81: the one pipe never claimed. ----
//
// The device declares a second interface that nothing has ever touched:
//
//   iface 1 class=0x03 subclass=0x01 protocol=0x01  -> boot keyboard
//   ep 0x81 INTR mps=8 interval=10
//
// subclass 0x01 / protocol 0x01 is the HID boot-keyboard pair, and an 8-byte MPS
// is the size of a boot report. That is a real input pipe with a real interrupt
// endpoint, and the deck's own boot-keyboard emulation may well be where input
// first appears. It has never been claimed, never armed, and therefore cannot
// account for reports=0 — until now it simply was not in the running.
//
// It is armed as a SEPARATE transfer with its own counters rather than by
// switching deck_in_ep. The question is precisely which endpoint the device
// talks on, and swapping the single IN URB would only ever answer "0x82 or not
// 0x82" — the failure log already says not-0x82. Running both simultaneously
// distinguishes "the device never sends on ANY IN pipe" from "it sends on the
// one I was ignoring", in a single boot, with no extra cycle.
static usb_transfer_t* deck_aux_xfer = nullptr;
static volatile bool     aux_in_flight = false;
static volatile uint32_t deck_aux_completed = 0;
static volatile uint32_t deck_aux_bytes = 0;
static uint8_t  deck_aux_ep    = 0;
static uint8_t  deck_aux_iface = 0;
static volatile bool aux_arm_pending = false;
static volatile bool aux_resubmit_needed = false;
// Only 8 bytes of payload, but +8 so a control-style read could reuse the buffer.
static constexpr uint16_t AUX_XFER_SIZE = 8;

// Forward declarations: deck_usb_init() allocates and installs these callbacks
// well above where they are defined, matching how deck_in_arm/deck_in_transfer_cb
// are ordered in this file.
static void deck_aux_transfer_cb(usb_transfer_t* xfer);
static bool deck_aux_arm(void);
// Set once when the init sequence has drained; deck_usb_poll() sees it and
// submits a single GET_REPORT(Input) over EP0 to ask the device whether it has
// any buffered state worth reading. The interrupt pipe (EP 0x82) has been
// silent across every variant tested; this decouples "is anything there?" from
// "is the interrupt pipe wired?". See deck_usb_poll() for the gate.
static volatile bool     deck_post_init_report_query = false;
static uint32_t           last_status_ms     = 0;
static uint32_t           last_in_completed_ms = 0;
static uint32_t           in_watchdog_fires  = 0;

// Forward declarations
static void deck_client_event_cb(const usb_host_client_event_msg_t* msg, void* arg);
static void deck_in_transfer_cb(usb_transfer_t* xfer);
static void deck_out_transfer_cb(usb_transfer_t* xfer);
static esp_err_t deck_open_and_configure(uint8_t dev_addr);
static void      deck_close_device(void);
static void      deck_send_keepalive(void);
static void      deck_hid_class_init(void);
static bool      deck_in_arm(bool clear_first);

// ---------------------------------------------------------------------------
// HID class bring-up: SET_PROTOCOL then SET_IDLE on the claimed interface.
//
// This is the step that was missing. On Linux the kernel's HID driver issues
// SET_PROTOCOL(Report) and SET_IDLE(0) for every HID device with an interrupt
// IN endpoint during enumeration, so mirajazz never has to ask — it opens the
// device and reads. The ESP-IDF host stack has no equivalent: nothing sends
// those requests unless the application does. Left unset, the deck keeps its
// power-on state and its interrupt IN pipe NAKs forever, which is exactly what
// the bench showed — `reports=0` with the URB armed, indefinitely, while the
// OUT path completed normally.
//
// This IDF has no synchronous clients (usb_host_client_config_t::is_synchronous
// is documented "set to false for now"), so these go on the async client and
// complete from usb_host_client_handle_events() like any other transfer. That
// makes the sequence a small state machine pumped from deck_usb_poll() rather
// than a blocking loop.
//
// Both requests are class/interface/host-to-device with no data stage, so each
// is one 8-byte SETUP packet on EP0 and nothing else.
// ---------------------------------------------------------------------------
struct DeckHidInitReq {
  uint8_t     bm_request_type;
  uint8_t     request;
  uint16_t    value;
  uint16_t    length;      // wLength
  bool        reads_data;  // device->host data stage follows the SETUP
  uint8_t     iface;       // wIndex — the interface this request is aimed at
  const char* name;
};

static const DeckHidInitReq kDeckHidInit[] = {
  // ---- Probe A: plain standard IN fetches, BEFORE any class request. ----
  //
  // Last run sent SET_PROTOCOL and SET_IDLE first, and then every IN data
  // stage stalled — including GET_DESCRIPTOR(Device), which is not optional in
  // any USB device and which the IDF stack itself had already completed
  // successfully during enumeration (that fetch is how we got the 66-byte
  // config descriptor). So the device plainly CAN do an IN data stage, and the
  // only thing that changed before my request was the two class writes.
  //
  // Running the same fetch first, with nothing sent to the device beforehand,
  // separates "this device refuses IN data stages" from "our class requests put
  // EP0 into a state where it stops answering". If A succeeds and the identical
  // request later in the sequence fails, the class writes are the culprit and
  // the bring-up order is the bug.
  //
  // bmRequestType note, because I got this wrong and it cost several runs:
  // 0x80 = device-to-host | standard | DEVICE. 0x81 = device-to-host | standard
  // | INTERFACE. The low bit is the RECIPIENT, so Device and Config descriptors
  // must use 0x80. I had been sending 0x81 for all three and reading the stalls
  // as a statement about the deck. The deck was right and the probe was
  // malformed — a request it is entitled to refuse. Only the Report descriptor
  // genuinely takes 0x81, since a class descriptor is addressed to the interface.
  { 0x80, 0x06, 0x0100, 18, true, 0, "A:GET_DESCRIPTOR(Device)" },
  { 0x80, 0x06, 0x0200, 66, true, 0, "A:GET_DESCRIPTOR(Config)" },

  // ---- The report descriptor, asked FIRST, before any class request. ----
  //
  // HidDump (nexmon) reads this descriptor from the deck on a laptop with no
  // prior traffic, so the device plainly serves it — which retracts my earlier
  // conclusion that the stall was a device quirk. It was mine. The stall we saw
  // came after SET_PROTOCOL and SET_IDLE, so the two live explanations are:
  //
  //   1. our SETUP is still malformed in a way the other descriptors do not
  //      catch (they were only ever checked for "did not stall");
  //   2. the request is fine but our own class writes put EP0 somewhere the
  //      report fetch cannot follow.
  //
  // Asking it here, first, before anything has been written to the device, is
  // the A/B that separates those. If it succeeds now and still stalls in probe B
  // below, the class writes are the culprit and the bring-up order is the bug.
  // If it stalls here too, the request itself is wrong and the stall was never
  // about ordering.
  //
  // wLength 54 is not a guess — it is the device's own wDescriptorLength from
  // the HID class descriptor, and the dumped descriptor is exactly 54 bytes.
  { 0x81, 0x06, 0x2200, 54, true, 0, "A:GET_DESCRIPTOR(Report)" },

  // HID class requests. bmRequestType 0x21 = host-to-device | class | interface.
  { 0x21, 0x0B, 0x0000, 0, false, 0, "SET_PROTOCOL(report)" },  // wValue 0 = report protocol

  // GET_REPORT(Input, report_id=0) — the HID class-specific way to fetch a
  // single report synchronously. bmRequestType 0xA1 = device-to-host | class |
  // interface, request 0x01 = GET_REPORT, wValue = (report_type<<8) | report_id
  // (report_type 1 = Input, 2 = Output, 3 = Feature). wLength is the size of
  // the report we want back, taken from the descriptor: the input item uses
  // Report Count 96 00 02 = 512 × 8 bits, so 512 bytes is the natural choice.
  //
  // This is a deliberate probe, not part of the bring-up. The interrupt IN
  // pipe (EP 0x82) has been silent across every host-side variant we have
  // tried. GET_REPORT reads the same report via EP0 and decouples the question
  // "does the device have a report to send?" from "is our interrupt pipe
  // wired?". If it returns data here, the device is alive and we know its
  // current state; if it stalls, the device genuinely has nothing buffered,
  // and our IN-path hunts are pointing at the wrong layer.
  { 0xA1, 0x01, 0x0100, 512, true, 0, "GET_REPORT(Input)" },

  // SET_IDLE(0) — "report only on change" — which is what the Linux kernel driver
  // sends and therefore what the reference stack runs against.
  //
  // I previously sent SET_IDLE(100 ms) as a heartbeat on the theory that a
  // broken on-change path would be forced open. The run came back with both
  // pipes silent and both requests accepted, and accepting a class request is
  // NOT evidence of honouring it: cheap firmware ACKs and ignores. So that
  // experiment did not rule out the idle rate — it failed to test it.
  //
  // Matching the reference is the defensible choice, and the button-press runs
  // already cover the on-change case. If the device ignores SET_IDLE entirely,
  // no value we send will help, and matching the only known-working
  // configuration is what is left to try.
  { 0x21, 0x0A, 0x0000, 0, false, 0, "SET_IDLE(0,iface0)" },
  { 0x21, 0x0A, 0x0000, 0, false, 1, "SET_IDLE(0,iface1)" },

  // ---- Probe B: the identical requests again, now AFTER the class writes. ----
  //
  // A is the control; B is the treatment. Same request, same size, same
  // interface — only the two HID class writes sit between them. B failing where
  // A succeeded would be a sharp, self-contained result, and it would explain
  // every result so far in one stroke: the deck was never refusing to report
  // input, we had been breaking its control endpoint before asking.
  { 0x80, 0x06, 0x0100, 18, true, 0, "B:GET_DESCRIPTOR(Device)" },
  { 0x81, 0x06, 0x2200, 54, true, 0, "B:GET_DESCRIPTOR(Report)" },
};
static constexpr uint8_t DECK_HID_INIT_COUNT = sizeof(kDeckHidInit) / sizeof(kDeckHidInit[0]);

// ---------------------------------------------------------------------------
// deck_dump_raw_descriptors
//
// Walks the active configuration descriptor byte by byte and logs every
// descriptor's type and length, with the HID descriptor (type 0x21) dumped in
// full.
//
// This dump exists to test — and has now disproved — the theory that the HID
// class byte was decorative. The 9 bytes are read as:
//   [0] bLength (9)
//   [1] bDescriptorType (0x21)
//   [2..3] bcdHID — the HID spec version, 0x0200 on this deck
//   [4]   bCountryCode
//   [5]   bNumDescriptors — how many class descriptors follow
//   [6]   bDescriptorType of the following class descriptor (0x22 = Report)
//   [7..8] wDescriptorLength of the following class descriptor
//
// The bench said bNumDescriptors=1 with a real 0x22 Report descriptor (54 bytes
// on interface 0, 63 on interface 1), so the deck IS a HID device and the
// "vendor protocol wearing a HID class byte" reading was wrong. What is
// genuinely anomalous is that it still refuses GET_DESCRIPTOR(Report) — which
// is a separate question, and one kDeckHidInit now probes directly.
//
// The device has never been enumerated at more than a few hundred bytes, and
// this runs once per connect, before any transfer is armed.
// ---------------------------------------------------------------------------
static void deck_dump_raw_descriptors(const usb_config_desc_t* cfg) {
  if (!cfg) return;
  const uint8_t* raw = (const uint8_t*)cfg;
  const int total = cfg->wTotalLength;
  DECK_LOG("dump: config descriptor %d bytes, bNumInterfaces=%u",
           total, (unsigned)cfg->bNumInterfaces);

  int off = 0;
  while (off + 2 <= total) {
    const uint8_t len = raw[off];
    const uint8_t type = raw[off + 1];
    if (len < 2 || off + len > total) {
      DECK_LOG("dump:   @%3d type=0x%02X len=%u  <-- malformed, stopping",
               off, type, (unsigned)len);
      break;
    }

    // Every descriptor's first bytes, so an unexpected type is visible rather
    // than silently skipped. 16 bytes covers every descriptor this device has.
    const int shown = (len < 16) ? len : 16;
    char hex[16 * 3 + 1];
    for (int i = 0; i < shown; i++) snprintf(hex + (3 * i), 4, "%02X ", raw[off + i]);
    hex[3 * shown] = '\0';
    DECK_LOG("dump:   @%3d type=0x%02X len=%3u  %s",
             off, type, (unsigned)len, hex);

    if (type == 0x21 && len >= 9) {
      DECK_LOG("dump:     HID bcdHID=0x%04X bCountryCode=%02X bNumDescriptors=%u",
               (unsigned)(raw[off + 2] | (raw[off + 3] << 8)), raw[off + 4],
               (unsigned)raw[off + 5]);
      if (raw[off + 5] > 0 && len >= 9) {
        DECK_LOG("dump:     class desc type=0x%02X len=%u",
                 raw[off + 6], (unsigned)(raw[off + 7] | (raw[off + 8] << 8)));
      } else {
        DECK_LOG("dump:     NO trailing class descriptor — HID declaration is "
                 "decorative, this is a vendor protocol");
      }
    }

    // Interface (0x09) is followed by its endpoint descriptors and any class
    // descriptor; step over the whole interface block by counting bNumEndpoints
    // plus one class descriptor when bNumEndpoints > 0, which is the standard
    // layout. This keeps the walk aligned without a full parser.
    if (type == 0x04) {   // interface
      const int eps = raw[off + 4];
      for (int e = 0; e < eps; e++) {
        if (off + 2 > total) break;
        const uint8_t elen = raw[off];
        const uint8_t etype = raw[off + 1];
        if (elen < 2 || off + elen > total) break;
        const int eshown = (elen < 16) ? elen : 16;
        char ehex[16 * 3 + 1];
        for (int i = 0; i < eshown; i++) snprintf(ehex + (3 * i), 4, "%02X ", raw[off + i]);
        ehex[3 * eshown] = '\0';
        DECK_LOG("dump:     @%3d ep type=0x%02X len=%u addr=0x%02X mps=%u  %s",
                 off, etype, (unsigned)elen, raw[off + 2],
                 (unsigned)(raw[off + 3] | (raw[off + 4] << 8)), ehex);
        off += elen;
      }
      // A class descriptor (0x21) may follow the endpoints.
      if (off + 2 <= total && raw[off + 1] == 0x21) {
        const uint8_t clen = raw[off];
        if (clen >= 2 && off + clen <= total) {
          const int cshown = (clen < 16) ? clen : 16;
          char chex[16 * 3 + 1];
          for (int i = 0; i < cshown; i++) snprintf(chex + (3 * i), 4, "%02X ", raw[off + i]);
          chex[3 * cshown] = '\0';
          DECK_LOG("dump:     @%3d class type=0x%02X len=%u  %s",
                   off, raw[off + 1], (unsigned)clen, chex);
          if (clen >= 9) {
            DECK_LOG("dump:       HID bcdHID=0x%04X bCountryCode=%02X bNumDescriptors=%u",
                     (unsigned)(raw[off + 2] | (raw[off + 3] << 8)), raw[off + 4],
                     (unsigned)raw[off + 5]);
            if (raw[off + 5] > 0 && clen >= 9) {
              DECK_LOG("dump:       class desc type=0x%02X len=%u",
                       raw[off + 6], (unsigned)(raw[off + 7] | (raw[off + 8] << 8)));
            } else {
              DECK_LOG("dump:       NO trailing class descriptor — no HID "
                       "report descriptor declared");
            }
          }
          off += clen;
        }
      }
      continue;
    }

    off += len;
  }
}

// 0 = nothing owed, 1..N = send kDeckHidInit[state-1] next, N+1 = finished.
static volatile uint8_t deck_hid_state = 0;
static volatile uint8_t deck_ctrl_idx  = 0;   // index of the request in flight
static volatile bool    deck_ctrl_busy = false;

static const char* deck_xfer_status_str(usb_transfer_status_t s) {
  switch (s) {
    case USB_TRANSFER_STATUS_COMPLETED: return "completed";
    case USB_TRANSFER_STATUS_ERROR:     return "error";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "timed out";
    case USB_TRANSFER_STATUS_CANCELED:  return "canceled";
    case USB_TRANSFER_STATUS_STALL:     return "stalled";
    case USB_TRANSFER_STATUS_OVERFLOW:  return "overflow";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "no device";
    default:                            return "skipped/other";
  }
}

static void deck_ctrl_transfer_cb(usb_transfer_t* xfer) {
  deck_ctrl_busy = false;
  const bool have_req = deck_ctrl_idx < DECK_HID_INIT_COUNT;
  const DeckHidInitReq& req = kDeckHidInit[have_req ? deck_ctrl_idx : 0];
  // 0xFE is the post-init GET_REPORT probe, fired once from deck_usb_poll().
  // The probe table does not own it, so we name it explicitly here rather
  // than the "?" placeholder — having a real label in the bench log is what
  // makes this probe useful when it stalls or returns data.
  const char* name = have_req ? req.name
                    : (deck_ctrl_idx == 0xFE ? "post-init:GET_REPORT(Input)" : "?");

  if (!xfer) return;

  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    // A stall here is not fatal — the device is allowed to refuse, and
    // reporting simply stays at whatever default it chose. It is logged with
    // the interface number because these four probes exist to find out which
    // interface answers, and "somebody stalled" is not that answer.
    if (have_req) {
      DECK_LOG("hid: %s (wIndex=iface %u) -> %s", name, (unsigned)req.iface,
               deck_xfer_status_str(xfer->status));
    } else {
      DECK_LOG("hid: %s -> %s", name, deck_xfer_status_str(xfer->status));
    }
    return;
  }

  DECK_LOG("hid: %s OK", name);

  // The post-init GET_REPORT probe (deck_ctrl_idx == 0xFE) is not in the
  // kDeckHidInit table but still has a data stage — its 512-byte return is
  // exactly what we are testing. Treat any out-of-table completion whose deck
  // control transfer asked for wLength > 0 as a data read to be dumped.
  const bool has_data = have_req ? req.reads_data
                                : (deck_ctrl_idx == 0xFE);
  if (!has_data) return;

  // A data stage returned bytes after the 8 SETUP bytes. Which descriptor that
  // is depends on the request, so it is printed generically.
  const int total = xfer->actual_num_bytes;
  const uint8_t* desc = xfer->data_buffer + 8;
  const int desc_len = total - 8;

  if (desc_len <= 0) {
    if (have_req) {
      DECK_LOG("hid: %s returned no data (wLength=%u)", name, (unsigned)req.length);
    } else {
      DECK_LOG("hid: %s returned no data", name);
    }
    return;
  }

  DECK_LOG("hid: %s returned %d bytes (asked %u):", name, desc_len,
           have_req ? (unsigned)req.length : 512u);
  // Print every byte the device gave us. The 48-byte cap that used to sit on
  // this loop truncated report descriptors above their 48th byte — the CN003
  // returns 54, and the missing six bytes hid the trailing Output/Feature items.
  for (int i = 0; i < desc_len; i += 8) {
    char line[8 * 3 + 1];
    const int n = (desc_len - i) < 8 ? (desc_len - i) : 8;
    for (int j = 0; j < n; j++) snprintf(line + (3 * j), 4, "%02X ", desc[i + j]);
    line[3 * n] = '\0';
    DECK_LOG("hid:   %2d: %s", i, line);
  }
  // Only meaningful for the Report descriptor: every item begins with a byte
  // encoding bTag (upper nibble), bType (bits 2-3) and bSize (low nibble). The
  // first Input item's size is the report length once long items are folded in.
  // Harmless to print for the other two, so no branch is needed.
  DECK_LOG("hid:   item0=%02X bTag=%u bType=%u bSize=%u",
           desc[0], desc[0] >> 4, (desc[0] >> 2) & 0x03, desc[0] & 0x0F);
}

// Arms the single IN URB. `clear_first` halts, flushes and clears the pipe
// first; that resets the data toggle, so it is only safe when the previous URB
// did not complete cleanly.
//
// The halt is asynchronous: usb_host_endpoint_halt() cancels the in-flight URB
// and that cancellation is reported later, in the transfer callback. Submitting
// on the next line used to race it and failed 5-for-5 with ESP_ERR_NOT_FINISHED
// — which is our own defect, not device behaviour, and it meant the recovery
// path could never re-arm the pipe at all. So: if something is in flight, halt
// and return false. The callback then sets in_resubmit_needed as usual, and
// deck_usb_poll() comes back here with in_flight already false and the pipe
// settled, which is the only point at which halt/flush/clear/submit is legal.
static bool deck_in_arm(bool clear_first) {
  usb_transfer_t* x = deck_in_xfer;
  if (!x || !x->device_handle) return false;

  if (clear_first) {
    if (in_flight) {
      in_halt_pending = true;
      usb_host_endpoint_halt(x->device_handle, x->bEndpointAddress);
      DECK_DEBUG("IN: halt requested, waiting for the callback to re-arm");
      return false;
    }
    usb_host_endpoint_flush(x->device_handle, x->bEndpointAddress);
    usb_host_endpoint_clear(x->device_handle, x->bEndpointAddress);
  }

  x->num_bytes = IN_XFER_SIZE;
  esp_err_t err = usb_host_transfer_submit(x);
  if (err == ESP_OK) {
    in_flight = true;
    return true;
  }

  in_flight = false;
  DECK_DEBUG("IN arm failed: %s", esp_err_to_name(err));
  return false;
}

static void deck_hid_class_init(void) {
  if (!deck_dev_hdl || !deck_ctrl_xfer) return;

  if (deck_hid_state == 0) return;                        // nothing owed
  if (deck_hid_state > DECK_HID_INIT_COUNT) {              // already done
    deck_hid_state = 0;
    return;
  }
  if (deck_ctrl_busy) return;                              // previous one still in flight

  const uint8_t idx = deck_hid_state - 1;
  const DeckHidInitReq& req = kDeckHidInit[idx];

  // ESP-IDF takes no setup_packet field on usb_transfer_t at this version: the
  // 8-byte SETUP packet goes at the head of the data buffer and num_bytes
  // includes it.
  uint8_t* buf = deck_ctrl_xfer->data_buffer;
  buf[0] = req.bm_request_type;
  buf[1] = req.request;
  buf[2] = (uint8_t)(req.value & 0xFF);            // wValue, little endian
  buf[3] = (uint8_t)(req.value >> 8);
  buf[4] = (uint8_t)(req.iface & 0xFF);        // wIndex = interface number
  buf[5] = (uint8_t)(req.iface >> 8);
  buf[6] = (uint8_t)(req.length & 0xFF);           // wLength, little endian
  buf[7] = (uint8_t)(req.length >> 8);

  deck_ctrl_xfer->num_bytes        = 8 + req.length;
  deck_ctrl_xfer->device_handle    = deck_dev_hdl;
  deck_ctrl_xfer->bEndpointAddress = 0;             // EP0
  deck_ctrl_xfer->callback         = deck_ctrl_transfer_cb;
  deck_ctrl_xfer->context          = nullptr;

  deck_ctrl_idx = idx;
  esp_err_t err = usb_host_transfer_submit_control(deck_client, deck_ctrl_xfer);
  if (err != ESP_OK) {
    DECK_LOG("hid: %s submit failed: %s", req.name, esp_err_to_name(err));
    deck_hid_state = 0;                             // do not retry forever
    return;
  }

  deck_ctrl_busy = true;
  deck_hid_state = (uint8_t)(idx + 2);              // advances past this request
}

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

  // The control URB carries a SETUP packet plus, for the report-descriptor
  // read, a data stage. Note there is no separate control client at this IDF
  // version: usb_host_client_config_t::is_synchronous is documented "set to
  // false for now", so control transfers ride the async client and complete from
  // usb_host_client_handle_events().
  err = usb_host_transfer_alloc(DECK_CTRL_BUF, 0, &deck_ctrl_xfer);
  if (err != ESP_OK) {
    DECK_LOG("init: control transfer alloc failed: %s (input may stay silent)",
             esp_err_to_name(err));
    deck_ctrl_xfer = nullptr;
  }

  err = usb_host_transfer_alloc(IN_XFER_SIZE, 0, &deck_in_xfer);
  if (err != ESP_OK) {
    DECK_LOG("init: IN transfer alloc failed: %s", esp_err_to_name(err));
    deck_in_xfer = nullptr;
  } else {
    deck_in_xfer->callback = deck_in_transfer_cb;
    deck_in_xfer->context = nullptr;
  }

  // Aux IN transfer for the second interrupt-IN interface (see AUX state block).
  // Allocated unconditionally, even if no second interface turns up — the
  // descriptor walk decides whether it gets claimed and armed.
  err = usb_host_transfer_alloc(AUX_XFER_SIZE, 0, &deck_aux_xfer);
  if (err != ESP_OK) {
    DECK_LOG("init: AUX transfer alloc failed: %s", esp_err_to_name(err));
    deck_aux_xfer = nullptr;
  } else {
    deck_aux_xfer->callback = deck_aux_transfer_cb;
    deck_aux_xfer->context  = nullptr;
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

  // HID class setup is driven one request per pump, so a device that stalls
  // EP0 costs a poll cycle rather than stalling the whole USB task.
  deck_hid_class_init();

  // Hold both IN pipes until the device has been initialized, then arm them
  // together. This mirrors mirajazz's order (open -> DIS -> LIG 0 -> read),
  // which we previously did not: the read used to start ~100 ms BEFORE the
  // device was brought up, with the OUT URB and five EP0 control transfers
  // still competing for DWC2 channels.
  //
  // The HID class sequence must also finish first, so a stall on the report
  // descriptor probe (which the deck does produce) cannot leave the IN pipes
  // armed on a device that is still mid-handshake.
  //
  // Gated on ALL init packets having completed, not merely the first. The
  // reference init is ~11 packets (see deck_open_and_configure), and arming the
  // read after the first completion would put the IN URBs back in competition
  // with the remaining init writes — reintroducing exactly the contention this
  // gate exists to remove.
  if (deck_waiting_for_init && !deck_ctrl_busy && deck_hid_state == 0 &&
      deck_out_completed >= deck_init_packets_expected) {
    deck_waiting_for_init = false;
    in_arm_pending = true;
    aux_arm_pending = true;
    // One-shot follow-up: ask the device over EP0 what its current state is,
    // after the device has been brought up. If GET_REPORT comes back with
    // something other than the version banner (which is what comes back before
    // init), we have a report without a button press, and the interrupt-side
    // silence stops pointing at the receive path.
    deck_post_init_report_query = true;
    last_in_completed_ms = millis();
    DECK_LOG("init: DIS/LIG sent (out_done=%lu), HID setup done — arming IN pipes",
             (unsigned long)deck_out_completed);
  }

  // Post-init GET_REPORT. Submitted on EP0 with the control URB once the init
  // packets have drained. The probe table at boot also issued a GET_REPORT,
  // but that ran BEFORE DIS/LIG and the device answered with its boot-time
  // version banner ("V3.CN003.02.015"), not a state report. Re-asking here
  // shows whether the device, after being initialised, whether it has any
  // state worth reading — independent of the interrupt pipe (EP 0x82) being
  // silent.
  //
  // The control URB is the same one the probe table uses, so no extra alloc.
  // bmRequestType 0xA1 = device-to-host | class | interface,
  // request 0x01 = GET_REPORT,
  // wValue = (report_type<<8) | report_id = (1<<8) | 0 = 0x0100 (Input, id 0),
  // wIndex = interface 0,
  // wLength = 512 — the input report size from the descriptor.
  if (deck_post_init_report_query && deck_dev_hdl && deck_ctrl_xfer &&
      !deck_ctrl_busy) {
    deck_post_init_report_query = false;
    uint8_t* buf = deck_ctrl_xfer->data_buffer;
    buf[0] = 0xA1;
    buf[1] = 0x01;
    buf[2] = 0x00;                       // wValue low: report id 0
    buf[3] = 0x01;                       // wValue high: report type 1 = Input
    buf[4] = 0x00;                       // wIndex low
    buf[5] = 0x00;                       // wIndex high (interface 0)
    buf[6] = 0x00;                       // wLength low (512)
    buf[7] = 0x02;                       // wLength high
    deck_ctrl_xfer->num_bytes        = 8 + 512;
    deck_ctrl_xfer->device_handle    = deck_dev_hdl;
    deck_ctrl_xfer->bEndpointAddress = 0;
    deck_ctrl_xfer->callback         = deck_ctrl_transfer_cb;
    deck_ctrl_xfer->context          = nullptr;

    // Use a dedicated callback name for the log line so the post-init probe is
    // distinguishable from the boot-time probes in the bench log.
    deck_ctrl_idx = 0xFE;
    esp_err_t err = usb_host_transfer_submit_control(deck_client, deck_ctrl_xfer);
    if (err == ESP_OK) {
      deck_ctrl_busy = true;
      DECK_LOG("post-init: GET_REPORT(Input) submitted — press buttons now");
    } else {
      DECK_LOG("post-init: GET_REPORT submit failed: %s", esp_err_to_name(err));
    }
  }

  // Arm the first IN URB from here rather than from the event callback.
  //
  // The OUT path has always been submitted from this function and works; the IN
  // URB was submitted from inside deck_open_and_configure(), which runs in the
  // client's event callback, and it never delivered a byte. A URB submitted from
  // inside that callback is offered to the HCD in the same event-processing pass
  // the device was just enumerated in, which is the one context where the pipe
  // state is not yet settled. Submitting both pipes from the same place removes
  // that difference and costs nothing.
  if (in_arm_pending && deck_dev_hdl && deck_in_xfer) {
    in_arm_pending = false;
    if (deck_in_arm(false)) {
      DECK_LOG("open: 1 IN transfer submitted");
    }
  }

  // Re-submit a completed IN transfer here rather than in the callback, so the
  // library has fully settled the pipe state first.
  if (in_resubmit_needed && deck_dev_hdl && deck_in_xfer) {
    in_resubmit_needed = false;
    const bool clear_first = in_error_recovery_needed;
    in_error_recovery_needed = false;
    if (!deck_in_arm(clear_first)) {
      in_resubmit_needed = true;
      in_error_recovery_needed = true;
    }
  }

  // Aux (interface 1 / EP 0x81) arm and re-arm. Same shape as the primary IN
  // pipe and in the same place, for the same reason: submit from the main loop,
  // never from inside a client event callback.
  if (aux_arm_pending && deck_dev_hdl && deck_aux_xfer) {
    aux_arm_pending = false;
    if (deck_aux_arm()) {
      DECK_LOG("open: AUX 0x%02X 1 transfer submitted", deck_aux_ep);
    }
  }
  if (aux_resubmit_needed && deck_dev_hdl && deck_aux_xfer) {
    aux_resubmit_needed = false;
    if (!deck_aux_arm()) aux_resubmit_needed = true;
  }

  // Watchdog, active only until the first report ever arrives.
  if (deck_dev_hdl && deck_in_xfer && deck_in_completed == 0 &&
      in_flight && !in_resubmit_needed && !in_arm_pending &&
      in_watchdog_fires < IN_WATCHDOG_MAX &&
      (uint32_t)(millis() - last_in_completed_ms) >= IN_WATCHDOG_MS) {
    last_in_completed_ms = millis();
    in_watchdog_fires++;
    DECK_LOG("IN: no report after %u ms — clearing pipe and re-arming (%u/%u)",
             (unsigned)IN_WATCHDOG_MS, (unsigned)in_watchdog_fires,
             (unsigned)IN_WATCHDOG_MAX);
    // deck_in_arm() manages in_flight itself: with a URB still out it halts and
    // waits for the callback. Forcing in_flight = false here would defeat that
    // and re-open the race it exists to close.
    deck_in_arm(true);
  }

  // Keepalive: the device times out and drops the session without it.
  if (deck_dev_hdl && deck_out_ep) {
    uint32_t now = millis();
    if ((uint32_t)(now - last_keepalive_ms) >= KEEPALIVE_MS) {
      last_keepalive_ms = now;
      deck_send_keepalive();
    }
  }

  // Phase 0 heartbeat. An interrupt IN endpoint NAKs while the device has
  // nothing to report, so "zero completions" is expected at idle and is not by
  // itself a fault — but it does tell us the pipe is live and being polled,
  // which separates a dead endpoint from a silent device.
  if (deck_dev_hdl) {
    uint32_t now = millis();
    if ((uint32_t)(now - last_status_ms) >= 3000) {
      last_status_ms = now;
      DECK_LOG("status: IN=0x%02X OUT=0x%02X reports=%lu out_done=%lu "
               "in_flight=%d out_in_flight=%d resubmit_pending=%d "
               "| AUX=0x%02X reports=%lu bytes=%lu aux_in_flight=%d",
               deck_in_ep, deck_out_ep, (unsigned long)deck_in_completed,
               (unsigned long)deck_out_completed,
               in_flight ? 1 : 0, out_in_flight ? 1 : 0,
               in_resubmit_needed ? 1 : 0,
               deck_aux_ep, (unsigned long)deck_aux_completed,
               (unsigned long)deck_aux_bytes, aux_in_flight ? 1 : 0);
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
      // Every record in the ring is already padded to CMD_WIRE by
      // deck_out_queue, so the read is record-aligned.
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
//
// Pads every record out to CMD_WIRE before queueing it. The deck expects a
// fixed-width output report: mirajazz resizes every command buffer to
// 1 + packet_size (1025 for protocol v2/v3) before writing, and a short packet
// is silently ignored — which is why a 9-byte "LIG" left the key backlight
// dark on the first bench attempt. Padding here rather than in each builder
// keeps the ring buffer record-aligned with the CMD_WIRE-sized reads in
// deck_usb_poll, and image chunks pass through unchanged.
// ---------------------------------------------------------------------------
static bool deck_out_queue(const uint8_t* data, uint16_t len) {
  if (!deck_dev_hdl || !deck_out_ep || !deck_out_rb || !data || !len) return false;
  if (len > DeckProto::CMD_WIRE) {
    DECK_DEBUG("OUT packet %u bytes exceeds CMD_WIRE (%u), dropping",
               len, (unsigned)DeckProto::CMD_WIRE);
    return false;
  }

  uint8_t padded[DeckProto::CMD_WIRE];
  memcpy(padded, data, len);
  memset(padded + len, 0, DeckProto::CMD_WIRE - len);

  BaseType_t ok = xRingbufferSend(deck_out_rb, padded, DeckProto::CMD_WIRE, 0);
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
  in_flight = false;
  last_in_completed_ms = millis();

  // The pipe is settled now, so a pending halt may proceed. This callback is
  // the completion signal for usb_host_endpoint_halt(); nothing else can tell us
  // when the async cancel has finished.
  if (in_halt_pending) {
    in_halt_pending = false;
    DECK_DEBUG("IN: halt completed (status=%d), pipe ready to re-arm",
               (int)xfer->status);
  }

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    const uint8_t* p = xfer->data_buffer;
    uint16_t n = xfer->actual_num_bytes;

    // Dump the head of every report, ACK or not. Phase 0 exists to find out
    // what the device actually sends, so nothing is filtered on arrival: a
    // report that fails the ACK check is still the most interesting thing on
    // the bench, and the whole reason this callback logs at all.
    {
      char head[3 * 8 + 1];
      const uint16_t shown = (n < 8) ? n : 8;
      for (uint16_t i = 0; i < shown; i++) {
        snprintf(head + (3 * i), 4, "%02X ", p[i]);
      }
      head[3 * shown] = '\0';
      DECK_LOG("IN: len=%u ack=%d head=%s", n, DeckProto::inIsAck(p, n) ? 1 : 0, head);
    }

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
    deck_in_completed++;
    in_error_recovery_needed = false;
  } else {
    DECK_DEBUG("IN callback: status=%d bytes=%d (recover in poll)",
               (int)xfer->status, (int)xfer->actual_num_bytes);
    in_error_recovery_needed = true;
  }

  in_resubmit_needed = true;
}

// ---------------------------------------------------------------------------
// AUX (interface 1 / EP 0x81) callback — the boot-keyboard pipe.
//
// Deliberately does NOT feed the app queue or parse anything. The only question
// here is whether a single byte ever arrives on this endpoint; if it does, the
// 8-byte boot-report layout has to be understood properly before it drives
// anything. So it is counted and dumped, and nothing more.
// ---------------------------------------------------------------------------
static void deck_aux_transfer_cb(usb_transfer_t* xfer) {
  if (!xfer) return;
  aux_in_flight = false;

  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
    const uint8_t* p = xfer->data_buffer;
    const uint16_t n = xfer->actual_num_bytes;
    char hex[3 * 8 + 1];
    const uint16_t shown = (n < 8) ? n : 8;
    for (uint16_t i = 0; i < shown; i++) snprintf(hex + (3 * i), 4, "%02X ", p[i]);
    hex[3 * shown] = '\0';

    deck_aux_completed++;
    deck_aux_bytes += n;
    // DECK_LOG, not DECK_DEBUG: a byte on this pipe is the first input this
    // firmware has ever seen, and it must never be hidden behind a build flag.
    DECK_LOG("AUX 0x%02X: REPORT! len=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
             deck_aux_ep, n, p[0], n > 1 ? p[1] : 0, n > 2 ? p[2] : 0,
             n > 3 ? p[3] : 0, n > 4 ? p[4] : 0, n > 5 ? p[5] : 0,
             n > 6 ? p[6] : 0, n > 7 ? p[7] : 0);
    (void)hex;
  } else {
    DECK_DEBUG("AUX 0x%02X cb: status=%d bytes=%d", deck_aux_ep,
               (int)xfer->status, (int)xfer->actual_num_bytes);
  }

  aux_resubmit_needed = true;
}

// Arms the interface-1 IN URB. Kept separate from deck_in_arm() so the two
// pipes cannot share a completion counter and mask each other.
static bool deck_aux_arm(void) {
  usb_transfer_t* x = deck_aux_xfer;
  if (!x || !x->device_handle) return false;
  x->num_bytes = AUX_XFER_SIZE;
  esp_err_t err = usb_host_transfer_submit(x);
  if (err == ESP_OK) {
    aux_in_flight = true;
    return true;
  }
  aux_in_flight = false;
  DECK_DEBUG("AUX arm failed: %s", esp_err_to_name(err));
  return false;
}

// ---------------------------------------------------------------------------
// OUT transfer callback
// ---------------------------------------------------------------------------
static void deck_out_transfer_cb(usb_transfer_t* xfer) {
  out_in_flight = false;
  deck_out_completed++;

  // Phase 0: log the result rather than swallowing it. An OUT packet that
  // never completes (or errors) is otherwise completely invisible, and it
  // matters because these decks typically stay silent on the IN pipe until the
  // host has initialised them with DIS — so a dead OUT path looks exactly like
  // "the device never sends reports".
  if (xfer && (xfer->status != USB_TRANSFER_STATUS_COMPLETED || xfer->actual_num_bytes == 0)) {
    DECK_DEBUG("OUT: status=%d bytes=%d of %u",
               (int)xfer->status, (int)xfer->actual_num_bytes, (unsigned)xfer->num_bytes);
  }
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

  // The negotiated speed matters more than it looks. This board runs the HS PHY
  // forced into high-speed mode (see wled.cpp), and in DWC host mode the
  // periodic schedule that drives interrupt IN polling is laid out differently
  // for a full-speed device than for a high-speed one. A mismatch there shows up
  // as OUT transfers completing normally while interrupt IN never delivers a
  // single byte — which is the current symptom. Log the real value rather than
  // inferring it.
  usb_device_info_t dev_info = {};
  if (usb_host_device_info(dev, &dev_info) == ESP_OK) {
    DECK_LOG("open: speed=%s ep0_mps=%u", deck_speed_str(dev_info.speed),
             dev_info.bMaxPacketSize0);
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

  DECK_LOG("open: VID=0x%04X PID=0x%04X (%s) bcdUSB=%d.%02x — claimed as deck",
           deck_vid_v, deck_pid_v, deck_device_name(),
           dev_desc->bcdUSB >> 8, dev_desc->bcdUSB & 0xFF);

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

  // Always dump the raw descriptor walk. This runs once per connect, before any
  // transfer is armed, and it is currently the only way to tell a genuine HID
  // declaration from a vendor blob wearing the 0x03 class byte — the device
  // STALLs GET_DESCRIPTOR(Report) on every interface, so the report descriptor
  // cannot be asked for. See deck_dump_raw_descriptors() for what to look for.
  deck_dump_raw_descriptors(cfg_desc);

  // Find the deck's HID interface and its interrupt endpoints. These devices
  // present usage page 0xFF00; depending on firmware that arrives as a
  // vendor-class interface (0xFF) or as HID (0x03) with a vendor usage page.
  // A composite deck may also expose HID interfaces we don't want (media keys
  // and the like), so score the candidates and keep the best one: a vendor
  // interface that is bidirectional beats everything else.
  //
  // Every interface and every endpoint is logged regardless of class. An
  // earlier version filtered on class *before* logging, which meant an
  // interface of some other class — holding the real input endpoint, if any
  // exists — would be silently invisible. With the interrupt IN pipe refusing
  // to deliver, the descriptor is the only remaining source of truth, so the
  // walk reports everything it sees rather than only what it expected.
  int   claimed_iface = -1;
  int   best_score    = 0;
  uint8_t in_ep = 0, out_ep = 0;

  // Every interface carrying an interrupt IN endpoint, so a second one can be
  // claimed as an auxiliary input source (see the AUX state block). This deck
  // has two; only one has ever been armed.
  uint8_t in_iface_list[8] = {0};
  uint8_t in_ep_list[8]    = {0};
  uint8_t n_in_ifaces       = 0;
  int   aux_iface_num = -1;
  uint8_t aux_in_ep   = 0;

  for (uint8_t iface_num = 0; iface_num < 8; iface_num++) {
    int iface_off = 0;
    const usb_intf_desc_t* iface =
      usb_parse_interface_descriptor(cfg_desc, iface_num, 0, &iface_off);
    if (!iface) break;

    bool vendor = (iface->bInterfaceClass == 0xFF);
    bool hid    = (iface->bInterfaceClass == 0x03);

    uint8_t this_in = 0, this_out = 0;
    uint16_t this_in_mps = 0, this_out_mps = 0;

    // Log every endpoint, not just the interrupt ones, so an unusual endpoint
    // type or address shows up rather than being filtered into silence.
    for (uint8_t ep_idx = 0; ep_idx < iface->bNumEndpoints; ep_idx++) {
      int ep_off = iface_off;
      const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(
        iface, ep_idx, cfg_desc->wTotalLength, &ep_off);
      if (!ep) continue;

      static const char* kXferName[4] = { "CTRL", "ISO", "BULK", "INTR" };
      uint8_t attr = ep->bmAttributes & 0x03;
      DECK_LOG("open:     ep 0x%02X %s mps=%u interval=%u",
               ep->bEndpointAddress, kXferName[attr], ep->wMaxPacketSize,
               ep->bInterval);

      if (attr != 3) continue;               // only interrupt can carry input
      if ((ep->bEndpointAddress & 0x80) && !this_in) {
        this_in = ep->bEndpointAddress;
        this_in_mps = ep->wMaxPacketSize;
      } else if (!(ep->bEndpointAddress & 0x80) && !this_out) {
        this_out = ep->bEndpointAddress;
        this_out_mps = ep->wMaxPacketSize;
      }
    }

    // MPS is worth printing: it is what the DWC HCD refuses to allocate past,
    // and it is the value that decides whether the FIFO split in wled.cpp is
    // large enough for this device.
    uint16_t bcd_hid = 0;
    const bool has_hid_desc = deck_find_hid_desc(cfg_desc, iface_off, &bcd_hid);

    DECK_LOG("open:   iface %u class=0x%02X subclass=0x%02X protocol=0x%02X "
             "eps=%u alt=%u IN=0x%02X/%u OUT=0x%02X/%u bcdHID=%s",
             iface_num, iface->bInterfaceClass, iface->bInterfaceSubClass,
             iface->bInterfaceProtocol, iface->bNumEndpoints, iface->bAlternateSetting,
             this_in, this_in_mps, this_out, this_out_mps,
             has_hid_desc ? "present" : "none");

    if (!this_in) continue;                  // cannot work without input

    // Any interface with an interrupt IN is a candidate, whatever its class.
    // Class is used to rank, not to filter: the deck has already shown it
    // declines standard HID class requests, so bInterfaceClass is not a
    // reliable statement about which interface carries input.
    int score = (vendor ? 4 : (hid ? 2 : 0)) + (this_out ? 2 : 0);
    if (score > best_score) {
      best_score = score;
      claimed_iface = iface_num;
      in_ep = this_in;
      out_ep = this_out;
    }

    // Record every interface with an interrupt IN. The aux pipe is chosen
    // AFTER the walk, because claimed_iface is still changing here — picking
    // the aux interface inline would compare against a stale winner and could
    // end up claiming the same interface twice.
    if (n_in_ifaces < 8) {
      in_iface_list[n_in_ifaces] = iface_num;
      in_ep_list[n_in_ifaces] = this_in;
      n_in_ifaces++;
    }
  }

  // Aux pipe: the first interrupt-IN interface that is NOT the primary one.
  // On this device that is interface 1 / EP 0x81, the boot-keyboard endpoint
  // with an 8-byte MPS, which has never been claimed or armed by any build.
  aux_iface_num = -1;
  for (uint8_t i = 0; i < n_in_ifaces; i++) {
    if (in_iface_list[i] != claimed_iface) {
      aux_iface_num = in_iface_list[i];
      aux_in_ep = in_ep_list[i];
      break;
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

  // ---- Claim and arm the aux input pipe (interface 1 / EP 0x81). ----
  //
  // Best-effort: if the second claim fails for any reason the primary pipe
  // still works, and losing this experiment is no worse than the state we are
  // in now. It is claimed separately from the primary because usb_host allows
  // only one claim per interface, and both must be released on close.
  if (aux_iface_num >= 0) {
    err = usb_host_interface_claim(deck_client, dev, (uint8_t)aux_iface_num, 0);
    if (err == ESP_OK) {
      deck_aux_iface = (uint8_t)aux_iface_num;
      deck_aux_ep    = aux_in_ep;
      DECK_LOG("open: AUX CLAIMED iface=%d IN=0x%02X (boot-keyboard pipe)",
               aux_iface_num, aux_in_ep);
      if (deck_aux_xfer) {
        deck_aux_xfer->device_handle  = dev;
        deck_aux_xfer->bEndpointAddress = aux_in_ep;
        deck_aux_xfer->callback       = deck_aux_transfer_cb;
        deck_aux_xfer->context        = nullptr;
        aux_arm_pending = true;
      }
    } else {
      DECK_LOG("open: AUX interface_claim(iface=%d) failed: %s",
               aux_iface_num, esp_err_to_name(err));
    }
  } else {
    DECK_LOG("open: no second interrupt-IN interface, AUX pipe not claimed");
  }

  // Ask the device to start reporting. Deferred to deck_usb_poll() because the
  // requests complete asynchronously; see deck_hid_class_init().
  deck_hid_state = 1;

  // Bring the device up, reproducing opendeck-akp05's connect path EXACTLY.
  //
  // Read from the reference rather than assumed. opendeck-akp05/src/device.rs
  // device_task() does, in order:
  //
  //     connect()                -> Device::connect()   -> initialize()
  //     set_brightness(50)       -> initialize(), LIG 50
  //     clear_all_button_images()-> initialize(), CLE 0xFF, STP
  //     flush()                  -> initialize(), cache empty -> return (no STP)
  //
  // and mirajazz's initialize() is NOT a one-time thing — set_brightness(),
  // clear_all_button_images() and flush() each call it again first. So the real
  // wire sequence on a fresh connect is:
  //
  //     DIS, LIG 0
  //     DIS, LIG 0, LIG 50
  //     DIS, LIG 0, CLE 0xFF, STP
  //     DIS, LIG 0                      <- flush() then returns on empty cache
  //
  // i.e. DIS+LIG0 four times, and an STP. Two corrections to what I had assumed:
  //
  //   - I had removed STP as "a stray the reference never issues". Wrong. STP is
  //     sent by clear_all_button_images() for protocol v2/v3, which is what this
  //     deck reports. Removing it was the same mistake as the bmRequestType
  //     error: treating an unexplained difference as a defect instead of a clue.
  //   - The repeated DIS before every command is something I had never modelled
  //     at all. If the device arms its input path on DIS, sending it once may be
  //     exactly the difference.
  //
  // Note: this deck has NO LED rings on the rotary encoders. Knob state is shown
  // on the touch strip above them, so there is nothing to SETLB — that opcode
  // exists in the protocol but has no visible effect on this hardware.
  if (out_ep) {
    uint8_t buf[DeckProto::MAX_OUT_BYTES];
    // Counted as packets are queued, not hardcoded, so the arm gate in
    // deck_usb_poll() can never be satisfied by a queue that silently dropped
    // one — the count and the gate then stay consistent by construction.
    uint32_t queued = 0;

    // DeckProto::init() emits the DIS + LIG 0 pair that initialize() sends; the
    // reference repeats that pair before every subsequent command, so it is
    // called four times here rather than once.
    for (int step = 0; step < 4; step++) {
      size_t n = DeckProto::dis(buf, sizeof(buf));
      if (n) { deck_out_queue(buf, n); queued++; }

      n = DeckProto::lig(buf, sizeof(buf), 0);
      if (n) { deck_out_queue(buf, n); queued++; }

      // set_brightness(50): LIG 50 follows its initialize().
      if (step == 1) {
        n = DeckProto::lig(buf, sizeof(buf), 50);
        if (n) { deck_out_queue(buf, n); queued++; }
      }
      // clear_all_button_images(): CLE 0xFF then STP.
      if (step == 2) {
        n = DeckProto::cle(buf, sizeof(buf), DeckProto::CLEAR_ALL);
        if (n) { deck_out_queue(buf, n); queued++; }

        n = DeckProto::stp(buf, sizeof(buf));
        if (n) { deck_out_queue(buf, n); queued++; }
      }
      // step 3 is flush(): its initialize(), then an early return on the empty
      // image cache — so no STP.
    }

    deck_init_packets_expected = queued;
    DECK_LOG("init: queued %lu init packets (mirajazz connect sequence)", (unsigned long)queued);
  }

  // Hand the IN URBs to the device — but NOT YET. See the note below.
  if (deck_in_xfer) {
    deck_in_xfer->device_handle  = dev;
    deck_in_xfer->bEndpointAddress = in_ep;
    last_in_completed_ms = millis();
  }
  if (deck_aux_xfer) {
    deck_aux_xfer->device_handle  = dev;
    deck_aux_xfer->bEndpointAddress = aux_in_ep;
  }

  // Defer BOTH IN pipes until after the init packets have actually gone out.
  //
  // mirajazz's order is: open, write DIS, write LIG 0, and only THEN begin
  // reading. We were arming the IN URB immediately on claim — before the DIS/LIG
  // packets were even queued, and before the five HID class control transfers on
  // EP0 had run. So our read began ~100 ms before the device had been
  // initialized at all, which is not the configuration the reference uses.
  //
  // That matters more than the ordering alone suggests. While the IN URBs are
  // outstanding, the keepalive OUT URB and the EP0 control transfers are all
  // competing for the same DWC2 channels. Starting the read only once the
  // device is initialized — and once EP0 is quiet — removes both the ordering
  // variable and that contention, and makes this the last untested structural
  // difference from the reference.
  //
  // Gated on the OUT queue having actually drained (out_done > 0) rather than on
  // a timer, so the read genuinely begins after the device has been brought up
  // and not merely after a fixed delay that might land first on a slow boot.
  in_arm_pending = false;
  aux_arm_pending = false;
  deck_waiting_for_init = (out_ep != 0);

  last_keepalive_ms = millis();
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// deck_close_device
// ---------------------------------------------------------------------------
static void deck_close_device(void) {
  if (!deck_dev_hdl) return;

  deck_hid_state = 0;
  deck_ctrl_busy = false;
  in_arm_pending = false;
  in_resubmit_needed = false;
  in_error_recovery_needed = false;
  in_halt_pending = false;
  in_flight = false;
  in_watchdog_fires = 0;
  deck_waiting_for_init = false;
  deck_init_packets_expected = 0;
  deck_in_completed = 0;
  deck_out_completed = 0;
  if (deck_ctrl_xfer) deck_ctrl_xfer->device_handle = nullptr;

  // Note: the "red knobs on exit" shutdown sequence (DeckProto::shutdown_cmd)
  // needs the device to still be present, so it belongs on the reboot path,
  // not here — by the time DEV_GONE fires the pipe is already gone.

  usb_host_interface_release(deck_client, deck_dev_hdl, deck_iface);
  // Release the aux interface too — it was claimed separately, and leaving a
  // claim outstanding across a disconnect would block any future re-claim.
  if (deck_aux_iface) usb_host_interface_release(deck_client, deck_dev_hdl, deck_aux_iface);
  if (deck_in_xfer)  deck_in_xfer->device_handle = nullptr;
  if (deck_out_xfer) deck_out_xfer->device_handle = nullptr;
  if (deck_aux_xfer) deck_aux_xfer->device_handle = nullptr;
  out_in_flight = false;
  aux_in_flight = false;
  aux_arm_pending = false;
  aux_resubmit_needed = false;
  deck_aux_completed = 0;
  deck_aux_bytes = 0;
  deck_aux_iface = 0;
  deck_aux_ep = 0;
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
