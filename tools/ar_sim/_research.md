
########
* CLAIM: mode_bpm_strobe() is entirely wrapped in a ProLink-only compile guard; without USERMOD_PIONEER_PROLINK or _V3 the effect is inert. Any audio-only fallback must widen this #if, not just add a runtime branch.
  EVIDENCE: wled00/FX.cpp:657-767:
  uint16_t mode_bpm_strobe(void) {
    #if defined(USERMOD_PIONEER_PROLINK) || defined(USERMOD_PIONEER_PROLINK_V3)
      extern volatile float   prolink_bpm_public;
      extern volatile uint8_t prolink_beat_public;
      extern volatile float   prolink_beat_progress_public;
      extern volatile bool    prolink_connected_public;
      ...
    #else
      // ProLink usermod not compiled in. The effect has nothing to lock to.
      SEGMENT.fill(SEGCOLOR(1));
      return FRAMETIME;
    #endif
  }
The effect is registered unconditionally inside a PPA block: wled00/FX.cpp:14577-14584 `addEffect(FX_MODE_PRO_LINK_STROBE, &mode_bpm_strobe, _data_FX_MODE_PRO_LINK_STROBE); // not PPA yet but needs PRO_LINK`. FX_MODE_PRO_LINK_STROBE = 233 (wled00/FX.h:376).
  RELEVANCE: The consumer-side "ProLink first, estimator second" arbitration has to live inside a widened #if so the estimator path compiles and runs on a board with no ProLink usermod at all (Troy's board).
* CLAIM: The BPM value is NOT used for the strobe phase. It is used only to size the flash envelope in milliseconds; the phase comes from an absolute beat index plus a 0..1 progress within the beat.
  EVIDENCE: wled00/FX.cpp:693-698 (phase) and :711-713 (BPM):
  const uint8_t  barBeat   = prolink_beat_public;                       // 1..4, 1 = downbeat
  const float    beatProg  = prolink_beat_progress_public;
  const float    barPos    = (float)(barBeat - 1u) + beatProg;          // [0, 4)
  const float    subBarPos = barPos * ((float)flashesPerBar / 4.0f);     // [0, flashesPerBar)
  const uint16_t tickIdx   = (uint16_t)subBarPos;
  const float    withinTick= subBarPos - (float)tickIdx;                 // [0, 1)
  ...
  float    bpm            = prolink_bpm_public;
  if (bpm < 60.0f) bpm = 60.0f;
  const float subintervalMs = (60000.0f * 4.0f) / (bpm * (float)flashesPerBar);
So bpm appears exactly once in the phase path: as the divisor that turns a beat fraction into milliseconds (the 50 ms flash floor and the 25%-of-subinterval cap). A 1% BPM error changes the flash WIDTH by 1%, not the flash POSITION.
  RELEVANCE: The estimator must deliver a beat PHASE (beat index + within-beat progress), not merely a BPM number. A BPM-only estimator would force the consumer to integrate it, which is exactly what the user wants to avoid (arbitration in the consumer, estimator audio-only).
* CLAIM: The strobe RE-SYNCHRONISES per beat, so a jittery BPM does not accumulate phase error; but the within-beat progress IS an integration of the BPM and is reset only at a beat boundary.
  EVIDENCE: The effect reads an absolute beat index each frame (barBeat from prolink_beat_public, 1..4) plus a 0..1 fraction. The fraction is produced by the usermod as `progress = timeSinceBeat / beatDurationMs` (usermods/usermod_v3_pioneer_prolink/usermod_v3_pioneer_prolink.h:1732-1744, `float beatDurationMs = 60000.0f / effectiveBpm; unsigned long timeSinceBeat = millis() - lastBeatTime; float progress = timeSinceBeat / beatDurationMs;`). So the BPM's instantaneous value sets the intra-beat phase SLOPE and is re-zeroed at each beat. Maximum intra-beat phase error = (dBPM/BPM) x beatDuration.
  RELEVANCE: Answers the PLL question: a PLL is NOT needed to keep the strobe locked, because the phase is re-established at every beat. What IS needed is a beat boundary that arrives on time and is not lost. A 1-2% BPM error at 128 BPM (469 ms/beat) gives <= 9 ms of intra-beat error - about half a render frame - which is acceptable.
* CLAIM: The within-beat progress is published by the ProLink usermod, not by the effect, and its anchor differs between the v2 and v3 usermods: v2 anchors to the BEAT packet (per beat, good phase) and free-runs the BPM across a missed beat; v3 anchors to the STATUS packet (0xA6, ~200 ms) and FREEZES at 0.999 if a beat is missed.
  EVIDENCE: v3, usermods/usermod_v3_pioneer_prolink/usermod_v3_pioneer_prolink.h:1696-1718 and :1731-1747:
  if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) {
    // Note: do NOT reset lastBeatTime here. The beat packet and the status
    // packet ... arrive on independent streams ...
  ...
  if (linkState.beatInMeasure != lastBeatInMeasure && linkState.beatInMeasure > 0) {
    lastBeatTime = millis(); lastBeatInMeasure = linkState.beatInMeasure;
  }
  ...
  if (progress >= 1.0f) {
    // Don't roll over locally - let the next beat packet reset it.
    progress = 0.999f;
  }
v2, usermods/usermod_v2_pioneer_prolink/usermod_v2_pioneer_prolink.h:1529-1531 and :1558-1562:
  if (linkState.beatNumber != lastBeatNumber && linkState.beatNumber > 0) { lastBeatTime = millis(); ... }
  ...
  while (progress >= 1.0f) { progress -= 1.0f; lastBeatTime += (unsigned long)beatDurationMs; }
The FX.cpp doc block (wled00/FX.cpp:623-644) even says the status packet "can lag the actual beat moment by up to one packet interval" (~200 ms) and proposes deriving the in-bar position locally from the wrap edge of beatProgress - but that fix is NOT in the code; the effect still reads barBeat + beatProg directly (FX.cpp:693-695).
  RELEVANCE: This is the SKIPPED-BEAT failure mode. In v2 a dropped beat packet is silently swallowed by the local rollover (the phase jumps a whole beat forward - a skipped beat with no indication). In v3 a dropped beat leaves the phase pinned at 0.999, so the flash sits at the end of its tick until the next status packet. The consumer needs an explicit policy for both, and the estimator path must not inherit either failure.
* CLAIM: ProLink BPM is parsed from the BEAT packet as a 16-bit centi-BPM value scaled by the 32-bit pitch/speed ratio, giving a float; the fractional part is kept.
  EVIDENCE: usermods/usermod_v3_pioneer_prolink/usermod_v3_pioneer_prolink.h:637-649 (parseBeatPacket, packet type 0x28 on UDP port 50001):
  uint16_t baseBpmRaw = (data[0x5A] << 8) | data[0x5B];
  uint32_t speedRaw  = ((uint32_t)data[0x54] << 24) | ... | data[0x57];
  float baseBpm    = baseBpmRaw / 100.0f;
  float speedRatio = speedRaw / SPEED_UNITY;      // SPEED_UNITY = 1048576.0f  (line 42)
  linkState.bpm = baseBpm * speedRatio;
Published at :1732-1734 `prolink_bpm_public = effectiveBpm;` (float, declared :75 `volatile float prolink_bpm_public = 0.0f;`). The v2 usermod does the same at usermod_v2_pioneer_prolink.h:523-560. The '#BPM' text macro prints one decimal: wled00/FX.cpp:7475-7477 `dst += sprintf(dst, "%.1f", prolink_bpm_public);`.
  RELEVANCE: The published tempo should be a float BPM with a fractional part too, so the two sources are the same shape and the consumer can pick one without conversion. The consumer only needs ~1-2% accuracy, and the estimator's error should be compared on the same scale.
* CLAIM: The effect's validity gate is a triple test (connected, BPM > 0, beat-in-measure != 0) and on failure it blanks the segment and resets its tracking state.
  EVIDENCE: wled00/FX.cpp:670-675:
  if (!prolink_connected_public || prolink_bpm_public <= 0.0f || prolink_beat_public == 0u) {
    SEGMENT.fill(SEGCOLOR(1));   // off-color
    SEGENV.aux0 = 0;             // reset beat tracking so the
    SEGENV.aux1 = 0;             // counter restarts cleanly on reconnect
    return FRAMETIME;
  }
prolink_connected_public is set from the link state at v3 usermod line 1754 `prolink_connected_public = linkState.isMaster;` and cleared at :1619.
  RELEVANCE: The estimator branch needs the same three-part validity test (a valid flag, BPM > 0, and a beat index that is not the "no data" sentinel), otherwise the effect strobes to a fabricated tempo during a gap in the music. The blank-segment-on-no-data behaviour is intentional and must be preserved.
* CLAIM: The consumer render rate sets the phase resolution: the effect returns FRAMETIME, so it draws once per strip frame (42 fps default = 23.8 ms; 60 fps with WLEDMM_FASTPATH = 16.7 ms; MIN_SHOW_DELAY permits up to 250 fps).
  EVIDENCE: wled00/FX.cpp:760 `return FRAMETIME;` with `#define FRAMETIME strip.getFrameTime()` (wled00/FX.h:49, :61) and `inline uint16_t getFrameTime(void) const { return _frametime; }` (FX.h:1039). Defaults: wled00/FX.h:57-59 `#define WLED_FPS 42` / `#define WLED_FPS_SLOW 42`, `FX.h:909 _targetFps(WLED_FPS_SLOW)`. WLEDMM_FASTPATH path: FX.h:48-55 (`WLED_FPS 120`, `WLED_FPS_SLOW 60`, MIN_SHOW_DELAY comment "support higher framerates (up to 250fps)"). The flash floor is fixed at 50 ms: FX.cpp:714 `const float minFlashMs = 50.0f;  // ~3 frames at 30 fps`.
  RELEVANCE: The beat-phase accuracy the estimator must hit is one render frame, i.e. 17-24 ms, not the 10.67 ms analysis frame. The estimator can publish its beat grid at a slower rate than the render rate as long as the consumer can compute the phase from millis() and the grid's beat times.
* CLAIM: The downbeat colour needs a BAR phase (beat 1 of 4) that ProLink supplies for free but an audio-only estimator cannot know: tickIdx == 0 means barBeat == 1.
  EVIDENCE: wled00/FX.cpp:743:
  } else if ((downbeatColor && tickIdx == 0 && subBarPos < decayDur) || (downbeatColor && tickIdx + 1 == flashesPerBar && withinTick >= 1.0f - attackDur)) {
    peakColor = SEGCOLOR(2);   // downbeat
The bar phase comes from prolink_beat_public (beat-in-measure 1..4), which the CDJ sends in the STATUS packet at offset 0xA6: v3 usermod :585-597 `uint8_t newBeat = data[0xA6]; ... prolink_beat_public = newBeat;`.
  RELEVANCE: The estimator path has no downbeat. The consumer must decide: declare beat 1 = the first beat after the grid starts or after a track change, drop the downbeat colour on the estimator path, or keep a local beat counter mod 4. The '#BEAT' text macro (FX.cpp:7463-7466) and the effect's colour rule both depend on this.
* CLAIM: The UDP sound sync packet is a packed 44-byte struct whose version is an ASCII header string, and the transmit path explicitly refuses to change it.
  EVIDENCE: usermods/audioreactive/audio_reactive.h:5824-5836:
  // new "V2" audiosync struct - 44 Bytes
  struct __attribute__ ((packed)) audioSyncPacket {
    char    header[6];      //  06 Bytes  offset 0 - "00002" for protocol version 2
    uint8_t pressure[2];    //  02 Bytes, offset 6
    float   sampleRaw;      //  04 Bytes  offset 8
    float   sampleSmth;     //  04 Bytes  offset 12
    uint8_t samplePeak;     //  01 Bytes  offset 16
    uint8_t frameCounter;   //  01 Bytes  offset 17
    uint8_t bandBytes[16];  //  16 Bytes  offset 18 - 16 GEQ channels
    uint16_t zeroCrossingCount; // 02 Bytes, offset 34
    float  AR_Magnitude;    //  04 Bytes  offset 36
    float  AR_MajorPeak;    //  04 Bytes  offset 40
  };
  static const char UDP_SYNC_HEADER[] PROGMEM = "00002";      // line 9542
  static const char UDP_SYNC_HEADER_v1[] PROGMEM = "00001";   // line 9543
  #define UDPSOUND_MAX_PACKET 96                             // line 5851
The layout is printed by the self-test at :7055-7066 (44 bytes; bandBytes at offset 18). The transmit path states the freeze explicitly, audio_reactive.h:6366-6371:
  // The packet is the v2 format and stays the v2 format: 16 bands, 44
  // bytes, byte-identical to what every other WLED instance expects. The
  // 31 RTA bands are NOT in here ... and adding them to this packet is what
  // broke the other instances.
Receive side: decodeAudioData() at :6414-6478, version check isValidUdpSyncVersion() at :6407-6409 (header == "00002"), and the 16 bands
  RELEVANCE: Out of scope for this build, but the location is: a BPM field would go at offset 44 in a new struct version (new header string, e.g. "00003"), with the receiver gating on the header. Adding a field to the existing struct in place is exactly what the comment says broke other WLED instances.
* CLAIM: A board in UDP sound-sync receive mode runs NO local audio analysis, so it can never produce an estimator tempo; the AR usermod hard-disables sound processing in that mode.
  EVIDENCE: usermods/audioreactive/audio_reactive.h:7110-7111 `if (audioSyncEnabled == AUDIOSYNC_REC) useNetworkAudio = true;  // don't fall back to local audio in standard "receive mode"`; :7140 `if (audioSyncEnabled == AUDIOSYNC_REC) disableSoundProcessing = true;   // make sure everything is disabled IF in audio Receive mode`; :7147 `if ((audioSyncEnabled == AUDIOSYNC_REC_PLUS) && useNetworkAudio) disableSoundProcessing = true;`. The consequence is published at :7724-7729 `r["usable"] = arRtaUsable();` with the reason: "the UDP sound sync packets carry the sixteen display bands and nothing else".
  RELEVANCE: On a receiving board the BPM strobe will have no tempo source at all unless the UDP packet is extended. That is out of scope for this build, but the consumer's arbitration must not silently fall back to a stale/default tempo on a receiver.
* CLAIM: There is NO reusable tempo, autocorrelation, PLL or beat-tracker in WLED today. The only beat-ish code is a peak detector and the FastLED beatsin family, which is a fixed-BPM phase integrator, not an estimator.
  EVIDENCE: Repo-wide search for pll / autocorr / tempo / onset / beat found nothing usable. What exists: (a) detectSamplePeak() in usermods/audioreactive/audio_reactive.h:5527-5547 - "Poor man's beat detection by seeing if sample > Average + some value", gated on a single bin and a 100 ms timeout; (b) zeroCrossingCount, audio_reactive.h:5232-5255 - counted over ONE 512-sample hop and documented as "an INDICATOR of a change, not a measurement of anything", read only by the auto-playlist usermod; (c) the FastLED beatsin family, wled00/util.cpp:763-795 (beatsin88_t/beatsin16_t/beatsin8_t) over beat88() in .pio/libdeps/esp32p4_8MB_troyhacks/FastLED_no_fastled/src/lib8tion.h:1025-1054:
  LIB8STATIC uint16_t beat88(accum88 beats_per_minute_88, uint32_t timebase = 0) {
      return (((GET_MILLIS()) - timebase) * beats_per_minute_88 * 280) >> 16;
  }
That is a phase integrator: it multiplies the elapsed time by the BPM, so a BPM change alters the phase RATE and a jittery BPM makes the phase wander - exactly the behaviour the strobe must avoid. The ProLink usermods are the only beat source (v3 :1696-1747).
  RELEVANCE: The tempo estimator has to be built; there is no WLED code to lift. But beat88()/beatsin*() IS the ready-made free-running phase integrator for the CONSUMER side if the consumer ever has to synthesise a phase from a bare BPM - and it must not be fed a jittery BPM.
* CLAIM: The /json/ar endpoint is a usermod virtual (addToJsonAr) served by its own URL branch, and the AudioReactive one is nested by what the number IS: level / peak / display / geq / rta / bank / rtatopbank / timing_us / init_ms / self_test / calibration / codec / memory / udp_sync.
  EVIDENCE: wled00/json.cpp:1818-1841 routes "/ar": `usermods.addToJsonAr(root);` (the virtual is declared at wled00/fcn_declare.h:400 `virtual void addToJsonAr(JsonObject& obj) {}` and :438). The AudioReactive implementation is usermods/audioreactive/audio_reactive.h:7491-8042, opening with:
  void addToJsonAr(JsonObject& obj) override {
    JsonObject ar = obj.createNestedObject(FPSTR(_name));
    const int rtaBands = bandLayoutReady ? lay31.count : 0;
    ArFrameSnapshot snap; { ArPublishGuard guard; snap = arFrameSnap; }
    const ArLiveSettings st = arSettingsSnapshot();
The doc block at :7474-7490 states the rule: "EVERY number the analysis knows, as a NUMBER, with the unit in the key's name ... There are no strings anywhere in here except the few names that have no numeric form". The endpoint's own description is in usermods/audioreactive/readme.md:170-176.
  RELEVANCE: A tempo belongs in /json/ar as a nested object (e.g. "tempo": {bpm, beat, beat_phase, confidence}) with the unit in the key name, added as one more block in the same style, not as loose scalars. The values must come from the published snapshot under ArPublishGuard so they are one frame's numbers.
* CLAIM: Info-page rows are added in addToJsonInfo as a nested array per row under root["u"], and the convention is: one short string per row, a row only when it says something (success rows are omitted, failure rows are not).
  EVIDENCE: usermods/audioreactive/audio_reactive.h:8049-8074:
  void addToJsonInfo(JsonObject& root) {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray infoArr = user.createNestedArray(FPSTR(_name));
    ...
  and the row idiom, e.g. :8190-8192:
    infoArr = user.createNestedArray(F("Level (dB)"));
    snprintf(lvlBuf, sizeof(lvlBuf), "%.1f", (double)arLevelDb);
    infoArr.add(lvlBuf);
The rule is stated at :8091-8095: "Every row in this pane is ONE short string. The pane renders a row's first two elements back to back with no separator". Failure-only rows are the pattern (:8170-8179 chain self-test, :8280-8295 RTA bank self-test); the machine detail was deliberately moved to /json/ar (:8149-8153, :8264-8268).
  RELEVANCE: A tempo row belongs here as one short string (e.g. "128.4 BPM, beat 2/4"), following the existing idiom exactly - and per the memory note the usermod's own settings page is not where a live readout belongs.
* CLAIM: Single-character serial commands are dispatched in TWO places that must be kept in step: the else-if chain in wled_serial.cpp (which routes letters into the usermod) and the switch in audioReactiveRtaCommand() (which implements them).
  EVIDENCE: wled00/wled_serial.cpp:322-334:
  } else if (next == 'b' || next == 'B' || next == 'u' || next == 'U' ||
             next == 's' || next == 'S' ||
             next == 'o' || next == 'O' || next == 'n' || next == 'p' ||
             next == 'y' || next == 'Y' || next == 'k' || next == 'K' ||
             next == 'w' || next == 'W' || next == 'c' || next == 'C' ||
             next == 'z' || next == 'Z') {
    ...
    audioReactiveRtaCommand(next);
The key list is documented as a comment at :263-290 and in usermods/audioreactive/readme.md. The usermod entry point is usermods/audioreactive/audio_reactive.h:2837 `void audioReactiveRtaCommand(char c) {` with cases at :2842 ('b'/'B' floor), :2854 ('u'/'U' top), :2866 ('s'/'S' span), :2894 ('o'/'O' reference), :2904 ('n'/'p' band), :2914 ('y'/'Y' trim dB), :2924 ('k'/'K' detector), :2961 ('w'/'W' release), :2980 ('c'/'C' codec gain), :3011 ('z'/'Z' pattern), default at :3033. The prototypes are declared at wled00/wled.h:597-602.
  RELEVANCE: A new tempo key needs an entry in BOTH the wled_serial.cpp letter list and the audioReactiveRtaCommand switch, plus the readme table. The established convention is lower = lower, upper = higher, one letter per direction, and no multi-character commands.
* CLAIM: Free serial letters are scarce and the upper-case half is already crowded. Entirely unused pairs: e/E, h/H, i/I, j/J, q/Q. Half-free: a (A used), l (L free), m (M free; 'm' is taken only on P4+HDMI builds), n (N free), p (P free), r (R used), v (V free), x (X free), c (C taken).
  EVIDENCE: Full else-if chain in wled00/wled_serial.cpp:135-386 claims: 'v'(170) 'F'(172) 'f'(176) '^'(178) '0'-'9','+','-','*','/'(202) 'X'(206) 'C'(209) 'T'(211) 'R'(213) 'G'(223) 'd','D'(249) 't'(291) 'g'(297) 'A'(302) 'b','B','u','U','s','S','o','O','n','p','y','Y','k','K','w','W','c','C','z','Z'(322-327) 0xB0-0xB7(336-350) 'l'(352) '{'(354). Plus 'm' at :146 on CONFIG_IDF_TARGET_ESP32P4 && WLEDMM_DISPLAY_MODE (the HDMI menu). ProLink's own serial input takes digits and + - * / (:202-205).
  RELEVANCE: Picks the tempo tuning key: a single free letter pair (e/E, h/H, i/I, j/J or q/Q) with lower = lower / upper = higher, consistent with every other pair in the usermod.
* CLAIM: The publish module (wled00/ar_publish.{h,cpp}) currently publishes only the 31 RTA band bytes and a liveness flag; there is no slot for a tempo, and its API is deliberately narrow.
  EVIDENCE: wled00/ar_publish.h:47-88 exposes exactly: arPublishRtaBands(), arCopyRtaBands(), arRtaUsable(), arSetRtaLive(), arCopyRtaBandsLocked(), arResetRtaBands(), and the class ArPublishGuard (:39-45, "The lock now lives here, where every writer and every reader can reach it"). ar_publish.cpp:14-24 implements the single spin lock (std::atomic<uint8_t>, exchange/spin) and the double buffer arRtaBuf[2][NUM_RTA_CHANNELS] with an index swap (:25-41). The usermod's own 16-band publish is separate and local (arPublishBands(), audio_reactive.h:3123-3201) and the frame snapshot copy is arPublishFrameSnapshot() (:3457-3462) via arCopyRtaBandsLocked().
  RELEVANCE: A tempo would be published the same way: a new arPublishTempo()/arCopyTempo() pair under the same ArPublishGuard, so the effect, the /json/ar endpoint and the info page all read one coherent value set. ar_publish.h:36-38 warns the lock is NOT recursive.
-- pitfalls: [{"consequence": "The uppercase codec-gain key silently does nothing: pressing 'C' runs task_list() instead of raising the codec input gain by 0.5 dB. Any new key whose uppercase half lands on 'C' would be dead too.", "mitigation": "Pick a pair whose uppercase half is genuinely free (e/E, h/H, i/I, j/J, q/Q) - and if 'C' is wanted, move the task_list key first. Check the wled_serial.cpp else-if ORDER, not just the letter.", "pitfall": "Uppercase 'C' is claimed by WLEDMM's task-list display at wled_serial.cpp:209, which is evaluated BEFORE the AudioReactive block at :326. The readme (usermods/audioreactive/readme.md) and the code comment at :279 both list 'c'/'C' as the codec-gain pair."}, {"consequence": "A tempo published at 10.67 ms that is jittery will make the flash WIDTH jitter and the intra-beat phase wander, even though the strobe re-locks every beat. If the estimator's BPM moves by 5% frame to frame, the flash window breathes visibly.", "mitigation": "Publish a smoothed/median BPM alongside the raw estimate, or let the consumer smooth it; and make the estimator's BEAT TIMES (a grid) the primary product, with the BPM derived from the grid's own period, so a stable value is what the consumer integrates.", "pitfall": "The ProLink BPM is published raw and unfiltered (v3 usermod :1732-1734 `prolink_bpm_public = effectiveBpm;`) with no smoothing, and it is used inside a per-beat integration."}, {"consequence": "Comment/code divergence: a future reader will believe the wrap-edge fix is in place and will not account for the ~200 ms status-packet lag when reasoning about the estimator's required phase accuracy. The real phase error of the ProLink path is up to 200 ms, i.e. 43% of a beat at 128 BPM.", "mitigation": "Treat the ProLink path's measured phase lag as the baseline the estimator must beat, and fix or annotate the stale comment before relying on it. The estimator can be MORE accurate than ProLink here because it knows the beat times exactly.", "pitfall": "wled00/FX.cpp:623-644 documents a beat-anchor architecture (derive the in-bar position locally from the wrap edge of beatProgress, SEGENV.aux0 = previous beatProgress in Q16.16, aux1 = monotonic beat counter) that the code does NOT implement - the effect still reads barBeat + beatProg directly, and SEGENV.aux0/aux1 are only ever zeroed at :672-673."}, {"consequence": "A wrong-but-plausible tempo makes the strobe flash visibly out of sync with the music - worse than no strobe at all, because it looks alive. A half/double tempo also changes the flash width by 2x (subintervalMs = 240000/(bpm*flashesPerBar)), which changes the envelope shape, not just its timing.", "mitigation": "Require a confidence value and a minimum observation window before the consumer switches to the estimator, and keep the ProLink-first rule. Publish the confidence in /json/ar so the arbitration can be inspected. Consider a 'lock/unlock' serial key to force the estimator when ProLink is absent.", "pitfall": "The estimator's tempo will be wrong sometimes (half/double time, 128 vs 130). The consumer currently trusts prolink_bpm_public blindly, and the effect is a STROBE - a wrong tempo is far more objectionable than no tempo."}, {"consequence": "On a receiver the effect would either strobe to a stale tempo or go dark. If the AR usermod's local analysis is suspended, the estimator publishes nothing, so a naive fallback that uses \"the last published BPM\" would strobe to a stale value indefinitely.", "mitigation": "Gate the estimator branch on the estimator's own validity (not on the last value seen), and treat a non-live analysis as 'no tempo'. ar_publish's arSetRtaLive()/arRtaUsable() (ar_publish.h:63-79) is the existing precedent for a liveness flag.", "pitfall": "A receiving board has no local analysis at all (audio_reactive.h:7140), so the estimator branch can only ever run on the board that owns the microphone."}, {"consequence": "Adding a BPM field to the existing struct in place breaks every o

