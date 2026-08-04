// wled00/event_bus.h
// v3 event bus for WLED-MM-P4.
//
// A small, synchronous, typed event system. Producers (WLED core, usermods)
// call EventBus::publish(ev) which fans out to all registered usermods'
// onEvent() virtual hook in the publisher's task context.
//
// Handlers MUST be short and not acquire busMutex or perform blocking I/O —
// the publisher's task is held for the duration of the fan-out. This is
// enforced by code review, not at runtime.
//
// Why synchronous?  At <10 Hz event rate on a 240 MHz ESP32-P4, the
// handler cost is negligible and the simplicity is worth more than the
// decoupling an async queue would buy. We can layer an async queue on
// top later if a future subscriber needs it.

#pragma once

#include <stdint.h>

namespace wled {

// EventType — grow this list as new events are needed. Values are
// stable; do NOT renumber. New event types go at the bottom of the
// relevant category (lifecycle vs. hardware).
enum class EventType : uint16_t {
  // Lifecycle
  PresetApplied       = 0x0001,  // payload: { preset: uint8_t }
  PresetListMutated   = 0x0002,  // payload: { kind, slot }
  PlaylistStarted     = 0x0003,  // payload: { playlist, entry }
  PlaylistEnded       = 0x0004,  // payload: { playlist, hadEndPreset, endPreset }
  PowerEdge           = 0x0005,  // payload: { wasOff, isOff }
  EffectIndexChanged  = 0x0006,  // payload: { newIndex }
  PresetCycleRequested = 0x0007, // payload: { preset: uint8_t } — a usermod
                                 // (e.g. AutoPlaylist AutoChange) decided
                                 // to advance to this preset. Distinct
                                 // from PresetApplied (which fires for
                                 // ALL preset applications, including
                                 // user-initiated ones). Subscribers
                                 // can use this to know the change was
                                 // "automatic" rather than manual.
                                 // Not currently published by any core
                                 // code — defined for forward
                                 // compatibility (e.g., AutoPlaylist
                                 // v3 could publish it).
  SilenceEntered      = 0x0008,  // no payload — AutoPlaylist's silence
                                 // detector crossed the silence threshold
                                 // (silenceDetected false→true).
  SoundEntered        = 0x0009,  // no payload — AutoPlaylist's silence
                                 // detector crossed the sound threshold
                                 // (silenceDetected true→false).

  // Hardware
  UsbDeviceChanged    = 0x0101,  // payload: { connected, vid, pid, name }
};

// PresetListMutated "kind" values.
enum class PresetMutationKind : uint8_t {
  Saved    = 1,    // doSaveState() (a new or overwritten preset)
  Deleted  = 2,    // deletePreset()
};

// Event — the wire-format struct passed through the bus. POD; ~32 bytes
// when the union is fully populated. Pass by const ref at call sites.
struct Event {
  EventType type;
  uint32_t  timestamp_ms;
  uint8_t   source_id;  // USERMOD_ID_* of the publisher; 0 = WLED core

  union Payload {
    struct { uint8_t preset; } presetApplied;
    struct { uint8_t kind; uint8_t slot; } presetListMutated;
    struct { int16_t playlist; uint8_t entry; } playlistStarted;
    struct { int16_t playlist; bool hadEndPreset; uint8_t endPreset; } playlistEnded;
    struct { bool wasOff; bool isOff; } powerEdge;
    struct {
      uint8_t seg_id;        // segment whose effect index changed (0 = whole strip / unsegmented)
      uint8_t oldIndex;      // previous FX index, 255 = "unknown / first observation"
      uint8_t newIndex;      // new FX index (mirrors SEGMENT.mode after the change)
    } effectIndexChanged;
    struct { bool connected; uint16_t vid; uint16_t pid; char name[24]; } usbDevice;
    uint8_t raw[32];  // sized floor for the union; not used by any current event
  } payload;
};

// EventBus — synchronous fan-out. No state; the publisher is the
// iterator. Handlers run in the publisher's task context.
class EventBus {
 public:
  // Publish an event. Walks the registered usermods and calls each
  // one's onEvent() hook. Returns the number of usermods notified
  // (useful for tests; normally ignored).
  static unsigned publish(const Event& ev);
};

}  // namespace wled
