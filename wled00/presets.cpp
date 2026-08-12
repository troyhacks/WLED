#include "wled.h"

/*
 * Methods to handle saving and loading presets to/from the filesystem
 */

#ifdef ARDUINO_ARCH_ESP32
static char* tmpRAMbuffer = nullptr;
#endif

static volatile byte presetToApply = 0;
static volatile byte callModeToApply = 0;
static volatile byte presetToSave = 0;
static volatile int8_t saveLedmap = -1;
static char quickLoad[9];
static char saveName[33];
static bool includeBri = true, segBounds = true, selectedOnly = false, playlistSave = false;

PresetMetadata* presetCache = nullptr;

static const char* getFileName(bool persist = true) {
  return persist ? "/presets.json" : "/tmp.json";
}

bool presetsSavePending(void) {  // WLEDMM true if presetToSave, playlistSave or saveLedmap
  if (presetToSave > 0) return(true);
  if (playlistSave == true) return(true);
  if (saveLedmap >= 0) return(true);
  return(false);
}
bool presetsActionPending(void) {  // WLEDMM true if presetToApply, presetToSave, playlistSave or saveLedmap
  if (presetToApply > 0) return(true);
  if (presetToSave > 0) return(true);
  if (playlistSave == true) return(true);
  if (saveLedmap >= 0) return(true);
  return(false);
}

static void doSaveState() {

  bool persist = (presetToSave < 251);
  const char* filename = getFileName(persist);

  if (!requestJSONBufferLock(10)) return; // will set fileDoc

  initPresetsFile(); // just in case if someone deleted presets.json using /edit
  JsonObject sObj = doc.to<JsonObject>();

  DEBUG_PRINTLN(F("Serialize current state"));
  if (playlistSave) {
    serializePlaylist(sObj);
    if (includeBri) sObj["on"] = true;
  } else {
    serializeState(sObj, true, includeBri, segBounds, selectedOnly);
  }
  sObj["n"] = saveName;
  if (quickLoad[0]) sObj[F("ql")] = quickLoad;
  if (saveLedmap >= 0) sObj[F("ledmap")] = saveLedmap;
  /*
    #ifdef WLED_DEBUG
      DEBUG_PRINTLN(F("Serialized preset"));
      serializeJson(doc,Serial);
      DEBUG_PRINTLN();
    #endif
  */
  #if defined(ARDUINO_ARCH_ESP32)
  if (!persist) {
    if (tmpRAMbuffer != nullptr) free(tmpRAMbuffer);
    size_t len = measureJson(*fileDoc) + 1;
    DEBUG_PRINTLN(len);
    // if possible use SPI RAM on ESP32
    tmpRAMbuffer = (char*)heap_caps_calloc_prefer(len, 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED, MALLOC_CAP_INTERNAL);
    if (tmpRAMbuffer != nullptr) {
      serializeJson(*fileDoc, tmpRAMbuffer, len);
    } else {
      writeObjectToFileUsingId(filename, presetToSave, fileDoc);
    }
  } else
    #endif
    writeObjectToFileUsingId(filename, presetToSave, fileDoc);

  if (persist) presetsModifiedTime = toki.second(); //unix time
  releaseJSONBufferLock();
  updateFSInfo();

  // WLEDMM v3: publish PresetListMutated. The preset cache
  // (presetCache[]) was already updated above; this event tells
  // subscribers to refresh their UI / invalidate their state. We
  // publish AFTER releaseJSONBufferLock so subscribers that want
  // to do their own file reads can grab the lock.
  if (persist && presetToSave > 0 && presetToSave <= 250) {
    wled::Event ev = {};
    ev.type = wled::EventType::PresetListMutated;
    ev.timestamp_ms = millis();
    ev.source_id = 0;  // WLED core
    ev.payload.presetListMutated.kind = (uint8_t)wled::PresetMutationKind::Saved;
    ev.payload.presetListMutated.slot = presetToSave;
    wled::EventBus::publish(ev);
  }

  // clean up
  saveLedmap = -1;
  presetToSave = 0;
  saveName[0] = '\0';
  quickLoad[0] = '\0';
  playlistSave = false;
}

bool getPresetName(byte index, String& name) {
  if (!requestJSONBufferLock(19)) return false;
  bool presetExists = false;
  if (readObjectFromFileUsingId(getFileName(), index, &doc)) {
    JsonObject fdo = doc.as<JsonObject>();
    if (fdo["n"]) {
      name = (const char*)(fdo["n"]);
      presetExists = true;
    }
  }
  releaseJSONBufferLock();
  return presetExists;
}

void initPresetsFile() {
  if (WLED_FS.exists(getFileName())) return;

  StaticJsonDocument<64> doc;
  JsonObject sObj = doc.to<JsonObject>();
  sObj.createNestedObject("0");
  File f = WLED_FS.open(getFileName(), "w");
  if (!f) {
    errorFlag = ERR_FS_GENERAL;
    return;
  }
  serializeJson(doc, f);
  f.close();
}

bool applyPreset(byte index, byte callMode) {
  DEBUG_PRINT(F("Request to apply preset: "));
  DEBUG_PRINTLN(index);
  presetToApply = index;
  callModeToApply = callMode;
  return true;
}

// apply preset or fallback to a effect and palette if it doesn't exist
void applyPresetWithFallback(uint8_t index, uint8_t callMode, uint8_t effectID, uint8_t paletteID) {
  applyPreset(index, callMode);
  //these two will be overwritten if preset exists in handlePresets()
  effectCurrent = effectID;
  effectPalette = paletteID;
  // WLEDMM v3: publish EffectIndexChanged so subscribers (e.g., a
  // future LED feedback usermod, or anything watching for the legacy
  // global) can react. Posted AFTER the assignment so subscribers
  // reading the global see the new value.
  {
    wled::Event ev = {};
    ev.type = wled::EventType::EffectIndexChanged;
    ev.timestamp_ms = millis();
    ev.source_id = 0;  // WLED core
    ev.payload.effectIndexChanged.newIndex = effectID;
    wled::EventBus::publish(ev);
  }
}

void handlePresets() {
  if (presetToSave) {
    doSaveState();
    return;
  }

  if (presetToApply == 0 || fileDoc) return; // no preset waiting to apply, or JSON buffer is already allocated, return to loop until free

  bool changePreset = false;
  uint8_t tmpPreset = presetToApply; // store temporary since deserializeState() may call applyPreset()
  uint8_t tmpMode = callModeToApply;

  JsonObject fdo;
  const char* filename = getFileName(tmpPreset < 255);

  if (!requestJSONBufferLock(9)) return;  // will also assign fileDoc

  presetToApply = 0; //clear request for preset
  callModeToApply = 0;
  byte presetErrorFlag = ERR_NONE;

  DEBUG_PRINT(F("Applying preset: "));
  DEBUG_PRINTLN(tmpPreset);

  bool haveLocked = false;
  #if defined(ARDUINO_ARCH_ESP32)   // WLEDMM we apply this workaround to all esp32 boards (S3 and classic esp32 included)
  //#if defined(ARDUINO_ARCH_ESP32S2) || defined(ARDUINO_ARCH_ESP32C3)
  // in case we are called from web UI, wait until strip.service() is done
  // if (!suspendStripService) { suspendStripService = true; haveLocked = true; } // only lock service if not locked already
  unsigned long waitstart = millis();
  // while (strip.isServicing() && millis() - waitstart < FRAMETIME_FIXED) delay(1); // wait for effects to finish updating

  strip.fill(BLACK); strip.show(); // experimental: set LEDs to black while new preset loads (instead of freezing effects)

  unsigned long start = millis();
  // while (strip.isUpdating() && millis() - start < FRAMETIME_FIXED) delay(1); // wait for strip to finish updating, accessing FS during sendout causes glitches // WLEDMM delay instead of yield
  #endif

  #ifdef ARDUINO_ARCH_ESP32
  if (tmpPreset == 255 && tmpRAMbuffer != nullptr) {
    deserializeJson(*fileDoc, tmpRAMbuffer);
    if ((errorFlag == ERR_FS_PLOAD) || (errorFlag == ERR_JSON)) errorFlag = ERR_NONE;  // WLEDMM only reset our own error
  } else
    #endif
  {
    presetErrorFlag = readObjectFromFileUsingId(filename, tmpPreset, fileDoc) ? ERR_NONE : ERR_FS_PLOAD;
    if ((errorFlag == ERR_FS_PLOAD) || (errorFlag == ERR_JSON)) errorFlag = ERR_NONE;  // WLEDMM only reset our own error
    if (presetErrorFlag == ERR_FS_PLOAD) errorFlag = presetErrorFlag;
  }
  // if (haveLocked) suspendStripService = false; // WLEDMM unlock effects after presets file was loaded
  fdo = fileDoc->as<JsonObject>();

  if (loadedLedmap_lock) {
    changePreset = true; // force the override
  }

  //HTTP API commands
  const char* httpwin = fdo["win"];
  if (httpwin) {
    String apireq = "win"; // reduce flash string usage
    apireq += F("&IN&"); // internal call
    apireq += httpwin;
    handleSet(nullptr, apireq, false); // may call applyPreset() via PL=
    setValuesFromFirstSelectedSeg(); // fills legacy values
    changePreset = true;
  } else {
    if (!fdo["seg"].isNull() || !fdo["on"].isNull() || !fdo["bri"].isNull() || !fdo["nl"].isNull() || !fdo["ps"].isNull() || !fdo[F("playlist")].isNull()) changePreset = true;
    if (!(tmpMode == CALL_MODE_BUTTON_PRESET && fdo["ps"].is<const char*>() && strchr(fdo["ps"].as<const char*>(), '~') != strrchr(fdo["ps"].as<const char*>(), '~')))
      fdo.remove("ps"); // remove load request for presets to prevent recursive crash (if not called by button and contains preset cycling string "1~5~")
    deserializeState(fdo, CALL_MODE_NO_NOTIFY, tmpPreset); // may change presetToApply by calling applyPreset()
  }
  if (!presetErrorFlag && tmpPreset < 255 && changePreset) currentPreset = tmpPreset;

  #if defined(ARDUINO_ARCH_ESP32)
  //Aircoookie recommended not to delete buffer
  if (tmpPreset == 255 && tmpRAMbuffer != nullptr) {
    free(tmpRAMbuffer);
    tmpRAMbuffer = nullptr;
  }
  #endif

  releaseJSONBufferLock(); // will also clear fileDoc
  if (changePreset) notify(tmpMode); // force UDP notification
  stateUpdated(tmpMode);  // was colorUpdated() if anything breaks
  // WLEDMM v3: publish PresetApplied after stateUpdated so subscribers
  // see the post-state-change world. Payload carries the preset id
  // explicitly so handlers don't need to read currentPreset (which
  // stateUpdated may have just wiped to 0 via the led.cpp:106 wipe).
  if (changePreset && tmpPreset > 0 && tmpPreset < 255) {
    wled::Event ev = {};
    ev.type = wled::EventType::PresetApplied;
    ev.timestamp_ms = millis();
    ev.source_id = 0;  // WLED core
    ev.payload.presetApplied.preset = tmpPreset;
    wled::EventBus::publish(ev);
  }
  updateInterfaces(tmpMode);
}

//called from handleSet(PS=) [network callback (fileDoc==nullptr), IR (irrational), deserializeState, UDP] and deserializeState() [network callback (filedoc!=nullptr)]
void savePreset(byte index, const char* pname, JsonObject sObj) {
  if (index == 0 || (index > 250 && index < 255)) return;
  if (pname) strlcpy(saveName, pname, 33);
  else {
    if (sObj["n"].is<const char*>()) strlcpy(saveName, sObj["n"].as<const char*>(), 33);
    else                             sprintf_P(saveName, PSTR("Preset %d"), index);
  }

  DEBUG_PRINT(F("Saving preset (")); DEBUG_PRINT(index); DEBUG_PRINT(F(") ")); DEBUG_PRINTLN(saveName);

  presetToSave = index;
  playlistSave = false;
  if (sObj[F("ql")].is<const char*>()) strlcpy(quickLoad, sObj[F("ql")].as<const char*>(), 9); // client limits QL to 2 chars, buffer for 8 bytes to allow unicode

  if (sObj.size() == 0 || sObj["o"].isNull()) { // no "o" means not a playlist or custom API call, saving of state is async (not immediately)
    includeBri = sObj["ib"].as<bool>() || sObj.size() == 0 || index == 255; // temporary preset needs brightness
    segBounds = sObj["sb"].as<bool>() || sObj.size() == 0 || index == 255; // temporary preset needs bounds
    selectedOnly = sObj[F("sc")].as<bool>();
    saveLedmap = sObj[F("ledmap")] | -1;
  } else {
    // this is a playlist or API call
    if (sObj[F("playlist")].isNull()) {
      // we will save API call immediately (often causes presets.json corruption)
      presetToSave = 0;
      if (index > 250 || !fileDoc) return; // cannot save API calls to temporary preset (255)
      sObj.remove("o");
      sObj.remove("v");
      sObj.remove("time");
      sObj.remove(F("error"));
      sObj.remove(F("psave"));
      if (sObj["n"].isNull()) sObj["n"] = saveName;
      initPresetsFile(); // just in case if someone deleted presets.json using /edit
      writeObjectToFileUsingId(getFileName(index < 255), index, fileDoc);
      presetsModifiedTime = toki.second(); //unix time
      updateFSInfo();
    } else {
      // store playlist
      // WARNING: playlist will be loaded in json.cpp after this call and will have repeat counter increased by 1
      includeBri = true; // !sObj["on"].isNull();
      playlistSave = true;
    }
  }
  if (presetCache != nullptr && index > 0 && index <= 250) {
    presetCache[index].exists = true;
    presetCache[index].isPlaylist = playlistSave; // playlistSave is set earlier
    String safeName = strip_unicode(saveName);
    strlcpy(presetCache[index].name, safeName.c_str(), sizeof(presetCache[index].name));
    #if defined(CONFIG_SOC_PPA_SUPPORTED)
    update_screen_background = true;
    #endif
  }
}

void deletePreset(byte index) {
  StaticJsonDocument<24> empty;
  writeObjectToFileUsingId(getFileName(), index, &empty);
  presetsModifiedTime = toki.second(); //unix time
  updateFSInfo();
  if (presetCache != nullptr && index > 0 && index <= 250) {
    presetCache[index].exists = false;
    presetCache[index].isPlaylist = false;
    presetCache[index].repeat = 0;  // WLEDMM v3
    presetCache[index].ql[0] = '\0';  // WLEDMM v3
    presetCache[index].ledmap = -1;   // WLEDMM v3
    presetCache[index].name[0] = '\0';
    #if defined(CONFIG_SOC_PPA_SUPPORTED)
    update_screen_background = true;
    #endif
  }
  // WLEDMM v3: publish PresetListMutated so subscribers can react
  // (e.g., the MIDI usermod's onEvent(PresetListMutated) handler
  // forces the WS push with cooldown bypass).
  if (index > 0 && index <= 250) {
    wled::Event ev = {};
    ev.type = wled::EventType::PresetListMutated;
    ev.timestamp_ms = millis();
    ev.source_id = 0;  // WLED core
    ev.payload.presetListMutated.kind = (uint8_t)wled::PresetMutationKind::Deleted;
    ev.payload.presetListMutated.slot = index;
    wled::EventBus::publish(ev);
  }
}

bool getCachedPresetMetadata(byte index, String& name, bool& isPlaylist) {
  if (presetCache == nullptr) {
    return false;
  }

  if (index == 0 || index > 250) {
    return false;
  }

  if (presetCache[index].exists) {
    name = presetCache[index].name;
    isPlaylist = presetCache[index].isPlaylist;
    return true;
  }

  // Preset does not exist
  name = "";
  isPlaylist = false;
  return false;
}

void buildPresetCache() {
  if (presetCache == nullptr) {
    USER_PRINTLN(F("Allocating preset cache..."));

    // Use heap_caps_calloc to get zero-initialized memory from PSRAM
    // MALLOC_CAP_SPIRAM is the flag for PSRAM
    presetCache = (PresetMetadata*)heap_caps_calloc_prefer(251, sizeof(PresetMetadata), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT, MALLOC_CAP_DEFAULT);

    // If it's *still* null, we're out of memory.
    if (presetCache == nullptr) {
      USER_PRINTLN(F("FATAL: Failed to allocate preset cache."));
      return; // Can't continue
    }
  }
  // --- End allocation ---

  USER_PRINTLN(F("Building preset cache..."));
  // Clear the old cache.
  // BUG FIX: sizeof(presetCache) is the size of the pointer (4 bytes), not the array.
  // Use 251 * sizeof(PresetMetadata) so a rebuild actually zeros every entry.
  // (On first build heap_caps_calloc_prefer has already zeroed memory, so this
  // only matters for subsequent rebuilds, but it was previously leaving 250 slots
  // with stale data.)
  memset(presetCache, 0, 251 * sizeof(PresetMetadata));

  if (!requestJSONBufferLock(20)) {
    USER_PRINTLN(F("Preset cache build failed (lock)."));
    return; // Failed to get lock
  }

  if (!readObjectFromFile(getFileName(), nullptr, &doc)) {
    releaseJSONBufferLock();
    USER_PRINTLN(F("Preset cache build failed (read)."));
    return; // File not found or corrupt
  }

  JsonObject root = doc.as<JsonObject>();
  for (int i = 1; i <= 250; i++) {
    char id_str[4];
    sprintf(id_str, "%d", i);

    if (root.containsKey(id_str)) {
      JsonObject presetObj = root[id_str];
      presetCache[i].exists = true; // Mark as existing

      presetCache[i].isPlaylist = !presetObj[F("playlist")].isNull();

      // WLEDMM v3: capture the playlist repeat value so subscribers
      // (e.g., the MIDI usermod) can color an idle playlist pad
      // differently for finite vs. infinite playlists without
      // re-reading /presets.json.
      if (presetCache[i].isPlaylist) {
        JsonObject pl = presetObj[F("playlist")];
        presetCache[i].repeat = (uint8_t)(pl[F("repeat")].as<int>() | 0);
      }

      // WLEDMM v3: capture quickload name and ledmap (the same
      // fields doSaveState() writes when persisting). Both default
      // to "unset" via the memset(0) at the top of this function.
      if (presetObj[F("ql")].is<const char*>()) {
        strlcpy(presetCache[i].ql, presetObj[F("ql")].as<const char*>(),
                sizeof(presetCache[i].ql));
      }
      if (!presetObj[F("ledmap")].isNull()) {
        presetCache[i].ledmap = (int8_t)presetObj[F("ledmap")].as<int>();
      } else {
        presetCache[i].ledmap = -1;  // explicit "no ledmap"
      }

      if (presetObj["n"]) {
        // sanitize the JSON string before copying
        String safeName = strip_unicode((const char*)presetObj["n"]);
        strlcpy(presetCache[i].name, safeName.c_str(), sizeof(presetCache[i].name));
      }

      // else: name is already blank from memset
    }
  }
  releaseJSONBufferLock(); // Unlock
  USER_PRINTLN(F("Preset cache build complete."));
}

bool getCachedPresetExists(int id) {
  // valid range is 1..250
  if (id < 1 || id > 250) {
    return false;
  }

  // if cache hasn’t been allocated yet, nothing exists
  if (presetCache == nullptr) {
    return false;
  }

  return presetCache[id].exists;
}

byte getRandomPresetId() {
  // 1. Safety check: Ensure cache exists
  if (presetCache == nullptr) return 0;

  // 2. Temporary storage for eligible preset IDs
  // We use a size of 250 because that is the max number of presets WLED supports
  byte validCandidates[250];
  byte count = 0;

  // 3. Iterate through all possible slots
  for (byte i = 1; i <= 250; i++) {
    // Constraint Check:
    // 1. presetCache[i].exists must be true
    // 2. presetCache[i].isPlaylist must be false
    if (presetCache[i].exists && !presetCache[i].isPlaylist) {
      validCandidates[count++] = i;
    }
  }

  // 4. If no valid presets found, return 0
  if (count == 0) return 0;

  // 5. Pick a random index from the list of candidates
  // random(max) returns a number from 0 to max-1
  byte randomIndex = random(count);

  // 6. Return the actual Preset ID
  return validCandidates[randomIndex];
}

// Return the next existing preset after `currentId`.
// Wraps around to 1 if needed. Returns -1 if none found.
int getNextPreset(int currentId) {
  if (presetCache == nullptr) return -1;
  if (currentId < 1 || currentId > 250) return -1;

  for (int i = currentId + 1; i <= 250; i++) {
    if (presetCache[i].exists) return i;
  }
  // wrap around to beginning
  for (int i = 1; i < currentId; i++) {
    if (presetCache[i].exists) return i;
  }
  return -1; // no presets at all
}

// Return the previous existing preset before `currentId`.
// Wraps around to 250 if needed. Returns -1 if none found.
int getPreviousPreset(int currentId) {
  if (presetCache == nullptr) return -1;
  if (currentId < 1 || currentId > 250) return -1;

  for (int i = currentId - 1; i >= 1; i--) {
    if (presetCache[i].exists) return i;
  }
  // wrap around to end
  for (int i = 250; i > currentId; i--) {
    if (presetCache[i].exists) return i;
  }
  return -1; // no presets at all
}

// --- Preset pool builder from presetCache ---
std::vector<int> buildPresetPool() {
  std::vector<int> pool;
  if (presetCache == nullptr) return pool;

  for (int i = 1; i <= 250; i++) {
    if (presetCache[i].exists) {
      pool.push_back(i);
    }
  }
  return pool;
}

// Call this once when starting a track
void initPresetMapping() {
  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  auto pool = buildPresetPool();
  if (!pool.empty()) {
    prolink_presetOffset = random(pool.size()); // randomized start
    USER_PRINTLN(F("Preset mapping initialized."));
    for (int i = 0; i < (int)pool.size(); i++) {
      int presetId = pool[(prolink_presetOffset + i) % pool.size()];
      USER_PRINTF("Pool[%d] = Preset %d (%s)\n",
        i, presetId, presetCache[presetId].name);
    }
  } else {
    USER_PRINTLN(F("No presets available in cache."));
  }
  #endif
}

// --- Phrase → Preset mapping ---
int getPresetForPhrase(int phraseIdx, const std::vector<int>& pool) {
  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  if (pool.empty()) return -1;
  int presetCount = pool.size();
  return pool[(prolink_presetOffset + phraseIdx) % presetCount];
  #else
  return 0;
  #endif
}

// --- No-repeat variant (avoids consecutive duplicates) ---
int getPresetForPhraseNoRepeat(int phraseIdx, const std::vector<int>& pool) {
  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  if (pool.empty()) return -1;
  int presetCount = pool.size();
  int preset = pool[(prolink_presetOffset + phraseIdx) % presetCount];

  if (phraseIdx > 0) {
    int prev = pool[(prolink_presetOffset + phraseIdx - 1) % presetCount];
    if (preset == prev && presetCount > 1) {
      preset = pool[(prolink_presetOffset + phraseIdx + 1) % presetCount];
    }
  }
  return preset;
  #else
  return 0;
  #endif
}

// --- Helper to print preset name ---
void printPhrasePreset(int phraseIdx, const std::vector<int>& pool) {
  int presetId = getPresetForPhraseNoRepeat(phraseIdx, pool);
  if (presetId > 0) {
    const char* name = presetCache[presetId].name;
    USER_PRINTF("Phrase %d → Preset %d (%s)\n",
      phraseIdx, presetId, name);
  } else {
    USER_PRINTF("Phrase %d → No preset\n", phraseIdx);
  }
}

int getPresetByIndex(int presetIdx, const std::vector<int>& pool) {
  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  // Guard against invalid input
  if (presetIdx <= 0) return -1;
  if (pool.empty()) return -1;

  // Check if the requested preset index is in the pool
  for (int id : pool) {
    if (id == presetIdx) {
      return id;  // Found, return the preset number
    }
  }

  // Not found
  return -1;
  #else
  return 0;
  #endif
}
  
void handleSerialInput(char next) {
  #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
  // Build a fresh pool each time
  auto pool = buildPresetPool();
  if (pool.empty()) return;

  int newPreset = -1;

  if (next >= '0' && next <= '9') {
    // Digit pressed → convert to int
    int idx = next - '0';
    newPreset = getPresetByIndex(idx, pool);

  } else if (next == '+') {
    // Next preset relative to current
    auto it = std::find(pool.begin(), pool.end(), currentPreset);
    if (it != pool.end()) {
      ++it;
      if (it == pool.end()) it = pool.begin();
      newPreset = *it;
    }

  } else if (next == '/') {
    // Toggle automatic preset switching
    prolink_presetMover = !prolink_presetMover;
    USER_PRINTF("Pro Link Preset Advance %s\n", prolink_presetMover ? "Enabled" : "Disabled");
  } else if (next == '-') {
    // Previous preset relative to current
    auto it = std::find(pool.begin(), pool.end(), currentPreset);
    if (it != pool.end()) {
      if (it == pool.begin()) it = pool.end();
      --it;
      newPreset = *it;
    }

  } else if (next == '*') {
    // Random preset that isn’t current
    if (pool.size() > 1) {
      do {
        int r = random(pool.size());
        newPreset = pool[r];
      } while (newPreset == currentPreset);
    }
  }

  // Apply if valid
  if (newPreset != -1) {
    currentPreset = newPreset;
    applyPreset(newPreset);
    handlePresets();
  }
  #endif
}
// WLEDMM v3: read the playlist repeat value for a saved preset slot.
// Returns 0 if the slot doesn't exist or the cache hasn't been built.
// 0 also means "infinite loop" for a valid playlist preset — callers
// that need to distinguish should check getCachedPresetExists(slot)
// and the isPlaylist flag first.
uint8_t getPresetRepeat(byte slot) {
  if (presetCache == nullptr || slot == 0 || slot > 250) return 0;
  if (!presetCache[slot].exists || !presetCache[slot].isPlaylist) return 0;
  return presetCache[slot].repeat;
}

// WLEDMM v3: preset navigation helpers. Single, correct
// implementation that all consumers (MIDI, Pioneer v3, future
// usermods) share instead of each rolling their own.

// Walk forward from `slot` (inclusive) to find the next existing
// preset. Wraps around. Returns 0 only if the cache has no presets
// at all (in which case the wrap-around would still find one).
byte getNextPreset(byte slot) {
  if (presetCache == nullptr) return 0;
  if (slot == 0) slot = 1;            // start at 1 if caller passed 0
  else if (slot >= 250) slot = 1;     // wrap from 250 -> 1
  else slot = slot + 1;               // step forward
  // Worst case: walk all 250 slots once.
  for (uint16_t i = 0; i < 250; i++) {
    if (presetCache[slot].exists) return slot;
    if (slot >= 250) slot = 1; else slot++;
  }
  return 0;
}

// Walk backward from `slot` (inclusive) to find the previous existing
// preset. Wraps around.
byte getPreviousPreset(byte slot) {
  if (presetCache == nullptr) return 0;
  if (slot == 0) slot = 250;
  else if (slot <= 1) slot = 250;     // wrap from 1 -> 250
  else slot = slot - 1;               // step back
  for (uint16_t i = 0; i < 250; i++) {
    if (presetCache[slot].exists) return slot;
    if (slot <= 1) slot = 250; else slot--;
  }
  return 0;
}

byte getFirstPreset() {
  if (presetCache == nullptr) return 0;
  for (byte i = 1; i <= 250; i++) {
    if (presetCache[i].exists) return i;
  }
  return 0;
}

byte getLastPreset() {
  if (presetCache == nullptr) return 0;
  for (int16_t i = 250; i >= 1; i--) {
    if (presetCache[i].exists) return (byte)i;
  }
  return 0;
}

uint16_t getPresetCount() {
  if (presetCache == nullptr) return 0;
  uint16_t n = 0;
  for (uint16_t i = 1; i <= 250; i++) {
    if (presetCache[i].exists) n++;
  }
  return n;
}

uint16_t getPlaylistPresetCount() {
  if (presetCache == nullptr) return 0;
  uint16_t n = 0;
  for (uint16_t i = 1; i <= 250; i++) {
    if (presetCache[i].exists && presetCache[i].isPlaylist) n++;
  }
  return n;
}

// WLEDMM v3: accessors for the new PresetMetadata fields. Both
// return safe defaults for missing slots or unbuilt cache, so
// callers don't have to do their own bounds checks.
const char* getPresetQL(byte slot) {
  if (presetCache == nullptr || slot == 0 || slot > 250) return "";
  if (!presetCache[slot].exists) return "";
  return presetCache[slot].ql;
}

int8_t getPresetLedmap(byte slot) {
  if (presetCache == nullptr || slot == 0 || slot > 250) return -1;
  if (!presetCache[slot].exists) return -1;
  return presetCache[slot].ledmap;
}
