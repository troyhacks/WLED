#include "wled.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

#ifdef USERMOD_ARTNETMAP
#include "../usermods/usermod_v2_artnetmap/usermod_v2_artnetmap.h"
#endif

/*
 * UDP sync notifier / Realtime / Hyperion / TPM2.NET
 */

#define UDP_SEG_SIZE 36
#define SEG_OFFSET (41+(MAX_NUM_SEGMENTS*UDP_SEG_SIZE))
#define WLEDPACKETSIZE (41+(MAX_NUM_SEGMENTS*UDP_SEG_SIZE)+0)
#define UDP_IN_MAXSIZE 1472
#define PRESUMED_NETWORK_DELAY 3 //how many ms could it take on avg to reach the receiver? This will be added to transmitted times

void notify(byte callMode, bool followUp)
{
  if (!udpConnected) return;
  if (!syncGroups) return;
  switch (callMode)
  {
    case CALL_MODE_INIT:          return;
    case CALL_MODE_DIRECT_CHANGE: if (!notifyDirect) return; break;
    case CALL_MODE_BUTTON:        if (!notifyButton) return; break;
    case CALL_MODE_BUTTON_PRESET: if (!notifyButton) return; break;
    case CALL_MODE_NIGHTLIGHT:    if (!notifyDirect) return; break;
    case CALL_MODE_HUE:           if (!notifyHue)    return; break;
    case CALL_MODE_PRESET_CYCLE:  if (!notifyDirect) return; break;
    case CALL_MODE_ALEXA:         if (!notifyAlexa)  return; break;
    default: return;
  }
  byte udpOut[WLEDPACKETSIZE];
  Segment& mainseg = strip.getMainSegment();
  udpOut[0] = 0; //0: wled notifier protocol 1: WARLS protocol
  udpOut[1] = callMode;
  udpOut[2] = bri;
  uint32_t col = mainseg.colors[0];
  udpOut[3] = R(col);
  udpOut[4] = G(col);
  udpOut[5] = B(col);
  udpOut[6] = nightlightActive;
  udpOut[7] = nightlightDelayMins;
  udpOut[8] = mainseg.mode;
  udpOut[9] = mainseg.speed;
  udpOut[10] = W(col);
  //compatibilityVersionByte:
  //0: old 1: supports white 2: supports secondary color
  //3: supports FX intensity, 24 byte packet 4: supports transitionDelay 5: sup palette
  //6: supports timebase syncing, 29 byte packet 7: supports tertiary color 8: supports sys time sync, 36 byte packet
  //9: supports sync groups, 37 byte packet 10: supports CCT, 39 byte packet 11: per segment options, variable packet length (40+MAX_NUM_SEGMENTS*3)
  //12: enhanced effect sliders, 2D & mapping options
  udpOut[11] = 12;
  col = mainseg.colors[1];
  udpOut[12] = R(col);
  udpOut[13] = G(col);
  udpOut[14] = B(col);
  udpOut[15] = W(col);
  udpOut[16] = mainseg.intensity;
  udpOut[17] = (transitionDelay >> 0) & 0xFF;
  udpOut[18] = (transitionDelay >> 8) & 0xFF;
  udpOut[19] = mainseg.palette;
  col = mainseg.colors[2];
  udpOut[20] = R(col);
  udpOut[21] = G(col);
  udpOut[22] = B(col);
  udpOut[23] = W(col);

  udpOut[24] = followUp;
  uint32_t t = millis() + strip.timebase;
  udpOut[25] = (t >> 24) & 0xFF;
  udpOut[26] = (t >> 16) & 0xFF;
  udpOut[27] = (t >>  8) & 0xFF;
  udpOut[28] = (t >>  0) & 0xFF;

  //sync system time
  udpOut[29] = toki.getTimeSource();
  Toki::Time tm = toki.getTime();
  uint32_t unix = tm.sec;
  udpOut[30] = (unix >> 24) & 0xFF;
  udpOut[31] = (unix >> 16) & 0xFF;
  udpOut[32] = (unix >>  8) & 0xFF;
  udpOut[33] = (unix >>  0) & 0xFF;
  uint16_t ms = tm.ms;
  udpOut[34] = (ms >> 8) & 0xFF;
  udpOut[35] = (ms >> 0) & 0xFF;

  //sync groups
  udpOut[36] = syncGroups;

  //Might be changed to Kelvin in the future, receiver code should handle that case
  //0: byte 38 contains 0-255 value, 255: no valid CCT, 1-254: Kelvin value MSB
  udpOut[37] = strip.hasCCTBus() ? 0 : 255; //check this is 0 for the next value to be significant
  udpOut[38] = mainseg.cct;

  udpOut[39] = strip.getActiveSegmentsNum();
  udpOut[40] = UDP_SEG_SIZE; //size of each loop iteration (one segment)
  size_t s = 0, nsegs = strip.getSegmentsNum();
  for (size_t i = 0; i < nsegs; i++) {
    Segment &selseg = strip.getSegment(i);
    if (!selseg.isActive()) continue;
    uint16_t ofs = 41 + s*UDP_SEG_SIZE; //start of segment offset byte
    udpOut[0 +ofs] = s;
    udpOut[1 +ofs] = selseg.start >> 8;
    udpOut[2 +ofs] = selseg.start & 0xFF;
    udpOut[3 +ofs] = selseg.stop >> 8;
    udpOut[4 +ofs] = selseg.stop & 0xFF;
    udpOut[5 +ofs] = selseg.grouping;
    udpOut[6 +ofs] = selseg.spacing;
    udpOut[7 +ofs] = selseg.offset >> 8;
    udpOut[8 +ofs] = selseg.offset & 0xFF;
    udpOut[9 +ofs] = selseg.options & 0x8F; //only take into account selected, mirrored, on, reversed, reverse_y (for 2D); ignore freeze, reset, transitional
    udpOut[10+ofs] = selseg.opacity;
    udpOut[11+ofs] = selseg.mode;
    udpOut[12+ofs] = selseg.speed;
    udpOut[13+ofs] = selseg.intensity;
    udpOut[14+ofs] = selseg.palette;
    udpOut[15+ofs] = R(selseg.colors[0]);
    udpOut[16+ofs] = G(selseg.colors[0]);
    udpOut[17+ofs] = B(selseg.colors[0]);
    udpOut[18+ofs] = W(selseg.colors[0]);
    udpOut[19+ofs] = R(selseg.colors[1]);
    udpOut[20+ofs] = G(selseg.colors[1]);
    udpOut[21+ofs] = B(selseg.colors[1]);
    udpOut[22+ofs] = W(selseg.colors[1]);
    udpOut[23+ofs] = R(selseg.colors[2]);
    udpOut[24+ofs] = G(selseg.colors[2]);
    udpOut[25+ofs] = B(selseg.colors[2]);
    udpOut[26+ofs] = W(selseg.colors[2]);
    udpOut[27+ofs] = selseg.cct;
    udpOut[28+ofs] = (selseg.options>>8) & 0xFF; //mirror_y, transpose, 2D mapping & sound
    udpOut[29+ofs] = selseg.custom1;
    udpOut[30+ofs] = selseg.custom2;
    udpOut[31+ofs] = selseg.custom3 | (selseg.check1<<5) | (selseg.check2<<6) | (selseg.check3<<7);
    udpOut[32+ofs] = selseg.startY >> 8;
    udpOut[33+ofs] = selseg.startY & 0xFF;
    udpOut[34+ofs] = selseg.stopY >> 8;
    udpOut[35+ofs] = selseg.stopY & 0xFF;
    ++s;
  }

  //uint16_t offs = SEG_OFFSET;
  //next value to be added has index: udpOut[offs + 0]

  IPAddress broadcastIp;
  broadcastIp = ~uint32_t(Network.subnetMask()) | uint32_t(Network.gatewayIP());

  if (0 != notifierUdp.beginPacket(broadcastIp, udpPort)) { // WLEDMM beginPacket == 0 --> error
    notifierUdp.write(udpOut, WLEDPACKETSIZE);
    notifierUdp.endPacket();
  }
  notificationSentCallMode = callMode;
  notificationSentTime = millis();
  notificationCount = followUp ? notificationCount + 1 : 0;
}

void realtimeLock(uint32_t timeoutMs, byte md)
{
  if (!realtimeMode && !realtimeOverride) {
    // this code runs once when we enter realtime mode
    // WLEDMM begin - we need to init segment caches before putting any pixels
    USER_PRINT(F("realtimeLock() entering realtime mode [timeoutMs="));
    USER_PRINT(timeoutMs); USER_PRINT(",mode="); USER_PRINT(md);
    if (useMainSegmentOnly) { USER_PRINTLN(F(", main segment only].")); } else { USER_PRINTLN(F("]."));}
    USER_FLUSH();

    // if (strip.isServicing()) {
    //   USER_PRINTLN(F("realtimeLock() entering RTM: strip is still drawing effects."));
    //   strip.waitUntilIdle();
    // }
    strip.service(); // WLEDMM make sure that all segments are properly initialized
    busses.invalidateCache(true);
    // WLEDMM end

    uint16_t stop, start;
    if (useMainSegmentOnly) {
      Segment& mainseg = strip.getMainSegment();
      start = mainseg.start;
      stop  = mainseg.stop;
      mainseg.map1D2D = M12_Pixels; // WLEDMM no mapping
      mainseg.freeze = true;
    } else {
      start = 0;
      stop  = strip.getLengthTotal();
    }
    // clear strip/segment
    for (size_t i = start; i < stop; i++) strip.setPixelColor(i,BLACK);
    // if WLED was off and using main segment only, freeze non-main segments so they stay off
    if (useMainSegmentOnly && bri == 0) {
      for (size_t s=0; s < strip.getSegmentsNum(); s++) {
        strip.getSegment(s).freeze = true;
      }
    }
  }
  // if strip is off (bri==0) and not already in RTM
  if (briT == 0 && !realtimeMode && !realtimeOverride) {
    strip.setBrightness(scaledBri(briLast), true);
  }

  if (realtimeTimeout != UINT32_MAX) {
    realtimeTimeout = (timeoutMs == 255001 || timeoutMs == 65000) ? UINT32_MAX : millis() + timeoutMs;
  }
  realtimeMode = md;

  if (realtimeOverride) return;
  if (arlsForceMaxBri) strip.setBrightness(scaledBri(255), true);
  if (briT > 0 && md == REALTIME_MODE_GENERIC) strip.show();

  if (realtimeMode && !realtimeOverride && useMainSegmentOnly) strip.getMainSegment().startFrame(); // WLEDMM make sure the main segment is ready for drawing
}

void exitRealtime() {
  if (!realtimeMode) return;
  if (realtimeOverride == REALTIME_OVERRIDE_ONCE) realtimeOverride = REALTIME_OVERRIDE_NONE;
  strip.setBrightness(scaledBri(bri), true);
  realtimeTimeout = 0; // cancel realtime mode immediately
  realtimeMode = REALTIME_MODE_INACTIVE; // inform UI immediately
  realtimeIP[0] = 0;
  if (useMainSegmentOnly) { // unfreeze live segment again
    strip.getMainSegment().freeze = false;
  } else {
    strip.show(); // possible fix for #3589
  }
  busses.invalidateCache(false);  // WLEDMM
  USER_PRINTLN(F("exitRealtime() realtime mode ended."));
  updateInterfaces(CALL_MODE_WS_SEND);
}


#define TMP2NET_OUT_PORT 65442

void sendTPM2Ack() {
  if (0 != notifierUdp.beginPacket(notifierUdp.remoteIP(), TMP2NET_OUT_PORT)) {  // WLEDMM beginPacket == 0 --> error
    uint8_t response_ack = 0xac;
    notifierUdp.write(&response_ack, 1);
    notifierUdp.endPacket();
  }
}

#ifdef ARDUINO_ARCH_ESP32
// WLEDMM don't use dynamic arrays for receiving UDP. ESP32 has enough RAM, and handleNotifications() is only called from main loop, so one static buffer should be enough.
static uint8_t lbuf[UDP_IN_MAXSIZE+1];
static uint8_t udpIn[UDP_IN_MAXSIZE+1];
// WLEDMM end
#endif

void handleNotifications() {

  IPAddress localIP;

  //send second notification if enabled
  if(udpConnected && (notificationCount < udpNumRetries) && ((millis()-notificationSentTime) > 250)){
    notify(notificationSentCallMode,true);
  }

  if (e131NewData && millis() - strip.getLastShow() > 15)
  {
    e131NewData = false;
    strip.show();
  }

  //unlock strip when realtime UDP times out
  if (realtimeMode && millis() > realtimeTimeout) exitRealtime();

  //receive UDP notifications
  if (!udpConnected) return;

  bool isSupp = false;
#ifdef ARDUINO_ARCH_ESP32
  notifierUdp.flush();
#endif
  int packetSize = notifierUdp.parsePacket();    // WLEDMM function returns int, not size_t
  if ((packetSize < 1) && udp2Connected) {
#ifdef ARDUINO_ARCH_ESP32
    notifier2Udp.flush();
#endif
    packetSize = notifier2Udp.parsePacket();
    isSupp = true;
  }
  if (packetSize < 1) packetSize = 0; // WLEDMM

  //hyperion / raw RGB
  if (!packetSize && udpRgbConnected) {
    rgbUdp.flush();
    packetSize = rgbUdp.parsePacket();
    if (packetSize) {
      if (!receiveDirect) {rgbUdp.flush(); notifierUdp.flush(); notifier2Udp.flush(); return;}
      if (packetSize > UDP_IN_MAXSIZE || packetSize < 3) {rgbUdp.flush(); notifierUdp.flush(); notifier2Udp.flush(); return;}
      realtimeIP = rgbUdp.remoteIP();
      DEBUG_PRINTLN(rgbUdp.remoteIP());
      #ifndef ARDUINO_ARCH_ESP32
      uint8_t lbuf[packetSize+1]; // WLEDMM: use global buffer on ESP32
      #endif
      rgbUdp.read(lbuf, packetSize);
      realtimeLock(realtimeTimeoutMs, REALTIME_MODE_GENERIC);
      if (realtimeOverride && !(realtimeMode && useMainSegmentOnly)) {notifierUdp.flush(); notifier2Udp.flush(); return;}
      uint16_t id = 0;
      uint16_t totalLen = strip.getLengthTotal();
      for (int i = 0; i < packetSize -2; i += 3)
      {
        setRealtimePixel(id, lbuf[i], lbuf[i+1], lbuf[i+2], 0);
        id++; if (id >= totalLen) break;
      }
      if (!(realtimeMode && useMainSegmentOnly)) strip.show();
      return;
    }
  }

  if (!(receiveNotifications || receiveDirect)) {notifierUdp.flush(); notifier2Udp.flush(); return;}

  localIP = Network.localIP();
  //notifier and UDP realtime

  if (!packetSize || packetSize > UDP_IN_MAXSIZE) {notifierUdp.flush(); notifier2Udp.flush(); return;}
  if (!isSupp && notifierUdp.remoteIP() == localIP) {notifierUdp.flush(); notifier2Udp.flush(); return;} //don't process broadcasts we send ourselves

  uint16_t len;
  if (isSupp) len = notifier2Udp.read(udpIn, packetSize);
  else        len =  notifierUdp.read(udpIn, packetSize);

  // WLED nodes info notifications
  if (isSupp && udpIn[0] == 255 && udpIn[1] == 1 && len >= 40) {
    if (!nodeListEnabled || notifier2Udp.remoteIP() == localIP) return;

    uint8_t unit = udpIn[39];
    NodesMap::iterator it = Nodes.find(unit);
    if (it == Nodes.end() && Nodes.size() < WLED_MAX_NODES) { // Create a new element when not present
      Nodes[unit].age = 0;
      it = Nodes.find(unit);
    }

    if (it != Nodes.end()) {
      for (size_t x = 0; x < 4; x++) {
        it->second.ip[x] = udpIn[x + 2];
      }
      it->second.age = 0; // reset 'age counter'
      char tmpNodeName[33] = { 0 };
      memcpy(&tmpNodeName[0], reinterpret_cast<byte *>(&udpIn[6]), 32);
      tmpNodeName[32]     = 0;
      it->second.nodeName = tmpNodeName;
      it->second.nodeName.trim();
      it->second.nodeType = udpIn[38];
      uint32_t build = 0;
      if (len >= 44)
        for (size_t i=0; i<sizeof(uint32_t); i++)
          build |= udpIn[40+i]<<(8*i);
      it->second.build = build;
    }
    return;
  }

  //wled notifier, ignore if realtime packets active
  if (udpIn[0] == 0 && !realtimeMode && receiveNotifications) {

    //ignore notification if received within a second after sending a notification ourselves
    if (millis() - notificationSentTime < 1000) return;
    if (udpIn[1] > 199) return; //do not receive custom versions

    //compatibilityVersionByte:
    byte version = udpIn[11];

    // if we are not part of any sync group ignore message
    if (version < 9 || version > 199) {
      // legacy senders are treated as if sending in sync group 1 only
      if (!(receiveGroups & 0x01)) return;
    } else if (!(receiveGroups & udpIn[36])) return;

    bool someSel = (receiveNotificationBrightness || receiveNotificationColor || receiveNotificationEffects);

    //apply colors from notification to main segment, only if not syncing full segments
    if ((receiveNotificationColor || !someSel) && (version < 11 || !receiveSegmentOptions)) {
      // primary color, only apply white if intended (version > 0)
      strip.setColor(0, RGBW32(udpIn[3], udpIn[4], udpIn[5], (version > 0) ? udpIn[10] : 0));
      if (version > 1) {
        strip.setColor(1, RGBW32(udpIn[12], udpIn[13], udpIn[14], udpIn[15])); // secondary color
      }
      if (version > 6) {
        strip.setColor(2, RGBW32(udpIn[20], udpIn[21], udpIn[22], udpIn[23])); // tertiary color
        if (version > 9 && version < 200 && udpIn[37] < 255) { // valid CCT/Kelvin value
          uint16_t cct = udpIn[38];
          if (udpIn[37] > 0) { //Kelvin
            cct |= (udpIn[37] << 8);
          }
          strip.setCCT(cct);
        }
      }
    }

    bool timebaseUpdated = false;
    //apply effects from notification
    bool applyEffects = (receiveNotificationEffects || !someSel);
    if (version < 200)
    {
      if (applyEffects && currentPlaylist >= 0) unloadPlaylist();
      if (version > 10 && (receiveSegmentOptions || receiveSegmentBounds)) {
        uint8_t numSrcSegs = udpIn[39];
        for (size_t i = 0; i < numSrcSegs; i++) {
          uint16_t ofs = 41 + i*udpIn[40]; //start of segment offset byte
          uint8_t id = udpIn[0 +ofs];
          if (id > strip.getSegmentsNum()) break;

          Segment& selseg = strip.getSegment(id);
          if (!selseg.isActive() || !selseg.isSelected()) continue; //do not apply to non selected segments

          uint16_t startY = 0, start  = (udpIn[1+ofs] << 8 | udpIn[2+ofs]);
          uint16_t stopY  = 1, stop   = (udpIn[3+ofs] << 8 | udpIn[4+ofs]);
          uint16_t offset = (udpIn[7+ofs] << 8 | udpIn[8+ofs]);
          if (!receiveSegmentOptions) {
            selseg.setUp(start, stop, selseg.grouping, selseg.spacing, offset, startY, stopY);
            continue;
          }
          //for (size_t j = 1; j<4; j++) selseg.setOption(j, (udpIn[9 +ofs] >> j) & 0x01); //only take into account mirrored, on, reversed; ignore selected
          selseg.options = (selseg.options & 0x0071U) | (udpIn[9 +ofs] & 0x0E); // ignore selected, freeze, reset & transitional
          selseg.setOpacity(udpIn[10+ofs]);
          if (applyEffects) {
            strip.setMode(id,  udpIn[11+ofs]);
            selseg.speed     = udpIn[12+ofs];
            selseg.intensity = udpIn[13+ofs];
            selseg.palette   = udpIn[14+ofs];
          }
          if (receiveNotificationColor || !someSel) {
            selseg.setColor(0, RGBW32(udpIn[15+ofs],udpIn[16+ofs],udpIn[17+ofs],udpIn[18+ofs]));
            selseg.setColor(1, RGBW32(udpIn[19+ofs],udpIn[20+ofs],udpIn[21+ofs],udpIn[22+ofs]));
            selseg.setColor(2, RGBW32(udpIn[23+ofs],udpIn[24+ofs],udpIn[25+ofs],udpIn[26+ofs]));
            selseg.setCCT(udpIn[27+ofs]);
          }
          if (version > 11) {
            // when applying synced options ignore selected as it may be used as indicator of which segments to sync
            // freeze, reset should never be synced
            // LSB to MSB: select, reverse, on, mirror, freeze, reset, reverse_y, mirror_y, transpose, map1d2d (3), ssim (2), set (2)
            selseg.options = (selseg.options & 0b0000000000110001U) | (udpIn[28+ofs]<<8) | (udpIn[9 +ofs] & 0b11001110U); // ignore selected, freeze, reset
            if (applyEffects) {
              selseg.custom1 = udpIn[29+ofs];
              selseg.custom2 = udpIn[30+ofs];
              selseg.custom3 = udpIn[31+ofs] & 0x1F;
              selseg.check1  = (udpIn[31+ofs]>>5) & 0x1;
              selseg.check1  = (udpIn[31+ofs]>>6) & 0x1;
              selseg.check1  = (udpIn[31+ofs]>>7) & 0x1;
            }
            startY = (udpIn[32+ofs] << 8 | udpIn[33+ofs]);
            stopY  = (udpIn[34+ofs] << 8 | udpIn[35+ofs]);
          }
          if (receiveSegmentBounds) {
            selseg.setUp(start, stop, udpIn[5+ofs], udpIn[6+ofs], offset, startY, stopY);
          } else {
            selseg.setUp(selseg.start, selseg.stop, udpIn[5+ofs], udpIn[6+ofs], selseg.offset, selseg.startY, selseg.stopY);
          }
        }
        stateChanged = true;
      }

      // simple effect sync, applies to all selected segments
      if (applyEffects && (version < 11 || !receiveSegmentOptions)) {
        for (size_t i = 0; i < strip.getSegmentsNum(); i++) {
          Segment& seg = strip.getSegment(i);
          if (!seg.isActive() || !seg.isSelected()) continue;
          seg.setMode(udpIn[8]);
          seg.speed = udpIn[9];
          if (version > 2) seg.intensity = udpIn[16];
          if (version > 4) seg.setPalette(udpIn[19]);
        }
        stateChanged = true;
      }

      if (applyEffects && version > 5) {
        uint32_t t = (udpIn[25] << 24) | (udpIn[26] << 16) | (udpIn[27] << 8) | (udpIn[28]);
        t += PRESUMED_NETWORK_DELAY; //adjust trivially for network delay
        t -= millis();
        strip.timebase = t;
        timebaseUpdated = true;
      }
    }

    //adjust system time, but only if sender is more accurate than self
    if (version > 7 && version < 200)
    {
      Toki::Time tm;
      tm.sec = (udpIn[30] << 24) | (udpIn[31] << 16) | (udpIn[32] << 8) | (udpIn[33]);
      tm.ms = (udpIn[34] << 8) | (udpIn[35]);
      if (udpIn[29] > toki.getTimeSource()) { //if sender's time source is more accurate
        toki.adjust(tm, PRESUMED_NETWORK_DELAY); //adjust trivially for network delay
        uint8_t ts = TOKI_TS_UDP;
        if (udpIn[29] > 99) ts = TOKI_TS_UDP_NTP;
        else if (udpIn[29] >= TOKI_TS_SEC) ts = TOKI_TS_UDP_SEC;
        toki.setTime(tm, ts);
      } else if (timebaseUpdated && toki.getTimeSource() > 99) { //if we both have good times, get a more accurate timebase
        Toki::Time myTime = toki.getTime();
        uint32_t diff = toki.msDifference(tm, myTime);
        strip.timebase -= PRESUMED_NETWORK_DELAY; //no need to presume, use difference between NTP times at send and receive points
        if (toki.isLater(tm, myTime)) {
          strip.timebase += diff;
        } else {
          strip.timebase -= diff;
        }
      }
    }

    if (version > 3)
    {
      transitionDelayTemp = ((udpIn[17] << 0) & 0xFF) + ((udpIn[18] << 8) & 0xFF00);
    }

    nightlightActive = udpIn[6];
    if (nightlightActive) nightlightDelayMins = udpIn[7];

    if (receiveNotificationBrightness || !someSel) bri = udpIn[2];
    stateUpdated(CALL_MODE_NOTIFICATION);
    return;
  }

  if (!receiveDirect) return;

  // API over UDP
  udpIn[packetSize] = '\0';

  if (requestJSONBufferLock(18)) {
    if (udpIn[0] >= 'A' && udpIn[0] <= 'Z') { //HTTP API
      String apireq = "win"; apireq += '&'; // reduce flash string usage
      apireq += (char*)udpIn;
      handleSet(nullptr, apireq);
    } else if (udpIn[0] == '{') { //JSON API
      DeserializationError error = deserializeJson(doc, udpIn);
      JsonObject root = doc.as<JsonObject>();
      if (!error && !root.isNull()) deserializeState(root);
    }
    releaseJSONBufferLock();
  }
}

void setRealtimePixel(uint16_t i, byte r, byte g, byte b, byte w)
{
  uint32_t pix = i + arlsOffset;
  if (pix < strip.getLengthTotal()) {
    if (!arlsDisableGammaCorrection && gammaCorrectCol) {
      r = gamma8(r);
      g = gamma8(g);
      b = gamma8(b);
      w = gamma8(w);
    }
    if (useMainSegmentOnly) {
      Segment &seg = strip.getMainSegment();
      if (pix<seg.length()) seg.setPixelColor(pix, r, g, b, w);
    } else {
      strip.setPixelColor(pix, r, g, b, w);
    }
  }
}

/*********************************************************************************************\
   Refresh aging for remote units, drop if too old...
\*********************************************************************************************/
void refreshNodeList()
{
  for (NodesMap::iterator it = Nodes.begin(); it != Nodes.end();) {
    bool mustRemove = true;

    if (it->second.ip[0] != 0) {
      if (it->second.age < 10) {
        it->second.age++;
        mustRemove = false;
        ++it;
      }
    }

    if (mustRemove) {
      it = Nodes.erase(it);
    }
  }
}

/*********************************************************************************************\
   Broadcast system info to other nodes. (to update node lists)
\*********************************************************************************************/
void sendSysInfoUDP()
{
  if (!udp2Connected) return;

  IPAddress ip = Network.localIP();
  if (!ip || ip == IPAddress(255,255,255,255)) ip = IPAddress(4,3,2,1);

  // TODO: make a nice struct of it and clean up
  //  0: 1 byte 'binary token 255'
  //  1: 1 byte id '1'
  //  2: 4 byte ip
  //  6: 32 char name
  // 38: 1 byte node type id
  // 39: 1 byte node id
  // 40: 4 byte version ID
  // 44 bytes total

  // send my info to the world...
  uint8_t data[44] = {0};
  data[0] = 255;
  data[1] = 1;

  for (size_t x = 0; x < 4; x++) {
    data[x + 2] = ip[x];
  }
  memcpy((byte *)data + 6, serverDescription, 32);
  #ifdef ESP8266
  data[38] = NODE_TYPE_ID_ESP8266;
  #elif defined(CONFIG_IDF_TARGET_ESP32C3)
  data[38] = NODE_TYPE_ID_ESP32C3;
  #elif defined(CONFIG_IDF_TARGET_ESP32S3)
  data[38] = NODE_TYPE_ID_ESP32S3;
  #elif defined(CONFIG_IDF_TARGET_ESP32S2)
  data[38] = NODE_TYPE_ID_ESP32S2;
  #elif defined(CONFIG_IDF_TARGET_ESP32P4)
  data[38] = NODE_TYPE_ID_ESP32P4;
  #elif defined(ARDUINO_ARCH_ESP32)
  data[38] = NODE_TYPE_ID_ESP32;
  #else
  data[38] = NODE_TYPE_ID_UNDEFINED;
  #endif
  if (bri) data[38] |= 0x80U;  // add on/off state
  data[39] = ip[3]; // unit ID == last IP number

  uint32_t build = VERSION;
  for (size_t i=0; i<sizeof(uint32_t); i++)
    data[40+i] = (build>>(8*i)) & 0xFF;

  IPAddress broadcastIP(255, 255, 255, 255);
  if (0 != notifier2Udp.beginPacket(broadcastIP, udpPort2)) {  // WLEDMM beginPacket == 0 --> error
    notifier2Udp.write(data, sizeof(data));
    notifier2Udp.endPacket();
  }
}


/*********************************************************************************************\
 * Art-Net, DDP, E1.31 output
\*********************************************************************************************/

// Protocol constants
#define DDP_HEADER_LEN          10
#define DDP_MAX_DATALEN         1440  // Max payload per packet (480 RGB or 360 RGBW pixels)

#define E131_HEADER_LEN         126
#define E131_MAX_DATALEN        512   // DMX universe limit

#define ARTNET_HEADER_LEN       18
#define ARTNET_MAX_DATALEN      512   // DMX universe limit

// DDP flags
#define DDP_FLAGS1_VER          0xc0
#define DDP_FLAGS1_VER1         0x40
#define DDP_FLAGS1_PUSH         0x01
#define DDP_FLAGS1_QUERY        0x02
#define DDP_FLAGS1_REPLY        0x04
#define DDP_FLAGS1_STORAGE      0x08
#define DDP_FLAGS1_TIME         0x10

#define DDP_ID_DISPLAY          1
#define DDP_ID_CONFIG           250
#define DDP_ID_STATUS           251

// Direct ip4_addr_t creation for E1.31 multicast
static inline void e131MulticastAddr(uint16_t universe, ip4_addr_t* addr) {
  IP4_ADDR(addr, 239, 255, (universe >> 8) & 0xFF, universe & 0xFF);
}

// For sending packets (still need IPAddress)
static inline IPAddress e131MulticastIP(uint16_t universe) {
  return IPAddress(239, 255, (universe >> 8) & 0xFF, universe & 0xFF);
}

static       size_t sequenceNumber = 0;
static const byte   ART_NET_HEADER[12] PROGMEM = { 0x41,0x72,0x74,0x2d,0x4e,0x65,0x74,0x00,0x00,0x50,0x00,0x0e };

#if defined(CONFIG_IDF_TARGET_ESP32P4)
extern "C" {
  int p4_mul16x16(uint8_t* outpacket, uint8_t* brightness, uint16_t num_loops, uint8_t* pixelbuffer);
}
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// Shared pixel processing - handles brightness, color order, and pixel remapping
// ═══════════════════════════════════════════════════════════════════════════════
static inline void processPixelData(
  uint8_t* dest,
  const uint8_t* src,
  uint_fast16_t packetSize,
  uint_fast32_t bufferOffset,
  uint8_t bri,
  bool isRGBW,
  uint8_t color_order,
  uint32_t srcPixelCount  // Add this parameter - total pixels in src buffer
) {
  const uint8_t bpp = isRGBW ? 4 : 3;

  #ifdef WLEDMM_REMAP_AT_OUTPUT
  // Hold busMutex for the entire remapping section to prevent setUpMatrix()
  // from freeing/swapping the mapping table while we're reading it.
  // Non-blocking: if the mutex is unavailable (e.g. setUpMatrix is running),
  // fall through to the direct-copy path for this one frame.
  bool tookMutex = (xSemaphoreTake(busMutex, 0) == pdTRUE);
  uint32_t* mappingTable = tookMutex ? strip.getCustomMappingTable() : nullptr;
  uint32_t  mappingTableSize = tookMutex ? strip.getCustomMappingTableSize() : 0;

  // Must have valid table AND valid size
  const bool hasMappingTable = (mappingTable != nullptr && mappingTableSize > 0);
  const bool needsColorReorder = (color_order != COL_ORDER_RGB);
  const bool fullBrightness = (bri == 255);

  // Color order lookup
  uint8_t r_idx = 0, g_idx = 1, b_idx = 2;
  switch (color_order) {
  case COL_ORDER_GRB: r_idx = 1; g_idx = 0; b_idx = 2; break;
  case COL_ORDER_BRG: r_idx = 1; g_idx = 2; b_idx = 0; break;
  case COL_ORDER_RBG: r_idx = 0; g_idx = 2; b_idx = 1; break;
  case COL_ORDER_BGR: r_idx = 2; g_idx = 1; b_idx = 0; break;
  case COL_ORDER_GBR: r_idx = 1; g_idx = 0; b_idx = 2; break;
  default: break;  // RGB
  }

  // Fast path: no mapping, no color reorder
  if (!hasMappingTable && !needsColorReorder) {
    if (tookMutex) xSemaphoreGive(busMutex);
    #if defined(CONFIG_IDF_TARGET_ESP32P4)
    p4_mul16x16(dest, &bri, (packetSize >> 4) + 1, (uint8_t*)(src + bufferOffset));
    return;
    #else
    if (fullBrightness) {
      memcpy(dest, src + bufferOffset, packetSize);
    } else {
      for (uint_fast16_t i = 0; i < packetSize; i += bpp) {
        dest[i] = (src[bufferOffset + i] * bri) >> 8;
        dest[i + 1] = (src[bufferOffset + i + 1] * bri) >> 8;
        dest[i + 2] = (src[bufferOffset + i + 2] * bri) >> 8;
        if (isRGBW) dest[i + 3] = (src[bufferOffset + i + 3] * bri) >> 8;
      }
    }
    return;
    #endif
  }

  // Slow path: mapping and/or color reorder
  const uint_fast16_t numPixels = packetSize / bpp;
  const uint32_t startPixel = bufferOffset / bpp;

  for (uint_fast16_t i = 0; i < numPixels; ++i) {
    const uint8_t* pixel;

    if (hasMappingTable) {
      uint32_t idx = startPixel + i;

      // Bounds check index into mapping table
      if (idx >= mappingTableSize) {
        dest += bpp;
        continue;
      }

      uint32_t map = mappingTable[idx];

      // Skip unmapped pixels (UINT32_MAX = no mapping)
      if (map == UINT32_MAX) {
        continue;
      }

      // Bounds check mapped pixel index into source buffer
      if (map >= srcPixelCount) {
        dest += bpp;
        continue;
      }

      pixel = src + (map * bpp);
    } else {
      // No mapping table, just color reorder
      pixel = src + bufferOffset + (i * bpp);
    }

    if (fullBrightness) {
      dest[r_idx] = pixel[0];
      dest[g_idx] = pixel[1];
      dest[b_idx] = pixel[2];
      if (isRGBW) dest[3] = pixel[3];
    } else {
      dest[r_idx] = (pixel[0] * bri) >> 8;
      dest[g_idx] = (pixel[1] * bri) >> 8;
      dest[b_idx] = (pixel[2] * bri) >> 8;
      if (isRGBW) dest[3] = (pixel[3] * bri) >> 8;
    }
    dest += bpp;
  }
  if (tookMutex) xSemaphoreGive(busMutex);
  #else
  // No WLEDMM_REMAP_AT_OUTPUT - simple path
  #if defined(CONFIG_IDF_TARGET_ESP32P4)
  p4_mul16x16(dest, &bri, (packetSize >> 4) + 1, (uint8_t*)(src + bufferOffset));
  #else
  if (bri == 255) {
    memcpy(dest, src + bufferOffset, packetSize);
  } else {
    for (uint_fast16_t i = 0; i < packetSize; i += bpp) {
      dest[i] = (src[bufferOffset + i] * bri) >> 8;
      dest[i + 1] = (src[bufferOffset + i + 1] * bri) >> 8;
      dest[i + 2] = (src[bufferOffset + i + 2] * bri) >> 8;
      if (isRGBW) dest[i + 3] = (src[bufferOffset + i + 3] * bri) >> 8;
    }
  }
  #endif
  #endif
}

extern "C" {
  #include "lwip/opt.h"
  #include "lwip/inet.h"
  #include "lwip/udp.h"
  #include "lwip/igmp.h"
  #include "lwip/ip_addr.h"
  #include "lwip/mld6.h"
  #include "lwip/prot/ethernet.h"
  #include <esp_err.h>
  #include <esp_wifi.h>
  #include <esp_netif.h>
  #include <esp_netif_net_stack.h>
}

#include "lwip/priv/tcpip_priv.h"

class FastAsyncUDP : public AsyncUDP {
  ip_addr_t _addr_cache;
  struct udp_api_call_t {
    struct tcpip_api_call_data call;
    struct udp_pcb* pcb;
    const ip_addr_t* addr;
    u16_t port;
    struct pbuf* pb;
    struct netif* netif;
    err_t err;
  };
  udp_api_call_t _msg;  // Reuse instead of stack allocation each call

  static err_t _udp_sendto_if_api(struct tcpip_api_call_data* api_call_msg) {
    udp_api_call_t* msg = (udp_api_call_t*)api_call_msg;
    msg->err = udp_sendto_if(msg->pcb, msg->pb, msg->addr, msg->port, msg->netif);
    return msg->err;
  }

public:
  // Call once at startup
  bool connect(const IPAddress addr, uint16_t port) {
    _pcb = udp_new();
    if (!_pcb) return false;

    _addr_cache.type = IPADDR_TYPE_V4;
    _addr_cache.u_addr.ip4.addr = static_cast<uint32_t>(addr);

    // Pre-fill the static parts of the message
    _msg.pcb = _pcb;
    _msg.addr = &_addr_cache;
    _msg.port = port;
    _msg.netif = sender_netif;

    return true;
  }

  size_t write(const uint8_t* data, size_t len) {
    pbuf* pbt = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!pbt) return 0;

    memcpy(pbt->payload, data, len);

    _msg.pb = pbt;  // Only thing that changes per-call
    tcpip_api_call(_udp_sendto_if_api, (struct tcpip_api_call_data*)&_msg);

    pbuf_free(pbt);
    return (_msg.err == ERR_OK) ? len : 0;
  }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Main broadcast function
// type: 0=DDP, 1=E1.31, 2=Art-Net
// ═══════════════════════════════════════════════════════════════════════════════
uint8_t __attribute__((hot)) realtimeBroadcast(
  uint8_t type,
  IPAddress client,
  uint32_t length,
  uint8_t* buffer_in,
  uint8_t bri,
  bool isRGBW,
  uint32_t outputs,
  uint32_t leds_per_output,
  uint8_t fps_limit,
  uint8_t color_order,
  bool e131_multicast
) {
  if (fps_limit < 1 || fps_limit > 120) fps_limit = 60;
  if (!(apActive || interfacesInited) || !length) return 1;
  if (!e131_multicast && !client[0]) return 1;  // Unicast requires valid IP

  const uint8_t bpp = isRGBW ? 4 : 3;
  const size_t totalChannels = length * bpp;
  const char* protocolName = (type == 0) ? "DDP" : (type == 1) ? "E1.31" : "Art-Net";
  static auto last_netif = sender_netif;

  // Packet buffer sized for DDP (largest: 10 + 1440 = 1450 bytes)
  #ifdef ESP32
  static byte* packet_buffer = (byte*)heap_caps_calloc_prefer(DDP_HEADER_LEN + DDP_MAX_DATALEN, sizeof(byte), 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA, MALLOC_CAP_DEFAULT, MALLOC_CAP_SPIRAM);
  #else
  static byte* packet_buffer = (byte*)calloc(DDP_HEADER_LEN + DDP_MAX_DATALEN, sizeof(byte));
  #endif

  // FPS limiting
  static unsigned long frame_limiter = 0;
  if (fps_limit > 0) {
    long time_to_wait = frame_limiter - micros();
    if (time_to_wait > 0) {
      if (RealtimeSkipFrame) return 0;  // Frame skip for all protocols

      while (time_to_wait > 2000) {
        vTaskDelay(1);
        time_to_wait = frame_limiter - micros();
      }
      while ((long)(frame_limiter - micros()) > 0) {
        // asm volatile("nop");
      }
    }
  }

  unsigned long timer = micros();

  #ifdef REALTIME_OUTPUT_TIMER
  uint_fast32_t datatotal = 0;
  uint_fast16_t packetstotal = 0;
  uint16_t headerLen = (type == 0) ? DDP_HEADER_LEN : (type == 1) ? E131_HEADER_LEN : ARTNET_HEADER_LEN;
  #endif

  switch (type) {

  // ═══════════════════════════════════════════════════════════════════
  // DDP - Distributed Display Protocol
  // Efficiency: 94.9% | Header: 10 bytes | Max payload: 1440 bytes
  // ═══════════════════════════════════════════════════════════════════
  case 0: {
    #if !defined(CONFIG_IDF_TARGET_ESP32C5)
    static FastAsyncUDP ddpUdp;
    #else
    static AsyncUDP ddpUdp;
    #endif
    static IPAddress lastClient((uint32_t)0);

    if ((uint32_t)client != (uint32_t)lastClient || last_netif != sender_netif) {
      ddpUdp.connect(client, DDP_DEFAULT_PORT);
      lastClient = client;
      last_netif = sender_netif;
    }
    
    const uint16_t maxChannels = (DDP_MAX_DATALEN / bpp) * bpp;
    const size_t packetCount = ((totalChannels - 1) / maxChannels) + 1;

    uint32_t channel = 0;
    size_t bufferOffset = 0;

    sequenceNumber++;
    if (sequenceNumber > 15) sequenceNumber = 1;

    for (size_t pkt = 0; pkt < packetCount; pkt++) {
      size_t remaining = totalChannels - bufferOffset;
      size_t packetSize = (remaining < maxChannels) ? remaining : maxChannels;

      uint8_t flags = DDP_FLAGS1_VER1;
      if (pkt == packetCount - 1) flags |= DDP_FLAGS1_PUSH;

      // DDP Header
      packet_buffer[0] = flags;
      packet_buffer[1] = sequenceNumber;
      packet_buffer[2] = isRGBW ? DDP_TYPE_RGBW32 : DDP_TYPE_RGB24;
      packet_buffer[3] = DDP_ID_DISPLAY;
      packet_buffer[4] = (channel >> 24) & 0xFF;
      packet_buffer[5] = (channel >> 16) & 0xFF;
      packet_buffer[6] = (channel >> 8) & 0xFF;
      packet_buffer[7] = channel & 0xFF;
      packet_buffer[8] = (packetSize >> 8) & 0xFF;
      packet_buffer[9] = packetSize & 0xFF;

      processPixelData(packet_buffer + DDP_HEADER_LEN, buffer_in, packetSize, bufferOffset, bri, isRGBW, color_order, length);

      if (!ddpUdp.write(packet_buffer, packetSize + DDP_HEADER_LEN)) {
        DEBUG_PRINTLN(F("DDP writeTo error"));
        return 1;
      }

      #ifdef REALTIME_OUTPUT_TIMER
      packetstotal++;
      datatotal += packetSize + DDP_HEADER_LEN + 46;  // +46 for UDP/IP/Eth overhead
      #endif

      bufferOffset += packetSize;
      channel += packetSize;
    }
    break;
  }

  // ═══════════════════════════════════════════════════════════════════
  // E1.31 (sACN) - Streaming ACN
  // Efficiency: 72.7% | Header: 126 bytes | Max payload: 512 bytes
  // Supports unicast and multicast (239.255.x.x per universe)
  // ═══════════════════════════════════════════════════════════════════
  case 1: {
    static AsyncUDP e131Udp;
    static uint8_t e131_cid[16] = { 0 };
    static bool cid_init = false;

    // Multicast group management
    static bool multicast_joined = false;
    static uint16_t joined_start = 0, joined_count = 0;

    if (!cid_init) {
      uint8_t mac[6];
      Network.localMAC(mac);
      memcpy(e131_cid, mac, 6);
      memcpy(e131_cid + 6, "WLED", 4);
      cid_init = true;
    }

    const uint16_t maxChannels = isRGBW ? 512 : 510;
    const size_t packetCount = ((totalChannels - 1) / maxChannels) + 1;

    // Join/leave multicast groups as needed
    if (e131_multicast) {
      if (!multicast_joined || joined_start != 1 || joined_count != packetCount) {
        ip4_addr_t mcast_addr;

        for (uint16_t u = joined_start; u < joined_start + joined_count; u++) {
          e131MulticastAddr(u, &mcast_addr);
          igmp_leavegroup(IP4_ADDR_ANY4, &mcast_addr);
        }
        for (uint16_t u = 1; u <= packetCount; u++) {
          e131MulticastAddr(u, &mcast_addr);
          igmp_joingroup(IP4_ADDR_ANY4, &mcast_addr);
        }
        joined_start = 1;
        joined_count = packetCount;
        multicast_joined = true;
      }
    }

    size_t bufferOffset = 0;
    uint16_t universe = 1;
    const uint32_t channelsPerOutput = leds_per_output * bpp;

    sequenceNumber = (sequenceNumber + 1) & 0xFF;
    if (sequenceNumber == 0) sequenceNumber = 1;

    // Build static E1.31 header portions
    packet_buffer[0] = 0x00; packet_buffer[1] = 0x10;
    packet_buffer[2] = 0x00; packet_buffer[3] = 0x00;
    memcpy(packet_buffer + 4, "ASC-E1.17\0\0\0", 12);

    packet_buffer[18] = 0x00; packet_buffer[19] = 0x00;
    packet_buffer[20] = 0x00; packet_buffer[21] = 0x04;

    memcpy(packet_buffer + 22, e131_cid, 16);

    packet_buffer[40] = 0x00; packet_buffer[41] = 0x00;
    packet_buffer[42] = 0x00; packet_buffer[43] = 0x02;

    memset(packet_buffer + 44, 0, 64);
    strcpy((char*)(packet_buffer + 44), "WLED");

    packet_buffer[108] = 100;
    packet_buffer[109] = 0x00; packet_buffer[110] = 0x00;
    packet_buffer[112] = 0x00;

    packet_buffer[117] = 0x02;
    packet_buffer[118] = 0xA1;
    packet_buffer[119] = 0x00; packet_buffer[120] = 0x00;
    packet_buffer[121] = 0x00; packet_buffer[122] = 0x01;
    packet_buffer[125] = 0x00;

    for (uint32_t output = 0; output < outputs; output++) {
      bufferOffset = output * channelsPerOutput;
      uint32_t channels_remaining = channelsPerOutput;

      while (channels_remaining > 0) {
        uint32_t packetSize = (channels_remaining < maxChannels) ? channels_remaining : maxChannels;
        channels_remaining -= packetSize;

        uint16_t rootLen = 110 + packetSize;
        packet_buffer[16] = 0x70 | ((rootLen >> 8) & 0x0F);
        packet_buffer[17] = rootLen & 0xFF;

        uint16_t framingLen = 88 + packetSize;
        packet_buffer[38] = 0x70 | ((framingLen >> 8) & 0x0F);
        packet_buffer[39] = framingLen & 0xFF;

        uint16_t dmpLen = 11 + packetSize;
        packet_buffer[115] = 0x70 | ((dmpLen >> 8) & 0x0F);
        packet_buffer[116] = dmpLen & 0xFF;

        uint16_t propCount = packetSize + 1;
        packet_buffer[123] = (propCount >> 8) & 0xFF;
        packet_buffer[124] = propCount & 0xFF;

        packet_buffer[111] = sequenceNumber;
        packet_buffer[113] = (universe >> 8) & 0xFF;
        packet_buffer[114] = universe & 0xFF;

        processPixelData(packet_buffer + E131_HEADER_LEN, buffer_in, packetSize, bufferOffset, bri, isRGBW, color_order, length);

        IPAddress dest = e131_multicast ? e131MulticastIP(universe) : client;

        if (!e131Udp.writeTo(packet_buffer, packetSize + E131_HEADER_LEN, dest, E131_DEFAULT_PORT, send_interface)) {
          DEBUG_PRINTLN(F("E1.31 writeTo error"));
          return 1;
        }

        #ifdef REALTIME_OUTPUT_TIMER
        packetstotal++;
        datatotal += packetSize + E131_HEADER_LEN + 46;
        #endif

        bufferOffset += packetSize;
        universe++;
      }
    }

    #ifdef E131_SYNC_ENABLED
    if (e131_multicast && packetCount > 1) {
      packet_buffer[20] = 0x00; packet_buffer[21] = 0x08;
      packet_buffer[16] = 0x70; packet_buffer[17] = 33;
      packet_buffer[38] = 0x70; packet_buffer[39] = 11;
      packet_buffer[40] = 0x00; packet_buffer[41] = 0x00;
      packet_buffer[42] = 0x00; packet_buffer[43] = 0x01;
      packet_buffer[44] = sequenceNumber;
      packet_buffer[45] = 0xF9; packet_buffer[46] = 0xFF;
      packet_buffer[47] = 0x00; packet_buffer[48] = 0x00;

      e131Udp.writeTo(packet_buffer, 49, e131MulticastAddress(63999), E131_DEFAULT_PORT);

      #ifdef REALTIME_OUTPUT_TIMER
      packetstotal++;
      datatotal += 49 + 46;
      #endif

      packet_buffer[20] = 0x00; packet_buffer[21] = 0x04;
      packet_buffer[42] = 0x00; packet_buffer[43] = 0x02;
    }
    #endif
    break;
  }

  case 2: {
    #if !defined(CONFIG_IDF_TARGET_ESP32C5)
    static FastAsyncUDP artnetUdp;
    #else
    static AsyncUDP artnetUdp;
    #endif

    #ifdef USERMOD_ARTNETMAP
    ArtNetMapUsermod* artnetMap = (ArtNetMapUsermod*)usermods.lookup(USERMOD_ID_ARTNETMAP);
    const bool artnetMapActive = artnetMap && artnetMap->isEnabled();
    #else
    const bool artnetMapActive = false;
    #endif

    static IPAddress lastDest((uint32_t)0);
    IPAddress dest = client;
    #ifdef USERMOD_ARTNETMAP
    if (artnetMapActive) {
      const char* tip = artnetMap->getTargetIP();
      if (tip && tip[0]) {
        IPAddress parsed;
        if (parsed.fromString(tip)) dest = parsed;
      }
    }
    #endif

    if ((uint32_t)dest != (uint32_t)lastDest || last_netif != sender_netif) {
      artnetUdp.connect(dest, ARTNET_DEFAULT_PORT);
      lastDest = dest;
      last_netif = sender_netif;
    }

    if (packet_buffer[0] != 'A') {
      memcpy(packet_buffer, ART_NET_HEADER, 12);
    }

    uint16_t maxChannels = isRGBW ? 512 : 510;
    #ifdef USERMOD_ARTNETMAP
    if (artnetMapActive) maxChannels = artnetMap->getChannelsPerUniverse();
    #endif

    sequenceNumber = (sequenceNumber + 1) & 0xFF;
    if (sequenceNumber == 0) sequenceNumber = 1;

    uint_fast32_t bufferOffset = 0;

    #ifdef USERMOD_ARTNETMAP
    if (artnetMapActive && artnetMap->getNumOutputs() > 0) {
      // Use usermod configuration - each output has its own start universe and LED count
      uint32_t numOutputs = artnetMap->getNumOutputs();
      // length = artnetMap->getTotalLeds();
      for (uint32_t output = 0; output < numOutputs; output++) {
        uint_fast16_t universe = artnetMap->getStartUniverse(output);
        uint32_t output_leds = artnetMap->getLedsPerOutput(output);
        uint32_t channels_remaining = output_leds * bpp;

        while (channels_remaining > 0) {
          uint32_t packetSize = (channels_remaining < maxChannels)
            ? channels_remaining : maxChannels;
          channels_remaining -= packetSize;

          packet_buffer[12] = sequenceNumber;
          packet_buffer[13] = 0;
          packet_buffer[14] = universe & 0xFF;
          packet_buffer[15] = (universe >> 8) & 0xFF;
          packet_buffer[16] = (packetSize >> 8) & 0xFF;
          packet_buffer[17] = packetSize & 0xFF;

          processPixelData(packet_buffer + ARTNET_HEADER_LEN, buffer_in, packetSize, bufferOffset, bri, isRGBW, color_order, length);

          if (!artnetUdp.write(packet_buffer, packetSize + ARTNET_HEADER_LEN)) {
            USER_PRINTLN(F("Art-Net writeTo error"));
            return 1;
          }

          #ifdef REALTIME_OUTPUT_TIMER
          packetstotal++;
          datatotal += packetSize + ARTNET_HEADER_LEN + 46;
          #endif

          bufferOffset += packetSize;
          universe++;
        }
      }
    } else
      #endif
    {
      // Default behavior - sequential universes, uniform leds_per_output
      uint_fast16_t universe = 0;

      for (uint32_t output = 0; output < outputs; output++) {
        uint32_t channels_remaining = leds_per_output * bpp;

        while (channels_remaining > 0) {
          uint32_t packetSize = (channels_remaining < maxChannels)
            ? channels_remaining : maxChannels;
          channels_remaining -= packetSize;

          packet_buffer[12] = sequenceNumber;
          packet_buffer[13] = 0;
          packet_buffer[14] = universe & 0xFF;
          packet_buffer[15] = (universe >> 8) & 0xFF;
          packet_buffer[16] = (packetSize >> 8) & 0xFF;
          packet_buffer[17] = packetSize & 0xFF;

          #ifdef REALTIME_TESTING_ZEROS
          uint8_t test_bri = 0;
          processPixelData(packet_buffer + ARTNET_HEADER_LEN, buffer_in, packetSize, bufferOffset, test_bri, isRGBW, color_order, length);
          #else
          processPixelData(packet_buffer + ARTNET_HEADER_LEN, buffer_in, packetSize, bufferOffset, bri, isRGBW, color_order, length);
          #endif

          if (!artnetUdp.write(packet_buffer, packetSize + ARTNET_HEADER_LEN)) {
            USER_PRINTLN(F("Art-Net writeTo error"));
            return 1;
          }

          #ifdef REALTIME_OUTPUT_TIMER
          packetstotal++;
          datatotal += packetSize + ARTNET_HEADER_LEN + 46;
          #endif

          bufferOffset += packetSize;
          universe++;
        }
      }
    }

    #ifdef ARTNET_SYNC_ENABLED
    packet_buffer[9] = 0x52;
    packet_buffer[12] = 0x00;
    #ifdef ARTNET_SYNC_STRICT
    WiFiUDP artnetsync;
    artnetsync.begin(ETH.localIP(), ARTNET_DEFAULT_PORT);
    artnetsync.beginPacket(IPADDR_BROADCAST, ARTNET_DEFAULT_PORT);
    artnetsync.write(packet_buffer, 14);
    if (!artnetsync.endPacket()) {
      DEBUG_PRINTLN(F("Art-Net Sync Strict error"));
      return 1;
    }
    #else
    if (!artnetUdp.broadcastTo(packet_buffer, 14, ARTNET_DEFAULT_PORT)) {
      DEBUG_PRINTLN(F("Art-Net Sync error"));
      return 1;
    }
    #endif

    #ifdef REALTIME_OUTPUT_TIMER
    packetstotal++;
    datatotal += 14 + 46;
    #endif

    packet_buffer[9] = ART_NET_HEADER[9];
    #endif
    break;
  }

  default:
    return 1;
  }

  if (fps_limit > 0) {
    frame_limiter = timer + (1000000 / fps_limit);
  }

  #ifdef REALTIME_OUTPUT_TIMER
  if (datatotal > 0 && (micros() % 100 < 3)) {
    unsigned long elapsed = micros() - timer;
    float mbps = (float)(datatotal * 8) / (float)elapsed;
    USER_PRINTF("%s: %lu pixels, %lu us, %u bytes in %u pkts, %.2f Mbit/s @ %u FPS\n",
      protocolName, length, elapsed, datatotal, packetstotal, mbps, strip.getFps());
  }
  #endif

  return 0;
}
