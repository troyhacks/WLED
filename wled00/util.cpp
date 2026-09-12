#include "wled.h"
#include "fcn_declare.h"
#include "const.h"
#include <dirent.h>
#include <sys/stat.h>
#include <driver/i2c_master.h>

void scanI2C() {
  struct I2CDevice { uint8_t addr; const char* name; };
  static const I2CDevice knownDevices[] = {
    {0x10, "ES8388"},
    {0x13, "ES7243"},
    {0x14, "GT911 Touch Panel Controller (alt)"},
    {0x18, "ES8311 (common ESP32-P4 on-board mic codec)"},
    {0x19, "LIS3DH (alt)"},
    {0x1A, "WM8978/AC101"},
    {0x1C, "MMA8452Q"},
    {0x1D, "ADXL345/MMA8452Q"},
    {0x20, "PCF8574/TCA6408"},
    {0x23, "BH1750"},
    {0x27, "PCF8574/LCD"},
    {0x29, "VL53L0X/VL53L1X/VL53L8CX"},
    {0x37, "LT8912B internal video pipeline state (streaming 16-entry FIFO, undocumented)"},
    {0x38, "FT6336/AHT10/VEML6070"},
    {0x39, "APDS9960/TSL2561"},
    {0x3A, "LT8912B phantom/undocumented register bank"},
    {0x3C, "SSD1306 OLED"},
    {0x3D, "SSD1306 OLED (alt)"},
    {0x40, "ES7210 (common ESP32-P4 audio processor) but also matches INA219/HDC1080/PCA9685"},
    {0x44, "SHT30/SHT31"},
    {0x48, "LT8912B HDMI bridge (main control)"},
    {0x49, "LT8912B HDMI bridge (MIPI/DSI timing)"},
    {0x4A, "LT8912B HDMI bridge (AVI InfoFrame)"},
    {0x4B, "LT8912B HDMI bridge (EDID emulation write page — zeros = no custom EDID)"},
    {0x50, "LT8912B DDC/EDID proxy (connected monitor's EDID)"},
    {0x51, "PCF8563 RTC"},
    {0x52, "Nunchuk/VL53L8CX (LP)"},
    {0x53, "ADXL345 (alt)"},
    {0x54, "FE1.1s USB hub config EEPROM (24Cxx, A2=1)"},
    {0x57, "MAX30102"},
    {0x5A, "MLX90614/CCS811/MPR121"},
    {0x5B, "CCS811 (alt)/MPR121"},
    {0x5C, "AM2320/BH1750 (alt)"},
    {0x5D, "GT911 Touch Panel Controller"},
    {0x60, "SI1145/MCP4725"},
    {0x62, "SCD30/TSL2591"},
    {0x68, "DS3231 RTC/MPU6050/ICM20948"},
    {0x69, "MPU6050 (alt)/ICM20948"},
    {0x76, "BME280/BMP280/MS5611"},
    {0x77, "BME280/BMP180/BMP085"},
    {0x78, "S11059"},
  };
  const int knownCount = sizeof(knownDevices) / sizeof(knownDevices[0]);

  auto getName = [&](uint8_t addr) -> const char* {
    for (int i = 0; i < knownCount; i++) {
      if (addr == 0x40) ES7210_present = true;
      if (knownDevices[i].addr == addr) return knownDevices[i].name;
    }
    return nullptr; 
    };

  Serial.println(F("\n--- I2C Scan ---"));
  int found = 0;

  // Install a transient i2c_master_bus on port 1 — leaves port 0 free
  // for the audioreactive usermod (AR_I2C) or any legacy Wire bus. Port
  // 1 is available on chips like the ESP32-P4 which have SOC_HP_I2C_NUM=2.
  // On single-port chips (SOC_HP_I2C_NUM=1) this may fail — in that case
  // we log a friendly message and return.
  int8_t sda = i2c_sda, scl = i2c_scl;
  #ifdef HW_PIN_SDA
  if (sda < 0) sda = HW_PIN_SDA;
  if (scl < 0) scl = HW_PIN_SCL;
  #endif
  if (sda < 0 || scl < 0) {
    Serial.println(F("  (no I2C pins configured)"));
    return;
  }

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port          = (i2c_port_t)1;
  bus_cfg.sda_io_num        = (gpio_num_t)sda;
  bus_cfg.scl_io_num        = (gpio_num_t)scl;
  bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;

  i2c_master_bus_handle_t bus = NULL;
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
  if (err != ESP_OK || bus == NULL) {
    Serial.printf("  (i2c_new_master_bus port=1 failed: %d)\n", err);
    return;
  }

  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
      found++;
      Serial.printf("  0x%02X: ", addr);
      const char* name = getName(addr);
      Serial.println(name ? name : "Unknown");
    }
  }
  i2c_del_master_bus(bus);

  Serial.printf("--- %d device(s) found ---\n\n", found);
  if (ES7210_present) USER_PRINTLN("ES7210_present == true");
}

//helper to get int value at a position in string
int getNumVal(const String* req, uint32_t pos)
{
  return req->substring(pos+3).toInt();
}

bool backupLittleFStoSD() {
  // Delete oldest backup if it exists
  struct stat st;
  if (stat("/sdcard/littlefs_backup_9", &st) == 0) {
    removeDirectory("/sdcard/littlefs_backup_9");
  }

  // Rotate existing backups 8->9, 7->8, ... 0->1
  for (int i = 8; i >= 0; i--) {
    char oldPath[32];
    char newPath[32];
    snprintf(oldPath, sizeof(oldPath), "/sdcard/littlefs_backup_%d", i);
    snprintf(newPath, sizeof(newPath), "/sdcard/littlefs_backup_%d", i + 1);

    if (stat(oldPath, &st) == 0) {
      rename(oldPath, newPath);
    }
  }

  // Create new backup at 0
  mkdir("/sdcard/littlefs_backup_0", 0755);
  return copyDirectory("/littlefs", "/sdcard/littlefs_backup_0");
}

bool removeDirectory(const char* path) {
  DIR* dir = opendir(path);
  if (!dir) return false;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char fullPath[128];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);

    struct stat st;
    if (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode)) {
      removeDirectory(fullPath);
    } else {
      remove(fullPath);
    }
  }

  closedir(dir);
  return rmdir(path) == 0;
}

bool copyFile(const char* srcPath, const char* destPath) {
  FILE* src = fopen(srcPath, "rb");
  if (!src) return false;

  FILE* dest = fopen(destPath, "wb");
  if (!dest) {
    fclose(src);
    return false;
  }

  uint8_t buf[512];
  size_t bytesRead;
  while ((bytesRead = fread(buf, 1, sizeof(buf), src)) > 0) {
    fwrite(buf, 1, bytesRead, dest);
  }

  fclose(src);
  fclose(dest);
  return true;
}

bool copyDirectory(const char* srcDir, const char* destDir) {
  DIR* dir = opendir(srcDir);
  if (!dir) return false;

  mkdir(destDir, 0755);

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    // Skip . and .. and mount points
    if (strcmp(entry->d_name, ".") == 0 ||
      strcmp(entry->d_name, "..") == 0 ||
      strcmp(entry->d_name, "sdcard") == 0 ||
      strncmp(entry->d_name, "usb", 3) == 0) {
      continue;
    }

    char srcPath[128];
    char destPath[128];
    snprintf(srcPath, sizeof(srcPath), "%s%s%s",
      srcDir, (strcmp(srcDir, "/") == 0) ? "" : "/", entry->d_name);
    snprintf(destPath, sizeof(destPath), "%s/%s", destDir, entry->d_name);

    struct stat st;
    if (stat(srcPath, &st) == 0 && S_ISDIR(st.st_mode)) {
      copyDirectory(srcPath, destPath);
    } else {
      copyFile(srcPath, destPath);
    }
  }

  closedir(dir);
  return true;
}

bool saveBakedLedMap(const char* name, uint16_t width, uint16_t height, uint32_t* mappingTable, uint32_t tableSize, const char* filename) {
  if (!mappingTable || tableSize == 0) return false;
  bakeMap = false;
  File f = WLED_FS.open(filename, "w");
  if (!f) {
    USER_PRINTLN(F("Failed to open file for writing"));
    return false;
  }

  // Write header
  f.print(F("{\"n\":\""));
  f.print(name);
  f.print(F("\"\n,\"width\":"));
  f.print(width);
  f.print(F("\n,\"height\":"));
  f.print(height);
  f.print(F("\n,\"map\":[\n"));

  // Write map data - format as rows for readability (optional)
  const uint16_t valuesPerRow = width > 0 ? width : 48;  // Match width or default

  for (uint32_t i = 0; i < tableSize; i++) {
    if (mappingTable[i] == UINT32_MAX) {
      f.print(F("-1"));
    } else {
      f.print(mappingTable[i]);
    }

    if (i < tableSize - 1) {
      f.print(',');
      // Newline after each row for readability
      if ((i + 1) % valuesPerRow == 0) {
        f.print('\n');
      }
    }
  }

  f.print(F("\n]}"));
  f.close();

  USER_PRINTF("Saved LED map '%s' (%u values) to %s\n", name, tableSize, filename);
  return true;
}

//helper to get int value with in/decrementing support via ~ syntax
void parseNumber(const char* str, byte* val, byte minv, byte maxv)
{
  if (str == nullptr || str[0] == '\0') return;
  if (str[0] == 'r') {*val = random8(minv,maxv?maxv:255); return;} // maxv for random cannot be 0
  bool wrap = false;
  if (str[0] == 'w' && strlen(str) > 1) {str++; wrap = true;}
  if (str[0] == '~') {
    int out = atoi(str +1);
    if (out == 0) {
      if (str[1] == '0') return;
      if (str[1] == '-') {
        *val = (int)(*val -1) < (int)minv ? maxv : min((int)maxv,(*val -1)); //-1, wrap around
      } else {
        *val = (int)(*val +1) > (int)maxv ? minv : max((int)minv,(*val +1)); //+1, wrap around
      }
    } else {
      if (wrap && *val == maxv && out > 0) out = minv;
      else if (wrap && *val == minv && out < 0) out = maxv;
      else {
        out += *val;
        if (out > maxv) out = maxv;
        if (out < minv) out = minv;
      }
      *val = out;
    }
    return;
  } else if (minv == maxv && minv == 0) { // limits "unset" i.e. both 0
    byte p1 = atoi(str);
    const char* str2 = strchr(str,'~'); // min/max range (for preset cycle, e.g. "1~5~")
    if (str2) {
      byte p2 = atoi(++str2);           // skip ~
      if (p2 > 0) {
        while (isdigit(*(++str2)));     // skip digits
        parseNumber(str2, val, p1, p2);
        return;
      }
    }
  }
  *val = atoi(str);
}


bool getVal(JsonVariant elem, byte* val, byte vmin, byte vmax) {
  if (elem.is<int>()) {
		if (elem < 0) return false; //ignore e.g. {"ps":-1}
    *val = elem;
    return true;
  } else if (elem.is<const char*>()) {
    const char* str = elem;
    size_t len = strnlen(str, 12);
    if (len == 0 || len > 10) return false;
    parseNumber(str, val, vmin, vmax);
    return true;
  }
  return false; //key does not exist
}


bool updateVal(const char* req, const char* key, byte* val, byte minv, byte maxv)
{
  const char *v = strstr(req, key);
  if (v) v += strlen(key);
  else return false;
  parseNumber(v, val, minv, maxv);
  return true;
}


//append a numeric setting to string buffer
void sappend(char stype, const char* key, int val)
{
  char ds[] = "d.Sf.";

  switch(stype)
  {
    case 'c': //checkbox
      oappend(ds);
      oappend(key);
      oappend(".checked=");
      oappendi(val);
      oappend(";");
      break;
    case 'v': //numeric
      oappend(ds);
      oappend(key);
      oappend(".value=");
      oappendi(val);
      oappend(";");
      break;
    case 'i': //selectedIndex
      oappend(ds);
      oappend(key);
      oappend(SET_F(".selectedIndex="));
      oappendi(val);
      oappend(";");
      break;
  }
}


//append a string setting to buffer
void sappends(char stype, const char* key, char* val)
{
  switch(stype)
  {
    case 's': {//string (we can interpret val as char*)
      String buf = val;
      //convert "%" to "%%" to make EspAsyncWebServer happy
      //buf.replace("%","%%");
      oappend("d.Sf.");
      oappend(key);
      oappend(".value=\"");
      oappend(buf.c_str());
      oappend("\";");
      break;}
    case 'm': //message
      oappend(SET_F("d.getElementsByClassName"));
      oappend(key);
      oappend(SET_F(".innerHTML=\""));
      oappend(val);
      oappend("\";");
      break;
  }
}


bool oappendi(int i)
{
  char s[16];               // WLEDMM max 32bit integer needs 11 chars (sign + 10) not 10
  snprintf(s, 15, "%d", i); // WLEDMM
  return oappend(s);
}

static bool squeezeStrings = false;
void oappendUseDeflate(bool OnOff) { squeezeStrings = OnOff; }

bool oappend(const char* txt)
{
  String str = squeezeStrings ? String(txt) : String("");
  if (squeezeStrings) {
    // simple fixed-dictionary deflate
    str.replace(F("addField("),    F("adF("));
    str.replace(F("addDropdown("), F("adD("));
    str.replace(F("addOption("),   F("adO("));
    str.replace(F("addInfo("),     F("adI("));
  }
  const char* finalTxt = squeezeStrings ? str.c_str() : txt;

  size_t len = strlen(finalTxt);
  if ((obuf == nullptr) || (olen + len >= SETTINGS_STACK_BUF_SIZE)) { // sanity checks
	  if (obuf == nullptr) { USER_PRINTLN(F("oappend() error: obuf == nullptr."));
	  } else {
	    USER_PRINT(F("oappend() error: buffer full. Increase SETTINGS_STACK_BUF_SIZE for "));
      USER_PRINTF("%2u bytes \t\"", len /*1 + olen + len - SETTINGS_STACK_BUF_SIZE*/);
      USER_PRINT(finalTxt);
      USER_PRINTLN(F("\""));
      errorFlag = ERR_LOW_AJAX_MEM;
	  }
    return false;        // buffer full
  }
  strcpy(obuf + olen, finalTxt);
  olen += len;
  return true;
}


bool startsWithWled(const char* s) {
  return strncasecmp(s, "wled", 4) == 0;
}

void prepareHostname(char* hostname) {
  const char* pC = serverDescription;

  uint8_t pos = 0;

  // Only add "wled-" if the provided name does NOT already start with "wled"
  if (!startsWithWled(pC)) {
    sprintf_P(hostname, "wled-%*s", 6, escapedMac.c_str() + 6);
    pos = 5;  // continue after "wled-"
  } else {
    hostname[0] = '\0';  // start clean
  }

  // Append sanitized characters
  while (*pC && pos < 24) {
    if (isalnum(*pC)) {
      hostname[pos++] = *pC;
    } else if (*pC == ' ' || *pC == '_' || *pC == '-' || *pC == '+' ||
      *pC == '!' || *pC == '?' || *pC == '*') {
      hostname[pos++] = '-';
    }
    pC++;
  }

  // Trim trailing hyphens
  if (pos > 0) {
    while (pos > 0 && hostname[pos - 1] == '-') pos--;
    hostname[pos] = '\0';
  }
}



bool isAsterisksOnly(const char* str, byte maxLen)
{
  for (byte i = 0; i < maxLen; i++) {
    if (str[i] == 0) break;
    if (str[i] != '*') return false;
  }
  //at this point the password contains asterisks only
  return (str[0] != 0); //false on empty string
}


//threading/network callback details: https://github.com/Aircoookie/WLED/pull/2336#discussion_r762276994
bool requestJSONBufferLock(uint8_t module)
{
  unsigned long now = millis();

  while (jsonBufferLock && millis()-now < 1100) delay(1); // wait for fraction for buffer lock

  if (jsonBufferLock) {
    USER_PRINT(F("ERROR: Locking JSON buffer failed! (still locked by "));
    USER_PRINT(jsonBufferLock);
    USER_PRINTLN(")");
    return false; // waiting time-outed
  }

  jsonBufferLock = module ? module : 255;
  DEBUG_PRINT(F("JSON buffer locked. ("));
  DEBUG_PRINT(jsonBufferLock);
  DEBUG_PRINTLN(")");
  fileDoc = &doc;  // used for applying presets (presets.cpp)
  doc.clear();
  return true;
}


void releaseJSONBufferLock()
{
  DEBUG_PRINT(F("JSON buffer released. ("));
  DEBUG_PRINT(jsonBufferLock);
  DEBUG_PRINTLN(")");
  fileDoc = nullptr;
  jsonBufferLock = 0;
}


// extracts effect mode (or palette) name from names serialized string
// caller must provide large enough buffer for name (including SR extensions)!
uint8_t extractModeName(uint8_t mode, const char *src, char *dest, uint8_t maxLen)
{
  if (src == JSON_mode_names || src == nullptr) {
    if (mode < strip.getModeCount()) {
      char lineBuffer[256] = { '\0' };
      //strcpy_P(lineBuffer, (const char*)pgm_read_dword(&(WS2812FX::_modeData[mode])));
      strncpy_P(lineBuffer, strip.getModeData(mode), sizeof(lineBuffer)/sizeof(char)-1);
      lineBuffer[sizeof(lineBuffer)/sizeof(char)-1] = '\0'; // terminate string
      size_t len = strlen(lineBuffer);
      size_t j = 0;
      for (; j < maxLen && j < len; j++) {
        if (lineBuffer[j] == '\0' || lineBuffer[j] == '@') break;
        dest[j] = lineBuffer[j];
      }
      dest[j] = 0; // terminate string
      return strlen(dest);
    } else return 0;
  }

  if (src == JSON_palette_names && mode > (GRADIENT_PALETTE_COUNT + 13)) {
    snprintf_P(dest, maxLen, PSTR("~ Custom %d ~"), 255-mode);
    dest[maxLen-1] = '\0';
    return strlen(dest);
  }

  uint8_t qComma = 0;
  bool insideQuotes = false;
  uint8_t printedChars = 0;
  char singleJsonSymbol;
  size_t len = strlen_P(src);

  // Find the mode name in JSON
  for (size_t i = 0; i < len; i++) {
    singleJsonSymbol = pgm_read_byte_near(src + i);
    if (singleJsonSymbol == '\0') break;
    if (singleJsonSymbol == '@' && insideQuotes && qComma == mode) break; //stop when SR extension encountered
    switch (singleJsonSymbol) {
      case '"':
        insideQuotes = !insideQuotes;
        break;
      case '[':
      case ']':
        break;
      case ',':
        if (!insideQuotes) qComma++;
      default:
        if (!insideQuotes || (qComma != mode)) break;
        dest[printedChars++] = singleJsonSymbol;
    }
    if ((qComma > mode) || (printedChars >= maxLen)) break;
  }
  dest[printedChars] = '\0';
  return strlen(dest);
}


// extracts effect slider data (1st group after @)
uint8_t extractModeSlider(uint8_t mode, uint8_t slider, char *dest, uint8_t maxLen, uint8_t *var)
{
  dest[0] = '\0'; // start by clearing buffer

  if (mode < strip.getModeCount()) {
    String lineBuffer = FPSTR(strip.getModeData(mode));
    if (lineBuffer.length() > 0) {
      int16_t start = lineBuffer.indexOf('@');
      int16_t stop  = lineBuffer.indexOf(';', start);
      if (start>0 && stop>0) {
        String names = lineBuffer.substring(start, stop); // include @
        int16_t nameBegin = 1, nameEnd, nameDefault;
        if (slider < 10) {
          for (size_t i=0; i<=slider; i++) {
            const char *tmpstr;
            dest[0] = '\0'; //clear dest buffer
            if (nameBegin == 0) break; // there are no more names
            nameEnd = names.indexOf(',', nameBegin);
            if (i == slider) {
              nameDefault = names.indexOf('=', nameBegin); // find default value
              if (nameDefault > 0 && var && ((nameEnd>0 && nameDefault<nameEnd) || nameEnd<0)) {
                *var = (uint8_t)atoi(names.substring(nameDefault+1).c_str());
              }
              if (names.charAt(nameBegin) == '!') {
                switch (slider) {
                  case  0: tmpstr = PSTR("FX Speed");     break;
                  case  1: tmpstr = PSTR("FX Intensity"); break;
                  case  2: tmpstr = PSTR("FX Custom 1");  break;
                  case  3: tmpstr = PSTR("FX Custom 2");  break;
                  case  4: tmpstr = PSTR("FX Custom 3");  break;
                  default: tmpstr = PSTR("FX Custom");    break;
                }
                strncpy_P(dest, tmpstr, maxLen); // copy the name into buffer (replacing previous)
                dest[maxLen-1] = '\0';
              } else {
                if (nameEnd<0) tmpstr = names.substring(nameBegin).c_str(); // did not find ",", last name?
                else           tmpstr = names.substring(nameBegin, nameEnd).c_str();
                strlcpy(dest, tmpstr, maxLen); // copy the name into buffer (replacing previous)
              }
            }
            nameBegin = nameEnd+1; // next name (if "," is not found it will be 0)
          } // next slider
        } else if (slider == 255) {
          // palette
          strlcpy(dest, "pal", maxLen);
          names = lineBuffer.substring(stop+1); // stop has index of color slot names
          nameBegin = names.indexOf(';'); // look for palette
          if (nameBegin >= 0) {
            nameEnd = names.indexOf(';', nameBegin+1);
            if (!isdigit(names[nameBegin+1])) nameBegin = names.indexOf('=', nameBegin+1); // look for default value
            if (nameEnd >= 0 && nameBegin > nameEnd) nameBegin = -1;
            if (nameBegin >= 0 && var) {
              *var = (uint8_t)atoi(names.substring(nameBegin+1).c_str());
            }
          }
        }
        // we have slider name (including default value) in the dest buffer
        for (size_t i=0; i<strlen(dest); i++) if (dest[i]=='=') { dest[i]='\0'; break; } // truncate default value

      } else {
        // defaults to just speed and intensity since there is no slider data
        switch (slider) {
          case 0:  strncpy_P(dest, PSTR("FX Speed"), maxLen); break;
          case 1:  strncpy_P(dest, PSTR("FX Intensity"), maxLen); break;
        }
        dest[maxLen] = '\0'; // strncpy does not necessarily null terminate string
      }
    }
    return strlen(dest);
  }
  return 0;
}


// extracts mode parameter defaults from last section of mode data (e.g. "Juggle@!,Trail;!,!,;!;sx=16,ix=240,1d")
int16_t extractModeDefaults(uint8_t mode, const char *segVar)
{
  if (mode < strip.getModeCount()) {
    char lineBuffer[256] = { '\0' };
    strncpy_P(lineBuffer, strip.getModeData(mode), sizeof(lineBuffer)/sizeof(char)-1);
    lineBuffer[sizeof(lineBuffer)/sizeof(char)-1] = '\0'; // terminate string
    if (lineBuffer[0] != 0) {
      char* startPtr = strrchr(lineBuffer, ';'); // last ";" in FX data
      if (!startPtr) return -1;

      char* stopPtr = strstr(startPtr, segVar);
      if (!stopPtr) return -1;

      stopPtr += strlen(segVar) +1; // skip "="
      return atoi(stopPtr);
    }
  }
  return -1;
}


void checkSettingsPIN(const char* pin) {
  if (!pin) return;
  if (!correctPIN && millis() - lastEditTime < PIN_RETRY_COOLDOWN) return; // guard against PIN brute force
  bool correctBefore = correctPIN;
  correctPIN = (strlen(settingsPIN) == 0 || strncmp(settingsPIN, pin, 4) == 0);
  if (correctBefore != correctPIN) createEditHandler(correctPIN);
  lastEditTime = millis();
}


uint16_t crc16(const unsigned char* data_p, size_t length) {
  uint8_t x;
  uint16_t crc = 0xFFFF;
  if (!length) return 0x1D0F;
  while (length--) {
    x = crc >> 8 ^ *data_p++;
    x ^= x>>4;
    crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x <<5)) ^ ((uint16_t)x);
  }
  return crc;
}

// fastled beatsin: 1:1 replacements to remove the use of fastled sin16()
// Generates a 16-bit sine wave at a given BPM that oscillates within a given range. see fastled for details.
uint16_t beatsin88_t(accum88 beats_per_minute_88, uint16_t lowest, uint16_t highest, uint32_t timebase, uint16_t phase_offset)
{
    uint16_t beat = beat88( beats_per_minute_88, timebase);
    uint16_t beatsin (sin16_t( beat + phase_offset) + 32768);
    uint16_t rangewidth = highest - lowest;
    uint16_t scaledbeat = scale16( beatsin, rangewidth);
    uint16_t result = lowest + scaledbeat;
    return result;
}

// Generates a 16-bit sine wave at a given BPM that oscillates within a given range. see fastled for details.
uint16_t beatsin16_t(accum88 beats_per_minute, uint16_t lowest, uint16_t highest, uint32_t timebase, uint16_t phase_offset)
{
    uint16_t beat = beat16( beats_per_minute, timebase);
    uint16_t beatsin = (sin16_t( beat + phase_offset) + 32768);
    uint16_t rangewidth = highest - lowest;
    uint16_t scaledbeat = scale16( beatsin, rangewidth);
    uint16_t result = lowest + scaledbeat;
    return result;
}

// Generates an 8-bit sine wave at a given BPM that oscillates within a given range. see fastled for details.
uint8_t beatsin8_t(accum88 beats_per_minute, uint8_t lowest, uint8_t highest, uint32_t timebase, uint8_t phase_offset)
{
    uint8_t beat = beat8( beats_per_minute, timebase);
    uint8_t beatsin = sin8_t( beat + phase_offset);
    uint8_t rangewidth = highest - lowest;
    uint8_t scaledbeat = scale8( beatsin, rangewidth);
    uint8_t result = lowest + scaledbeat;
    return result;
}

///////////////////////////////////////////////////////////////////////////////
// Begin simulateSound (to enable audio enhanced effects to display something)
///////////////////////////////////////////////////////////////////////////////
// Currently 4 types defined, to be fine tuned and new types added
// (only 2 used as stored in 1 bit in segment options, consider switching to a single global simulation type)
typedef enum UM_SoundSimulations {
  UMS_BeatSin = 0,
  UMS_WeWillRockYou,
  UMS_10_13,
  UMS_14_3
} um_soundSimulations_t;

um_data_t* simulateSound(uint8_t simulationId)
{
  static uint8_t samplePeak;
  static float   FFT_MajorPeak;
  static uint8_t maxVol;
  static uint8_t binNum;

  static float    volumeSmth;
  static uint16_t volumeRaw;
  static float    my_magnitude;
  static uint16_t zeroCrossingCount = 0; // number of zero crossings in the current batch of 512 samples

  //arrays
  uint8_t *fftResult;

  static um_data_t* um_data = nullptr;

  if (!um_data) {
    //claim storage for arrays
    fftResult = (uint8_t *)malloc(sizeof(uint8_t) * 16);

    // initialize um_data pointer structure
    // NOTE!!!
    // This may change as AudioReactive usermod may change
    um_data = new um_data_t;
    um_data->u_size = 12;
    um_data->u_type = new um_types_t[um_data->u_size];
    um_data->u_data = new void*[um_data->u_size];
    um_data->u_data[0] = &volumeSmth;
    um_data->u_data[1] = &volumeRaw;
    um_data->u_data[2] = fftResult;
    um_data->u_data[3] = &samplePeak;
    um_data->u_data[4] = &FFT_MajorPeak;
    um_data->u_data[5] = &my_magnitude;
    um_data->u_data[6] = &maxVol;
    um_data->u_data[7] = &binNum;
    um_data->u_data[8]  = &FFT_MajorPeak; // dummy (FFT Peak smoothed)
    um_data->u_data[9]  = &volumeSmth;    // dummy (soundPressure)
    um_data->u_data[10] = &volumeSmth;    // dummy (agcSensitivity)
    um_data->u_data[11] = &zeroCrossingCount;
  } else {
    // get arrays from um_data
    fftResult =  (uint8_t*)um_data->u_data[2];
  }

  uint32_t ms = millis();

  switch (simulationId) {
    default:
    case UMS_BeatSin:
      for (int i = 0; i<16; i++)
        fftResult[i] = beatsin8_t(120 / (i+1), 0, 255);
        // fftResult[i] = (beatsin8_t(120, 0, 255) + (256/16 * i)) % 256;
        volumeSmth = fftResult[8];
      break;
    case UMS_WeWillRockYou:
      if (ms%2000 < 200) {
        volumeSmth = random8(255);
        for (int i = 0; i<5; i++)
          fftResult[i] = random8(255);
      }
      else if (ms%2000 < 400) {
        volumeSmth = 0;
        for (int i = 0; i<16; i++)
          fftResult[i] = 0;
      }
      else if (ms%2000 < 600) {
        volumeSmth = random8(255);
        for (int i = 5; i<11; i++)
          fftResult[i] = random8(255);
      }
      else if (ms%2000 < 800) {
        volumeSmth = 0;
        for (int i = 0; i<16; i++)
          fftResult[i] = 0;
      }
      else if (ms%2000 < 1000) {
        volumeSmth = random8(255);
        for (int i = 11; i<16; i++)
          fftResult[i] = random8(255);
      }
      else {
        volumeSmth = 0;
        for (int i = 0; i<16; i++)
          fftResult[i] = 0;
      }
      break;
    case UMS_10_13:
      for (int i = 0; i<16; i++)
        fftResult[i] = inoise8(beatsin8_t(90 / (i+1), 0, 200)*15 + (ms>>10), ms>>3);
        volumeSmth = fftResult[8];
      break;
    case UMS_14_3:
      for (int i = 0; i<16; i++)
        fftResult[i] = inoise8(beatsin8_t(120 / (i+1), 10, 30)*10 + (ms>>14), ms>>3);
      volumeSmth = fftResult[8];
      break;
  }

  samplePeak    = random8() > 250;
  FFT_MajorPeak = 21 + (volumeSmth*volumeSmth) / 8.0f; // walk thru full range of 21hz...8200hz
  maxVol        = 31;  // this gets feedback fro UI
  binNum        = 8;   // this gets feedback fro UI
  volumeRaw = volumeSmth;
  my_magnitude = 10000.0f / 8.0f; //no idea if 10000 is a good value for FFT_Magnitude ???
  if (volumeSmth < 1 ) my_magnitude = 0.001f;             // noise gate closed - mute
  zeroCrossingCount = floorf(FFT_MajorPeak / 36.0f); // 9Khz max frequency => 255 zero crossings

  return um_data;
}

//WLEDMM enumerateLedmaps moved to FX_fcn.cpp

//WLEDMM netmindz ar palette
CRGB getCRGBForBand(int x, uint8_t *fftResult, int pal) { 
  CRGB value;
  CHSV hsv;
  if(pal == 71) { // bit hacky to use palette id here, but don't want to litter the code with lots of different methods. TODO: add enum for palette creation type
    if(x == 1) {
      value = CRGB(fftResult[10]/2, fftResult[4]/2, fftResult[0]/2);
    }
    else if(x == 255) {
      value = CRGB(fftResult[10]/2, fftResult[0]/2, fftResult[4]/2);
    } 
    else {
      value = CRGB(fftResult[0]/2, fftResult[4]/2, fftResult[10]/2);
    } 
  }
  else if(pal == 72) {
    int b = map(x, 1, 255, 0, 10); // convert palette position to lower half of freq band
    hsv = CHSV(fftResult[b], 255, map(fftResult[b], 0, 255, 30, 255));  // pick hue
    hsv2rgb_rainbow(hsv, value);  // convert to R,G,B
  }
  else if(pal == 73) {
    int b = map(x, 0, 255, 0, 8); // convert palette position to lower half of freq band
    hsv = CHSV(uint8_t(fftResult[b]), 255, x);
    hsv2rgb_rainbow(hsv, value);  // convert to R,G,B
  }

  return value;
}

/*
 * Returns a new, random color wheel index with a minimum distance of 42 from pos.
 */
uint8_t get_random_wheel_index(uint8_t pos) {
  uint8_t r = 0, x = 0, y = 0, d = 0;
  while (d < 42) {
    r = random8();
    x = abs(pos - r);
    y = 255 - x;
    d = MIN(x, y);
  }
  return r;
}

String strip_unicode(const String& name) {
  String clean;
  clean.reserve(name.length()); // pre‑allocate for efficiency

  // Step 1: keep only ASCII printable characters (32..126)
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if ((uint8_t)c >= 32 && (uint8_t)c < 127) {
      clean += c;
    }
  }

  // Step 2: remove newlines, carriage returns, tabs
  clean.replace("\n", "");
  clean.replace("\r", "");
  clean.replace("\t", "");

  // Step 3: trim leading/trailing spaces
  // Arduino String has no built‑in trim, so implement manually
  int start = 0;
  while (start < clean.length() && isspace((unsigned char)clean[start])) {
    start++;
  }
  int end = clean.length() - 1;
  while (end >= start && isspace((unsigned char)clean[end])) {
    end--;
  }

  if (start > 0 || end < (int)clean.length() - 1) {
    clean = clean.substring(start, end + 1);
  }

  return clean;
}

// WLEDMM extended "trim string" function to support enumerateLedmaps
// The function takes char* as input, and removes all leading and trailing "decorations" like spaces, tabs, line endings, quotes, colons
// The conversion is "in place" (destructive).
// example: cleanUpName("\t \"Ring241x 60/9 squeeze \" ,\r") returns "Ring241x 60/9 squeeze"
// 
// Null pointer and zero size "C strings" are handled correctly.
// Will not work with flash strings. Unicode encoded multi-byte char strings may get corrupted.
//
static const char *unwantedChars = "\r\n\t\b ,;:\"\'`´\\"; // list of chars to delete
//
char *cleanUpName(char *in) {
  if (nullptr == in) return(in);
  size_t len = strlen(in);
  if (len == 0) return(in);

  // delete trailing garbage
  while ((len > 0) && (strchr(unwantedChars, in[len-1]) != nullptr)) {
    in[len-1] = '\0'; // deletes last char
    len--;
  }
  // delete leading garbage
  while ((len > 0) && (strchr(unwantedChars, in[0]) != nullptr)) {
    (void) memmove(in, in+1, len); // shifts string left by one
    len--;
  }
  
  return(in);
}

// 32 bit hardware random number generator, inlining uses more code, use hw_random16() if speed is critical (see fcn_declare.h)
uint32_t hw_random(uint32_t upperlimit) {
  uint32_t rnd = hw_random();
  uint64_t scaled = uint64_t(rnd) * uint64_t(upperlimit);
  return scaled >> 32;
}

int32_t hw_random(int32_t lowerlimit, int32_t upperlimit) {
  if (lowerlimit >= upperlimit) {
    return lowerlimit;
  }
  uint32_t diff = upperlimit - lowerlimit;
  return hw_random(diff) + lowerlimit;
}

void dumpAllTaskHWMs(void) {
  UBaseType_t num = uxTaskGetNumberOfTasks();
  TaskStatus_t* tasks = (TaskStatus_t*)calloc(num, sizeof(TaskStatus_t));
  if (!tasks) return;

  UBaseType_t count = uxTaskGetSystemState(tasks, num, NULL);

  USER_PRINT("\n=== Task High Water Marks ===\n");

  for (UBaseType_t i = 0; i < count; i++) {
    const char* name = tasks[i].pcTaskName;
    UBaseType_t hwm = tasks[i].usStackHighWaterMark;

    USER_PRINTF("%-16s HWM: %u words (%u bytes)\n",
      name,
      (unsigned)hwm,
      (unsigned)(hwm * 4));
  }

  USER_PRINT("==============================\n\n");

  free(tasks);
}

/*
 * Fixed point integer based Perlin noise functions by @dedehai
 * Note: optimized for speed and to mimic fastled inoise functions, not for accuracy or best randomness
 */
#define PERLIN_SHIFT 1

 // calculate gradient for corner from hash value
static inline __attribute__((always_inline)) int32_t hashToGradient(uint32_t h) {
  // using more steps yields more "detailed" perlin noise but looks less like the original fastled version (adjust PERLIN_SHIFT to compensate, also changes range and needs proper adustment)
  // return (h & 0xFF) - 128; // use PERLIN_SHIFT 7
  // return (h & 0x0F) - 8; // use PERLIN_SHIFT 3
  // return (h & 0x07) - 4; // use PERLIN_SHIFT 2
  return (h & 0x03) - 2; // use PERLIN_SHIFT 1 -> closest to original fastled version
}

// Gradient functions for 1D, 2D and 3D Perlin noise  note: forcing inline produces smaller code and makes it 3x faster!
static inline __attribute__((always_inline)) int32_t gradient1D(uint32_t x0, int32_t dx) {
  uint32_t h = x0 * 0x27D4EB2D;
  h ^= h >> 15;
  h *= 0x92C3412B;
  h ^= h >> 13;
  h ^= h >> 7;
  return (hashToGradient(h) * dx) >> PERLIN_SHIFT;
}

static inline __attribute__((always_inline)) int32_t gradient2D(uint32_t x0, int32_t dx, uint32_t y0, int32_t dy) {
  uint32_t h = (x0 * 0x27D4EB2D) ^ (y0 * 0xB5297A4D);
  h ^= h >> 15;
  h *= 0x92C3412B;
  h ^= h >> 13;
  return (hashToGradient(h) * dx + hashToGradient(h >> PERLIN_SHIFT) * dy) >> (1 + PERLIN_SHIFT);
}

static inline __attribute__((always_inline)) int32_t gradient3D(uint32_t x0, int32_t dx, uint32_t y0, int32_t dy, uint32_t z0, int32_t dz) {
  // fast and good entropy hash from corner coordinates
  uint32_t h = (x0 * 0x27D4EB2D) ^ (y0 * 0xB5297A4D) ^ (z0 * 0x1B56C4E9);
  h ^= h >> 15;
  h *= 0x92C3412B;
  h ^= h >> 13;
  return ((hashToGradient(h) * dx + hashToGradient(h >> (1 + PERLIN_SHIFT)) * dy + hashToGradient(h >> (1 + 2 * PERLIN_SHIFT)) * dz) * 85) >> (8 + PERLIN_SHIFT); // scale to 16bit, x*85 >> 8 = x/3
}

// fast cubic smoothstep: t*(3 - 2t²), optimized for fixed point, scaled to avoid overflows
static uint32_t smoothstep(const uint32_t t) {
  uint32_t t_squared = (t * t) >> 16;
  uint32_t factor = (3 << 16) - ((t << 1));
  return (t_squared * factor) >> 18; // scale to avoid overflows and give best resolution
}

// simple linear interpolation for fixed-point values, scaled for perlin noise use
static inline int32_t lerpPerlin(int32_t a, int32_t b, int32_t t) {
  return a + (((b - a) * t) >> 14); // match scaling with smoothstep to yield 16.16bit values
}

// 1D Perlin noise function that returns a value in range of -24691 to 24689
int32_t IRAM_ATTR_YN perlin1D_raw(uint32_t x, bool is16bit) {
  // integer and fractional part coordinates
  int32_t x0 = x >> 16;
  int32_t x1 = x0 + 1;
  if (is16bit) x1 = x1 & 0xFF; // wrap back to zero at 0xFF instead of 0xFFFF

  int32_t dx0 = x & 0xFFFF;
  int32_t dx1 = dx0 - 0x10000;
  // gradient values for the two corners
  int32_t g0 = gradient1D(x0, dx0);
  int32_t g1 = gradient1D(x1, dx1);
  // interpolate and smooth function
  int32_t tx = smoothstep(dx0);
  int32_t noise = lerpPerlin(g0, g1, tx);
  return noise;
}

// 2D Perlin noise function that returns a value in range of -20633 to 20629
int32_t IRAM_ATTR_YN perlin2D_raw(uint32_t x, uint32_t y, bool is16bit) {
  int32_t x0 = x >> 16;
  int32_t y0 = y >> 16;
  int32_t x1 = x0 + 1;
  int32_t y1 = y0 + 1;

  if (is16bit) {
    x1 = x1 & 0xFF; // wrap back to zero at 0xFF instead of 0xFFFF
    y1 = y1 & 0xFF;
  }

  int32_t dx0 = x & 0xFFFF;
  int32_t dy0 = y & 0xFFFF;
  int32_t dx1 = dx0 - 0x10000;
  int32_t dy1 = dy0 - 0x10000;

  int32_t g00 = gradient2D(x0, dx0, y0, dy0);
  int32_t g10 = gradient2D(x1, dx1, y0, dy0);
  int32_t g01 = gradient2D(x0, dx0, y1, dy1);
  int32_t g11 = gradient2D(x1, dx1, y1, dy1);

  uint32_t tx = smoothstep(dx0);
  uint32_t ty = smoothstep(dy0);

  int32_t nx0 = lerpPerlin(g00, g10, tx);
  int32_t nx1 = lerpPerlin(g01, g11, tx);

  int32_t noise = lerpPerlin(nx0, nx1, ty);
  return noise;
}

// 3D Perlin noise function that returns a value in range of -16788 to 16381
int32_t IRAM_ATTR_YN perlin3D_raw(uint32_t x, uint32_t y, uint32_t z, bool is16bit) {
  int32_t x0 = x >> 16;
  int32_t y0 = y >> 16;
  int32_t z0 = z >> 16;
  int32_t x1 = x0 + 1;
  int32_t y1 = y0 + 1;
  int32_t z1 = z0 + 1;

  if (is16bit) {
    x1 = x1 & 0xFF; // wrap back to zero at 0xFF instead of 0xFFFF
    y1 = y1 & 0xFF;
    z1 = z1 & 0xFF;
  }

  int32_t dx0 = x & 0xFFFF;
  int32_t dy0 = y & 0xFFFF;
  int32_t dz0 = z & 0xFFFF;
  int32_t dx1 = dx0 - 0x10000;
  int32_t dy1 = dy0 - 0x10000;
  int32_t dz1 = dz0 - 0x10000;

  int32_t g000 = gradient3D(x0, dx0, y0, dy0, z0, dz0);
  int32_t g001 = gradient3D(x0, dx0, y0, dy0, z1, dz1);
  int32_t g010 = gradient3D(x0, dx0, y1, dy1, z0, dz0);
  int32_t g011 = gradient3D(x0, dx0, y1, dy1, z1, dz1);
  int32_t g100 = gradient3D(x1, dx1, y0, dy0, z0, dz0);
  int32_t g101 = gradient3D(x1, dx1, y0, dy0, z1, dz1);
  int32_t g110 = gradient3D(x1, dx1, y1, dy1, z0, dz0);
  int32_t g111 = gradient3D(x1, dx1, y1, dy1, z1, dz1);

  uint32_t tx = smoothstep(dx0);
  uint32_t ty = smoothstep(dy0);
  uint32_t tz = smoothstep(dz0);

  int32_t nx0 = lerpPerlin(g000, g100, tx);
  int32_t nx1 = lerpPerlin(g010, g110, tx);
  int32_t nx2 = lerpPerlin(g001, g101, tx);
  int32_t nx3 = lerpPerlin(g011, g111, tx);
  int32_t ny0 = lerpPerlin(nx0, nx1, ty);
  int32_t ny1 = lerpPerlin(nx2, nx3, ty);

  int32_t noise = lerpPerlin(ny0, ny1, tz);
  return noise;
}

// scaling functions for fastled replacement
uint16_t perlin16(uint32_t x) {
  return ((perlin1D_raw(x) * 1159) >> 10) + 32803; //scale to 16bit and offset (fastled range: about 4838 to 60766)
}

uint16_t perlin16(uint32_t x, uint32_t y) {
  return ((perlin2D_raw(x, y) * 1537) >> 10) + 32725; //scale to 16bit and offset (fastled range: about 1748 to 63697)
}

uint16_t perlin16(uint32_t x, uint32_t y, uint32_t z) {
  return ((perlin3D_raw(x, y, z) * 1731) >> 10) + 33147; //scale to 16bit and offset (fastled range: about 4766 to 60840)
}

uint8_t perlin8(uint16_t x) {
  return (((perlin1D_raw((uint32_t)x << 8, true) * 1353) >> 10) + 32769) >> 8; //scale to 16 bit, offset, then scale to 8bit
}

uint8_t perlin8(uint16_t x, uint16_t y) {
  return (((perlin2D_raw((uint32_t)x << 8, (uint32_t)y << 8, true) * 1620) >> 10) + 32771) >> 8; //scale to 16 bit, offset, then scale to 8bit
}

uint8_t perlin8(uint16_t x, uint16_t y, uint16_t z) {
  return (((perlin3D_raw((uint32_t)x << 8, (uint32_t)y << 8, (uint32_t)z << 8, true) * 2015) >> 10) + 33168) >> 8; //scale to 16 bit, offset, then scale to 8bit
}