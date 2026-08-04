// wled00/event_log.cpp
// Implementation of the v3 debug event log.

#include "event_log.h"
// WLEDMM v3: forward declarations of the preset accessors we use in
// writePayload(). Defined in presets.cpp. We don't include
// fcn_declare.h here because event_log.h is included from headers
// that don't have the full UsermodManager context yet, and a
// transitive include of fcn_declare.h from event_log.h breaks
// the wled.h compile order (UsermodManager would be referenced
// before it's defined).
extern const char* getPresetQL(byte slot);
extern int8_t      getPresetLedmap(byte slot);
#include "src/dependencies/json/ArduinoJson-v6.h"

namespace wled {

// Static ring buffer. Single-threaded (the event bus is called from
// the publisher's task only), so no locking is needed.
namespace {
  Event     s_buf[kEventLogCapacity];
  unsigned  s_head    = 0;   // index of next write slot
  unsigned  s_count   = 0;   // current number of valid entries
  uint32_t  s_seq     = 0;   // monotonic sequence for ordering on read
  uint32_t  s_seq_buf[kEventLogCapacity] = {0};

  const char* eventTypeName(EventType t) {
    switch (t) {
      case EventType::PresetApplied:       return "PresetApplied";
      case EventType::PresetListMutated:   return "PresetListMutated";
      case EventType::PlaylistStarted:     return "PlaylistStarted";
      case EventType::PlaylistEnded:       return "PlaylistEnded";
      case EventType::PowerEdge:           return "PowerEdge";
      case EventType::EffectIndexChanged:  return "EffectIndexChanged";
      case EventType::PresetCycleRequested: return "PresetCycleRequested";
      case EventType::SilenceEntered:      return "SilenceEntered";
      case EventType::SoundEntered:        return "SoundEntered";
      case EventType::UsbDeviceChanged:    return "UsbDeviceChanged";
    }
    return "Unknown";
  }

  void writePayload(JsonObject& obj, EventType t, const Event::Payload& p) {
    switch (t) {
      case EventType::PresetApplied:
        obj["preset"] = p.presetApplied.preset;
        break;
      case EventType::PresetListMutated:
        obj["kind"] = (unsigned)p.presetListMutated.kind;
        obj["slot"] = p.presetListMutated.slot;
        // WLEDMM v3: surface ql/ledmap so the eventlog shows
        // what changed. Empty string / -1 means "not set".
        obj["ql"]     = getPresetQL(p.presetListMutated.slot);
        obj["ledmap"] = (int)getPresetLedmap(p.presetListMutated.slot);
        break;
      case EventType::PlaylistStarted:
        obj["playlist"] = p.playlistStarted.playlist;
        obj["entry"]   = p.playlistStarted.entry;
        break;
      case EventType::PlaylistEnded:
        obj["playlist"]      = p.playlistEnded.playlist;
        obj["hadEndPreset"]  = p.playlistEnded.hadEndPreset;
        obj["endPreset"]     = p.playlistEnded.endPreset;
        break;
      case EventType::PowerEdge:
        obj["wasOff"] = p.powerEdge.wasOff;
        obj["isOff"]  = p.powerEdge.isOff;
        break;
      case EventType::EffectIndexChanged:
        obj["seg_id"]   = p.effectIndexChanged.seg_id;
        obj["oldIndex"] = p.effectIndexChanged.oldIndex;
        obj["newIndex"] = p.effectIndexChanged.newIndex;
        break;
      case EventType::PresetCycleRequested:
        obj["preset"] = p.presetApplied.preset;
        break;
      case EventType::UsbDeviceChanged:
        obj["connected"] = p.usbDevice.connected;
        obj["vid"]       = p.usbDevice.vid;
        obj["pid"]       = p.usbDevice.pid;
        obj["name"]      = p.usbDevice.name;
        break;
    }
  }
}  // namespace

void EventLog::record(const Event& ev) {
  s_buf[s_head] = ev;
  s_seq_buf[s_head] = ++s_seq;
  s_head = (s_head + 1) % kEventLogCapacity;
  if (s_count < kEventLogCapacity) s_count++;
}

unsigned EventLog::size() {
  return s_count;
}

unsigned EventLog::toJson(JsonArray& arr, unsigned long since_ms) {
  // Walk the buffer in most-recent-first order. Newest entry is at
  // (s_head - 1) mod capacity. Oldest is at s_head (when full) or 0
  // (when not yet wrapped).
  if (s_count == 0) return 0;
  unsigned written = 0;
  // Iterate from newest to oldest.
  for (unsigned i = 0; i < s_count; i++) {
    // The i-th most-recent entry sits at slot (s_head - 1 - i) mod cap.
    int idx = (int)s_head - 1 - (int)i;
    while (idx < 0) idx += (int)kEventLogCapacity;
    const Event& e = s_buf[idx];
    if (e.timestamp_ms < since_ms) continue;
    JsonObject obj = arr.createNestedObject();
    obj["type"]         = eventTypeName(e.type);
    obj["timestamp_ms"] = e.timestamp_ms;
    obj["source_id"]    = e.source_id;
    obj["seq"]          = s_seq_buf[idx];
    JsonObject payload = obj.createNestedObject("payload");
    writePayload(payload, e.type, e.payload);
    written++;
  }
  return written;
}

void EventLog::clear() {
  s_head = 0;
  s_count = 0;
  s_seq = 0;
}

}  // namespace wled
