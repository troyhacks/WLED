// opendeck_proto.h
// The OpenDeck device protocol, as the reference implements it.
//
// This is a port of the reference, not a reconstruction: the wire format
// comes from `mirajazz` (ambiso/mirajazz, a fork of elgato-streamdeck) and
// the device-specific parts from `opendeck-akp05` (ambiso/opendeck-akp05),
// which is the OpenDeck plugin for the Soomfon CN003 and the other
// Mirabox N4-family / Ajazz AKP05 devices.
//
// THE WIRE FORMAT. Every command is ASCII:
//
//   00 43 52 54 00 00 | <ASCII mnemonic> | <payload>
//
// The first byte is the report ID (0x00), then "CRT", then two reserved
// zero bytes. The mnemonic is variable length - three bytes for most, five
// for LBLIG and SETLB, seven for CONNECT.
//
// EVERY PACKET IS ZERO-PADDED to 1 + PACKET_SIZE bytes (1025 for protocol
// v2/v3). This is not cosmetic: the reference resizes each command to that
// length before writing it, and the device ignores a short packet. The
// image chunks are the same length.
//
// THE INBOUND SIDE. The device sends 512-byte interrupt IN reports. Only
// the ones that start with "ACK" (0x41 0x43 0x4B) carry input; byte 9 is
// a MESSAGE ID (not a plain index) and byte 10 is the state. The message
// IDs are mapped to events by `opendeck-akp05/src/inputs.rs`, and the
// button bitmap in bytes 11..26 is a full snapshot rather than a delta -
// the reference diffs consecutive snapshots to synthesise down and up.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace Opendeck {

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

// Report ID 0x00 + "CRT" + two reserved zero bytes.
static constexpr uint8_t HDR[6] = { 0x00, 0x43, 0x52, 0x54, 0x00, 0x00 };
static constexpr size_t   HDR_LEN = sizeof(HDR);

// The mnemonics, as ASCII.
static constexpr uint8_t OP_DIS[]     = { 0x44, 0x49, 0x53 };                     // "DIS"
static constexpr uint8_t OP_LIG[]     = { 0x4C, 0x49, 0x47 };                     // "LIG"
static constexpr uint8_t OP_LBLIG[]   = { 0x4C, 0x42, 0x4C, 0x49, 0x47 };         // "LBLIG"
static constexpr uint8_t OP_SETLB[]   = { 0x53, 0x45, 0x54, 0x4C, 0x42 };         // "SETLB"
static constexpr uint8_t OP_BAT[]     = { 0x42, 0x41, 0x54 };                     // "BAT"
static constexpr uint8_t OP_CLE[]     = { 0x43, 0x4C, 0x45 };                     // "CLE"
static constexpr uint8_t OP_STP[]     = { 0x53, 0x54, 0x50 };                     // "STP"
static constexpr uint8_t OP_HAN[]     = { 0x48, 0x41, 0x4E };                     // "HAN"
static constexpr uint8_t OP_CONNECT[] = { 0x43, 0x4F, 0x4E, 0x4E, 0x45, 0x43, 0x54 }; // "CONNECT"
static constexpr uint8_t OP_MOD[]     = { 0x4D, 0x4F, 0x44 };                     // "MOD"
static constexpr uint8_t OP_QUCMD[]   = { 0x51, 0x55, 0x43, 0x4D, 0x44 };         // "QUCMD"

// The device's packet size for protocol v2/v3, and the wire length every
// packet is padded to.
static constexpr uint16_t PACKET_SIZE = 1024;
static constexpr uint16_t WIRE_LEN   = 1 + PACKET_SIZE;   // 1025

// The key index that means "every key".
static constexpr uint8_t CLEAR_ALL = 0xFF;

// The number of buttons the device reports: the ten keys with displays and
// five without.
static constexpr uint8_t KEY_COUNT     = 15;
static constexpr uint8_t ENCODER_COUNT = 4;
// The button bitmap in the report is 16 entries long.
static constexpr uint8_t BITMAP_LEN = 16;

// ---------------------------------------------------------------------------
// The command builders.
//
// Each one writes the command into `out` (which must have room for
// WIRE_LEN) and returns its length. The padding to WIRE_LEN is done here,
// because the device ignores a short packet.
// ---------------------------------------------------------------------------

static inline size_t padToWire(uint8_t* out, size_t len) {
  if (len < WIRE_LEN) memset(out + len, 0, WIRE_LEN - len);
  return WIRE_LEN;
}

// "DIS" - reset the display. Sent first on every connect.
static inline size_t dis(uint8_t* out) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_DIS, sizeof(OP_DIS));
  return padToWire(out, HDR_LEN + sizeof(OP_DIS));
}

// "LIG" - the key backlight, 0..100 percent.
static inline size_t lig(uint8_t* out, uint8_t percent) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_LIG, sizeof(OP_LIG));
  out[9]  = 0x00;
  out[10] = 0x00;
  out[11] = (percent > 100) ? 100 : percent;
  return padToWire(out, 12);
}

// "LBLIG" - the encoder LED rings' brightness, 0..100. No effect on the
// CN003, which has no rings.
static inline size_t lblig(uint8_t* out, uint8_t percent) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_LBLIG, sizeof(OP_LBLIG));
  out[10] = (percent > 100) ? 100 : percent;
  return padToWire(out, 11);
}

// "SETLB" - the colour of each encoder's LED ring, three bytes per knob.
// No effect on the CN003.
static inline size_t setlb(uint8_t* out, const uint8_t* rgb, uint8_t count) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_SETLB, sizeof(OP_SETLB));
  memcpy(out + HDR_LEN + sizeof(OP_SETLB), rgb, (size_t)count * 3);
  return padToWire(out, HDR_LEN + sizeof(OP_SETLB) + (size_t)count * 3);
}

// "CLE" - clear one key's image, or every key when key is CLEAR_ALL. The
// device indexes its keys from 1.
//
// The slot is at wire offset 12, after three zero bytes - that is what
// mirajazz sends, and mirajazz is the code that works with this family.
// (A vendor reverse-engineering note puts the slot one byte earlier; see
// the conflict list in the OpenDeck notes.)
static inline size_t cle(uint8_t* out, uint8_t key) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_CLE, sizeof(OP_CLE));
  out[9]  = 0x00;
  out[10] = 0x00;
  out[11] = 0x00;
  out[12] = (key == CLEAR_ALL) ? CLEAR_ALL : (uint8_t)(key + 1);
  return padToWire(out, 13);
}

// "STP" - commit. Nothing appears on the keys without it.
static inline size_t stp(uint8_t* out) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_STP, sizeof(OP_STP));
  return padToWire(out, HDR_LEN + sizeof(OP_STP));
}

// "HAN" - put the device to sleep.
static inline size_t han(uint8_t* out) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_HAN, sizeof(OP_HAN));
  return padToWire(out, HDR_LEN + sizeof(OP_HAN));
}

// "CONNECT" - the keepalive. The device drops the session without one
// roughly every ten seconds.
static inline size_t connect(uint8_t* out) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_CONNECT, sizeof(OP_CONNECT));
  return padToWire(out, HDR_LEN + sizeof(OP_CONNECT));
}

// "MOD" - the device mode (0x30 + n).
static inline size_t mod(uint8_t* out, uint8_t mode) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_MOD, sizeof(OP_MOD));
  out[9]  = 0x00;
  out[10] = 0x00;
  out[11] = (uint8_t)(0x30 + mode);
  return padToWire(out, 12);
}

// "QUCMD" - the firmware configuration command (the N4 Pro family's
// vibration setting). The payload is a six-byte config array.
static inline size_t qucmd(uint8_t* out, const uint8_t* cfg, uint8_t len) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_QUCMD, sizeof(OP_QUCMD));
  memcpy(out + HDR_LEN + sizeof(OP_QUCMD), cfg, len);
  return padToWire(out, HDR_LEN + sizeof(OP_QUCMD) + len);
}

// "BAT" - announce an image for one key: the length that follows, and the
// key. The device indexes its keys from 1.
static inline size_t bat(uint8_t* out, uint8_t key, uint16_t len) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_BAT, sizeof(OP_BAT));
  out[9]  = 0x00;
  out[10] = 0x00;
  out[11] = (uint8_t)(len >> 8);
  out[12] = (uint8_t)(len & 0xFF);
  out[13] = (uint8_t)(key + 1);
  return padToWire(out, 14);
}

// The shutdown sequence: "CLE" with the magic key 0x44 0x43, then "HAN".
static inline size_t shutdown_cmd(uint8_t* out) {
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_CLE, sizeof(OP_CLE));
  out[9]  = 0x00;
  out[10] = 0x00;
  out[11] = 0x44;
  out[12] = 0x43;
  return padToWire(out, 13);
}

// One chunk of an image: the leading report-ID byte, the payload, and the
// zero padding to the wire length. The device reads the image's length
// from the BAT header, so the padding in the last chunk is ignored.
static inline size_t imageChunk(uint8_t* out, const uint8_t* payload, uint16_t len) {
  if (len > PACKET_SIZE) return 0;
  out[0] = 0x00;
  memcpy(out + 1, payload, len);
  memset(out + 1 + len, 0, WIRE_LEN - 1 - len);
  return WIRE_LEN;
}

// ---------------------------------------------------------------------------
// The image formats
//
// Protocol v3 devices take JPEG, rotated 180 degrees. The key images are
// 112x112, the image on each encoder's dial is 176x112, and the wide touch
// strip is one 800x128 picture with a quarter for each dial.
//
// The sizes are all multiples of 8 in width and 16 in height, which is what
// the P4's hardware JPEG encoder wants.
// ---------------------------------------------------------------------------

static constexpr uint16_t KEY_W = 112, KEY_H = 112;
static constexpr uint16_t DIAL_W = 176, DIAL_H = 112;
static constexpr uint16_t STRIP_W = 800, STRIP_H = 128;

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

static constexpr size_t   IN_REPORT_LEN = 512;
static constexpr uint8_t  IN_ACK[3]     = { 0x41, 0x43, 0x4B };  // "ACK"
static constexpr size_t   IN_MSG_OFF     = 9;    // the message ID
static constexpr size_t   IN_STATE_OFF   = 10;   // pressed / released
static constexpr size_t   IN_BITMAP_OFF  = 11;   // 16 button states

// What a report means.
enum class Event : uint8_t {
  None,
  ButtonDown,     // a key (with or without a display) was pressed
  EncoderDown,    // an encoder was pushed
  EncoderTwist,   // an encoder turned: which one, and by how much
};

struct EventInfo {
  Event    type = Event::None;
  uint8_t  index = 0;    // the key or the encoder
  int8_t   delta = 0;    // the twist, -1 or +1
};

// The message IDs, from opendeck-akp05/src/inputs.rs.
//
//   1..10   the ten keys with displays
//   0x00, 0x40..0x43, 0x38   the five buttons without displays
//   0x37, 0x35, 0x33, 0x36   the four encoder push buttons
//   0xa0/0xa1, 0x50/0x51,
//   0x90/0x91, 0x70/0x71       the four encoders, twisted: the low bit
//                              of the ID is the direction (-1, +1)
//
// The button index the reference uses (its keypad's own numbering) is
// returned, and the caller maps that to the physical position with
// mapPosition().
inline EventInfo decodeMessage(uint8_t msg) {
  EventInfo e;
  if (msg >= 1 && msg <= 10) {
    e.type = Event::ButtonDown;
    e.index = msg;                 // 1..10 - the keys with displays
    return e;
  }
  switch (msg) {
    case 0x00: e.index = 11; break;   // the buttons without displays
    case 0x41: e.index = 12; break;
    case 0x42: e.index = 13; break;
    case 0x43: e.index = 14; break;
    case 0x38: e.index = 15; break;
    case 0x37: e.type = Event::EncoderDown; e.index = 0; return e;
    case 0x35: e.type = Event::EncoderDown; e.index = 1; return e;
    case 0x33: e.type = Event::EncoderDown; e.index = 2; return e;
    case 0x36: e.type = Event::EncoderDown; e.index = 3; return e;
    case 0xa0: e.type = Event::EncoderTwist; e.index = 0; e.delta = -1; return e;
    case 0xa1: e.type = Event::EncoderTwist; e.index = 0; e.delta = +1; return e;
    case 0x50: e.type = Event::EncoderTwist; e.index = 1; e.delta = -1; return e;
    case 0x51: e.type = Event::EncoderTwist; e.index = 1; e.delta = +1; return e;
    case 0x90: e.type = Event::EncoderTwist; e.index = 2; e.delta = -1; return e;
    case 0x91: e.type = Event::EncoderTwist; e.index = 2; e.delta = +1; return e;
    case 0x70: e.type = Event::EncoderTwist; e.index = 3; e.delta = -1; return e;
    case 0x71: e.type = Event::EncoderTwist; e.index = 3; e.delta = +1; return e;
    default: return e;                 // Event::None
  }
  e.type = Event::ButtonDown;
  return e;
}

// The keypad's own key numbering is not the physical order: the reference
// remaps it (opendeck-akp05/src/device.rs, map_position). `is_encoder`
// shifts the encoders' own images by ten.
inline uint8_t mapPosition(uint8_t position, bool is_encoder) {
  if (is_encoder) position += 10;
  switch (position) {
    case 0:  return 10;
    case 1:  return 11;
    case 2:  return 12;
    case 3:  return 13;
    case 4:  return 14;
    case 5:  return 5;
    case 6:  return 6;
    case 7:  return 7;
    case 8:  return 8;
    case 9:  return 9;
    case 10: return 0;
    case 11: return 1;
    case 12: return 2;
    case 13: return 3;
    default: return 0xFF;
  }
}

// Is this report one of the device's own? Anything else is noise.
inline bool isAck(const uint8_t* p, uint16_t n) {
  return n > IN_STATE_OFF && p[0] == IN_ACK[0] && p[1] == IN_ACK[1] && p[2] == IN_ACK[2];
}

}  // namespace Opendeck
