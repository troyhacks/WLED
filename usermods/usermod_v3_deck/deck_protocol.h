// deck_protocol.h
// Wire format for the Soomfon CN003 / Mirabox N4-family "CRT" command protocol.
//
// Reconstructed from the `mirajazz` Rust crate (a hardfork of `elgato-streamdeck`),
// which is what the OpenDeck plugin `opendeck-akp05` links against. Unlike Elgato
// hardware, these devices do NOT use a binary opcode/report-ID scheme — every
// outbound packet is a report-ID byte, an ASCII "CRT" signature, two reserved
// zero bytes, then an ASCII mnemonic and its payload.
//
//   00 43 52 54 00 00 | "SETLB" | R G B  R G B  R G B  R G B
//   ^--- header 6B ---^
//
// The mnemonic length varies per command (DIS/LIG/BAT/CLE/STP/HAN/MOD are 3
// bytes, LBLIG/SETLB are 5, CONNECT is 7), so the builders below are explicit
// about each packet's total length rather than assuming a fixed opcode width.
//
// Everything here is header-only and side-effect free: callers pass in an output
// buffer and get back the packet length. No allocation, no I/O.
//
// Build flag: -D USERMOD_DECK_USB

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace DeckProto {

// ---------------------------------------------------------------------------
// Frame constants
// ---------------------------------------------------------------------------

// Report ID 0x00 + "CRT" + two reserved zero bytes. Every command starts here.
static constexpr uint8_t HDR[6] = { 0x00, 0x43, 0x52, 0x54, 0x00, 0x00 };
static constexpr size_t   HDR_LEN = sizeof(HDR);

// ASCII mnemonics.
static constexpr uint8_t OP_DIS[]     = { 0x44, 0x49, 0x53 };                    // "DIS"
static constexpr uint8_t OP_LIG[]     = { 0x4C, 0x49, 0x47 };                    // "LIG"
static constexpr uint8_t OP_LBLIG[]   = { 0x4C, 0x42, 0x4C, 0x49, 0x47 };        // "LBLIG"
static constexpr uint8_t OP_SETLB[]   = { 0x53, 0x45, 0x54, 0x4C, 0x42 };        // "SETLB"
static constexpr uint8_t OP_BAT[]     = { 0x42, 0x41, 0x54 };                    // "BAT"
static constexpr uint8_t OP_CLE[]     = { 0x43, 0x4C, 0x45 };                    // "CLE"
static constexpr uint8_t OP_STP[]     = { 0x53, 0x54, 0x50 };                    // "STP"
static constexpr uint8_t OP_HAN[]     = { 0x48, 0x41, 0x4E };                    // "HAN"
static constexpr uint8_t OP_CONNECT[] = { 0x43, 0x4F, 0x4E, 0x4E, 0x45, 0x43, 0x54 }; // "CONNECT"
static constexpr uint8_t OP_MOD[]     = { 0x4D, 0x4F, 0x44 };                    // "MOD"

// Largest fixed-size command packet (BAT header is the longest: 6 + 3 + 2 + 2 + 1).
static constexpr size_t MAX_CMD_LEN = 14;

// Every outbound packet — command or image chunk — is zero-padded to a fixed
// wire length of 1 + PACKET_SIZE. This is not a convenience: `mirajazz` sends
// each command as a 9-to-14-byte buffer resized up to 1025 bytes before
// handing it to the writer (`write_extended_data`), and the deck ignores
// anything shorter. A bare 9-byte "DIS" or "LIG" is silently dropped — which is
// why the key backlight never lit on the first bench attempt.
//
// PACKET_SIZE is 1024 for protocol v2/v3 (what the CN003 reports) and 512 for
// v0/v1, matching the crate's `packet_size` field.
static constexpr uint16_t PACKET_SIZE = 1024;
static constexpr uint16_t CMD_WIRE    = 1 + PACKET_SIZE;   // 1025

// Image transport: BAT declares a payload length, then the payload is streamed
// as fixed-size chunks. Each chunk on the wire is a leading 0x00 (report ID)
// followed by up to CHUNK_PAYLOAD bytes of JPEG data, zero-padded to the full
// report length. CHUNK_PAYLOAD excludes that leading report-ID byte.
static constexpr uint16_t CHUNK_PAYLOAD = PACKET_SIZE;
static constexpr uint16_t CHUNK_WIRE    = CMD_WIRE;

// Largest outbound transfer we'll ever submit — commands and chunks are padded
// to the same wire length, so this covers both.
static constexpr uint16_t MAX_OUT_BYTES = CMD_WIRE;

// Pass as the key argument to cle() to clear every key at once.
static constexpr uint8_t CLEAR_ALL = 0xFF;

// Fixed packet lengths, so callers can assert rather than trust the builders.
static constexpr size_t LEN_DIS     = 9;    // header + "DIS"
static constexpr size_t LEN_STP     = 9;    // header + "STP"
static constexpr size_t LEN_HAN     = 9;    // header + "HAN"
static constexpr size_t LEN_LIG     = 12;   // header + "LIG" + 00 00 + percent
static constexpr size_t LEN_LBLIG   = 11;   // header + "LBLIG" + percent
static constexpr size_t LEN_CLE     = 12;   // header + "CLE" + 00 00 + key
static constexpr size_t LEN_CONNECT = HDR_LEN + 7;
static constexpr size_t LEN_BAT     = 14;   // header + "BAT" + 00 00 + len16 + key

// ---------------------------------------------------------------------------
// Command builders
//
// Each returns the number of bytes written, or 0 if the buffer is too small.
// `out` must have room for at least MAX_OUT_BYTES for the streaming builders.
// ---------------------------------------------------------------------------

// "DIS" — initialize / reset the display. Sent first on every connect.
static inline size_t dis(uint8_t* out, size_t cap) {
  if (cap < LEN_DIS) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_DIS, sizeof(OP_DIS));
  return LEN_DIS;
}

// "LIG" — keypad (key backlight) brightness, 0..100.
static inline size_t lig(uint8_t* out, size_t cap, uint8_t percent) {
  if (cap < LEN_LIG) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_LIG, sizeof(OP_LIG));
  out[9] = 0x00;
  out[10] = 0x00;
  out[11] = (percent > 100) ? 100 : percent;
  return LEN_LIG;
}

// "LBLIG" — rotary-knob LED ring brightness, 0..100.
// NOTE: the CN003 has no LED rings on its encoders (knob state is shown on the
// touch strip above them), so this is a no-op on this hardware. Kept for
// completeness because it is in the protocol.
static inline size_t lblig(uint8_t* out, size_t cap, uint8_t percent) {
  if (cap < LEN_LBLIG) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_LBLIG, sizeof(OP_LBLIG));
  out[10] = (percent > 100) ? 100 : percent;
  return LEN_LBLIG;
}

// "SETLB" — set the colour of each knob's LED ring. `colors` holds
// 3 * n RGB triples; n is taken from the count.
// NOTE: as with LBLIG, this hardware has no LED rings, so there is nothing to
// paint. There is therefore no cheap feedback path on this deck — every visible
// state change has to go through the BAT image pipeline, which means Phase 3
// has to be planned as a real image-push path rather than a tweak of ring LEDs.
static inline size_t setlb(uint8_t* out, size_t cap,
                           const uint8_t* colors, uint8_t count) {
  const size_t need = HDR_LEN + sizeof(OP_SETLB) + (size_t)count * 3;
  if (cap < need) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_SETLB, sizeof(OP_SETLB));
  memcpy(out + HDR_LEN + sizeof(OP_SETLB), colors, (size_t)count * 3);
  return need;
}

// "CLE" — clear one key's image, or every key when key == CLEAR_ALL.
// Note the device indexes keys from 1, so key 0 maps to wire value 1.
static inline size_t cle(uint8_t* out, size_t cap, uint8_t key) {
  if (cap < LEN_CLE) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_CLE, sizeof(OP_CLE));
  out[9] = 0x00;
  out[10] = 0x00;
  out[11] = (key == CLEAR_ALL) ? CLEAR_ALL : (uint8_t)(key + 1);
  return LEN_CLE;
}

// "STP" — commit. Sent after a batch of BAT writes; without it the device
// keeps buffering and nothing appears on screen.
static inline size_t stp(uint8_t* out, size_t cap) {
  if (cap < LEN_STP) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_STP, sizeof(OP_STP));
  return LEN_STP;
}

// "HAN" — put the device to sleep.
static inline size_t han(uint8_t* out, size_t cap) {
  if (cap < LEN_HAN) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_HAN, sizeof(OP_HAN));
  return LEN_HAN;
}

// "CONNECT" — keepalive ping. The device drops the session if this is not
// seen roughly every 10 seconds.
static inline size_t connect(uint8_t* out, size_t cap) {
  if (cap < LEN_CONNECT) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_CONNECT, sizeof(OP_CONNECT));
  return LEN_CONNECT;
}

// "MOD" — device mode (mode is sent as 0x30 + n).
static constexpr size_t LEN_MOD = 12;

static inline size_t mod(uint8_t* out, size_t cap, uint8_t mode) {
  if (cap < LEN_MOD) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_MOD, sizeof(OP_MOD));
  out[9] = 0x00;
  out[10] = 0x00;
  out[11] = (uint8_t)(0x30 + mode);
  return LEN_MOD;
}

// "BAT" — announce an image for one key. Declares the payload length that
// follows as a sequence of chunks. `key` is 0-based; the wire value is key+1.
static inline size_t bat(uint8_t* out, size_t cap, uint8_t key, uint16_t len) {
  if (cap < LEN_BAT) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_BAT, sizeof(OP_BAT));
  out[9] = 0x00;
  out[10] = 0x00;
  out[11] = (uint8_t)(len >> 8);
  out[12] = (uint8_t)(len & 0xFF);
  out[13] = (uint8_t)(key + 1);
  return LEN_BAT;
}

// Shutdown sequence: "CLE" with the magic key 0x44 0x43, followed by "HAN".
// The device lights all knobs solid red in response.
static constexpr size_t LEN_SHUTDOWN = 13;

static inline size_t shutdown_cmd(uint8_t* out, size_t cap) {
  if (cap < LEN_SHUTDOWN) return 0;
  memcpy(out, HDR, HDR_LEN);
  memcpy(out + HDR_LEN, OP_CLE, sizeof(OP_CLE));
  out[9] = 0x00;
  out[10] = 0x00;
  out[11] = 0x44;
  out[12] = 0x43;
  return LEN_SHUTDOWN;
}

// ---------------------------------------------------------------------------
// Image chunk framing
//
// buildChunk() lays down the leading report-ID byte plus up to CHUNK_PAYLOAD
// bytes of payload, zero-padding to the full wire length. The device reads the
// declared length from BAT to know how many chunks matter, so trailing padding
// in the final chunk is ignored.
// ---------------------------------------------------------------------------
static inline size_t buildChunk(uint8_t* out, size_t cap,
                                const uint8_t* payload, uint16_t len) {
  if (cap < CHUNK_WIRE || len > CHUNK_PAYLOAD) return 0;
  out[0] = 0x00;
  memcpy(out + 1, payload, len);
  memset(out + 1 + len, 0, CHUNK_WIRE - 1 - len);
  return CHUNK_WIRE;
}

// ---------------------------------------------------------------------------
// Inbound report parsing
//
// 512-byte interrupt IN reports. Only reports starting with the "ACK" magic
// carry input; everything else is noise and must be ignored. This is verbatim
// from the crate's `DeviceStateReader::read_input` (mirajazz src/state.rs),
// which gates on `data.starts_with(&[65, 67, 75])` — the same three bytes.
//
//   [0..2]  "ACK"  (0x41 0x43 0x4B)
//   [9]     input  — message ID: which key or encoder changed
//   [10]    state  — pressed / released
//
// [9] is a message ID, not a plain index, and is only interpreted once the
// ACK check passes. The CN003 is protocol v3, which reports
// supports_both_keypress_states == false, so state is not meaningful here —
// the crate substitutes 0x01 for it and treats every key as a down+up pair.
// Encoder twist arrives as an index into a delta vector inside the same report.
// See the README for the full input-code table; none of it is decoded yet.
// ---------------------------------------------------------------------------

static constexpr size_t   IN_REPORT_LEN  = 512;
static constexpr uint8_t  IN_ACK[3]      = { 0x41, 0x43, 0x4B };  // "ACK"
static constexpr size_t   IN_INDEX_OFF   = 9;
static constexpr size_t   IN_STATE_OFF   = 10;

inline bool inIsAck(const uint8_t* p, uint16_t n) {
  return n > IN_STATE_OFF && p[0] == IN_ACK[0] && p[1] == IN_ACK[1] && p[2] == IN_ACK[2];
}

}  // namespace DeckProto
