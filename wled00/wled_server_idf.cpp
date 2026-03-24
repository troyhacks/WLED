// wled_server_idf.cpp — HTTP server initialisation for pure ESP-IDF builds.
// Compiled only when WLED_IDF_BUILD is defined.
// Provides: initServer(), createEditHandler(), handleFileRead()
//
// All ESPAsyncWebServer API calls are backed by the AsyncWebServer.h shim in
// wled00/idf_shims/.  Route logic mirrors wled_server.cpp but using only the
// subset of APIs available in the IDF build.

#ifdef WLED_IDF_BUILD

#include "wled.h"

// HTML page headers (gzip-compressed byte arrays)
#include "html_ui.h"
#ifdef WLED_ENABLE_SIMPLE_UI
  #include "html_simple.h"
#endif
#include "html_settings.h"
#include "html_other.h"
#ifdef WLED_ENABLE_PIXART
  #include "html_pixart.h"
#endif
#include "html_cpal.h"

#include "src/dependencies/json/AsyncJson-v6.h"

// ── Flash string constants ────────────────────────────────────────────────────
static const char s_redirecting[] PROGMEM = "Redirecting...";
static const char s_content_enc[] PROGMEM = "Content-Encoding";
static const char s_unlock_ota [] PROGMEM = "Please unlock OTA in security settings!";
static const char s_unlock_cfg [] PROGMEM = "Please unlock settings using PIN code!";

// ── IDF-only macros ───────────────────────────────────────────────────────────
// ON_STA_FILTER: true when request arrives on STA interface (not AP).
// In IDF build we approximate: if apActive then requests on AP side are likely
// captive-portal candidates; on STA side they are not.
// Since we can't inspect the source IP easily here, use the apActive flag only.
#ifndef ON_STA_FILTER
#define ON_STA_FILTER(req) (!apActive)
#endif

// ── Forward declarations ──────────────────────────────────────────────────────
// File-local helpers (not declared in fcn_declare.h):
static bool handleIfNoneMatchCacheHeader(AsyncWebServerRequest* request);
static void setStaticContentCacheHeaders(AsyncWebServerResponse* response);

// Matches extern declaration in fcn_declare.h:
bool isIp(String str);

// These match extern declarations in fcn_declare.h (must not be static):
bool   captivePortal(AsyncWebServerRequest* request);
void   serveIndex(AsyncWebServerRequest* request);
void   serveIndexOrWelcome(AsyncWebServerRequest* request);
void   serveSettingsJS(AsyncWebServerRequest* request);
String msgProcessor(const String& var);
// handleUpload defined at bottom of this file
void handleUpload(AsyncWebServerRequest* request, const String& filename,
                  size_t index, uint8_t* data, size_t len, bool final);

// ── String constants persisted across this file ───────────────────────────────
// (messageHead/messageSub/optionType live in wled.h globals)

// ── isIp ──────────────────────────────────────────────────────────────────────
bool isIp(String str) {
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9')) return false;
    }
    return true;
}

// ── Content-type helper ───────────────────────────────────────────────────────
static String getContentType(const String& path) {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".gif"))  return "image/gif";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".gz"))   return "application/x-gzip";
    if (path.endsWith(".xml"))  return "text/xml";
    if (path.endsWith(".txt"))  return "text/plain";
    if (path.endsWith(".wav"))  return "audio/wav";
    return "application/octet-stream";
}

// ── handleFileRead — IDF build (reads from LittleFS via POSIX) ────────────────
bool handleFileRead(AsyncWebServerRequest* request, String path) {
    DEBUG_PRINTLN("WS FileRead: " + path);
    if (path.endsWith("/")) path += "index.htm";
    if (path.indexOf("sec") > -1) return false;

    // Build absolute POSIX path under LittleFS mount point
    String abspath = String(LITTLEFS_BASE) + path;

    // Try <path>.gz first
    String abspathGz = abspath + ".gz";
    FILE* fp = fopen(abspathGz.c_str(), "rb");
    bool isGzip = (fp != nullptr);
    if (!fp) fp = fopen(abspath.c_str(), "rb");

    if (!fp) {
        return false;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) { fclose(fp); return false; }

    // Set headers
    DefaultHeaders::Instance().applyTo(request->_req);
    request->_setStatus(200);
    String ct = getContentType(path);  // use logical path for content-type
    httpd_resp_set_type(request->_req, ct.c_str());
    if (isGzip) {
        httpd_resp_set_hdr(request->_req, "Content-Encoding", "gzip");
    }
    if (request->hasArg(F("download"))) {
        httpd_resp_set_hdr(request->_req, "Content-Disposition", "attachment");
    }

    // Send in chunks
    const size_t CHUNK = 4096;
    uint8_t* buf = (uint8_t*)malloc(CHUNK);
    if (!buf) { fclose(fp); return false; }

    size_t remaining = (size_t)fsize;
    while (remaining > 0) {
        size_t toRead = (remaining < CHUNK) ? remaining : CHUNK;
        size_t n = fread(buf, 1, toRead, fp);
        if (n == 0) break;
        httpd_resp_send_chunk(request->_req, reinterpret_cast<const char*>(buf), (int)n);
        remaining -= n;
    }
    httpd_resp_send_chunk(request->_req, nullptr, 0);  // signal end
    free(buf);
    fclose(fp);
    return true;
}

// ── captivePortal ─────────────────────────────────────────────────────────────
bool captivePortal(AsyncWebServerRequest* request) {
    if (ON_STA_FILTER(request)) return false;
    if (!request->hasHeader("Host")) return false;
    AsyncWebHeader* hdr = request->getHeader("Host");
    String hostH = hdr ? hdr->value() : String();
    if (!isIp(hostH) && hostH.indexOf("wled.me") < 0 && hostH.indexOf(cmDNS) < 0) {
        DEBUG_PRINTLN("Captive portal");
        AsyncWebServerResponse* response = request->beginResponse(302);
        response->addHeader(F("Location"), "http://" + Network.softAPIP().toString() + "/");
        request->send(response);
        delete response;
        return true;
    }
    return false;
}

// ── Cache helpers ─────────────────────────────────────────────────────────────
static bool handleIfNoneMatchCacheHeader(AsyncWebServerRequest* request) {
    AsyncWebHeader* header = request->getHeader("If-None-Match");
    if (header && header->value() == String(VERSION)) {
        request->send(304);
        return true;
    }
    return false;
}

static void setStaticContentCacheHeaders(AsyncWebServerResponse* response) {
    char tmp[12];
#ifndef WLED_DEBUG
    response->addHeader(F("Cache-Control"), "no-cache");
#else
    response->addHeader(F("Cache-Control"), "no-store,max-age=0");
#endif
    sprintf_P(tmp, PSTR("%8d-%02x"), VERSION, cacheInvalidate);
    response->addHeader(F("ETag"), tmp);
}

// ── msgProcessor ──────────────────────────────────────────────────────────────
String msgProcessor(const String& var) {
    if (var == "MSG") {
        String messageBody = messageHead;
        messageBody += F("</h2>");
        messageBody += messageSub;
        uint32_t optt = optionType;
        if      (optt < 60)  { messageBody += F("<script>setTimeout(RS,"); messageBody += String(optt*1000); messageBody += F(")</script>"); }
        else if (optt < 120) { /* redirect back — unused */ }
        else if (optt < 180) { messageBody += F("<script>setTimeout(RP,"); messageBody += String((optt-120)*1000); messageBody += F(")</script>"); }
        else if (optt == 253) { messageBody += F("<br><br><form action=/settings><button class=\"bt\" type=submit>Back</button></form>"); }
        else if (optt == 254) { messageBody += F("<br><br><button type=\"button\" class=\"bt\" onclick=\"B()\">Back</button>"); }
        return messageBody;
    }
    return String();
}

// ── serveMessage ──────────────────────────────────────────────────────────────
void serveMessage(AsyncWebServerRequest* request, uint16_t code,
                  const String& headl, const String& subl, byte optionT) {
    messageHead = headl;
    messageSub  = subl;
    optionType  = optionT;
    request->send_P(code, "text/html", PAGE_msg, msgProcessor);
}

// ── serveIndex ────────────────────────────────────────────────────────────────
void serveIndex(AsyncWebServerRequest* request) {
    if (handleFileRead(request, "/index.htm")) return;
    if (handleIfNoneMatchCacheHeader(request)) return;

    AsyncWebServerResponse* response;
#ifdef WLED_ENABLE_SIMPLE_UI
    if (simplifiedUI)
        response = request->beginResponse_P(200, "text/html", PAGE_simple, PAGE_simple_L);
    else
#endif
        response = request->beginResponse_P(200, "text/html", PAGE_index, PAGE_index_L);

    response->addHeader(FPSTR(s_content_enc), "gzip");
    setStaticContentCacheHeaders(response);
    request->send(response);
    delete response;
}

// ── serveIndexOrWelcome ───────────────────────────────────────────────────────
void serveIndexOrWelcome(AsyncWebServerRequest* request) {
#if defined(WLED_USE_ETHERNET_ONLY)
    showWelcomePage = false;
#endif
    if (!showWelcomePage) serveIndex(request);
    else                  serveSettings(request);
}

// ── serveSettingsJS ───────────────────────────────────────────────────────────
void serveSettingsJS(AsyncWebServerRequest* request) {
    size_t bufSize = SETTINGS_STACK_BUF_SIZE;
    char* buf = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char*)malloc(bufSize);
    if (!buf) { request->send(500, "text/plain", "OOM: Settings buffer"); return; }
    buf[0] = 0;

    byte subPage = request->arg(F("p")).toInt();
    if (subPage > 10) {
        strcpy_P(buf, PSTR("alert('Page not implemented.');"));
        request->send(501, "application/javascript", buf);
        free(buf);
        return;
    }
    if (subPage > 0 && !correctPIN && strlen(settingsPIN) > 0) {
        strcpy_P(buf, PSTR("alert('PIN incorrect.');"));
        request->send(403, "application/javascript", buf);
        free(buf);
        return;
    }
    strcat_P(buf, PSTR("function GetV(){var d=document;"));
    getSettingsJS(request, subPage, buf + strlen(buf));
    strcat_P(buf, PSTR("}"));

    AsyncWebServerResponse* response = request->beginResponse(200, "application/javascript", String(buf));
    response->addHeader(F("Cache-Control"), "no-store");
    response->addHeader(F("Expires"), "0");
    request->send(response);
    delete response;
    free(buf);
}

// ── serveSettings ─────────────────────────────────────────────────────────────
void serveSettings(AsyncWebServerRequest* request, bool post) {
    byte subPage = 0, originalSubPage = 0;
    const String& url = request->url();

    if (url.indexOf("sett") >= 0) {
        if      (url.indexOf(".js")  > 0) subPage = 254;
        else if (url.indexOf(".css") > 0) subPage = 253;
        else if (url.indexOf("wifi") > 0) subPage = 1;
        else if (url.indexOf("leds") > 0) subPage = 2;
        else if (url.indexOf("ui")   > 0) subPage = 3;
        else if (url.indexOf("sync") > 0) subPage = 4;
        else if (url.indexOf("time") > 0) subPage = 5;
        else if (url.indexOf("sec")  > 0) subPage = 6;
        else if (url.indexOf("dmx")  > 0) subPage = 7;
        else if (url.indexOf("um")   > 0) subPage = 8;
        else if (url.indexOf("2D")   > 0) subPage = 10;
        else if (url.indexOf("lock") > 0) subPage = 251;
    } else if (url.indexOf("/update") >= 0) {
        subPage = 9;
    } else {
        subPage = 255; // welcome page
    }

    if (!correctPIN && strlen(settingsPIN) > 0 && subPage > 0 && subPage < 11) {
        originalSubPage = subPage;
        subPage = 252;
    }

    if ((subPage == 1 && wifiLock && otaLock) ||
        (post && !correctPIN && millis() - lastEditTime < 3000)) {
        serveMessage(request, 500, "Access Denied", FPSTR(s_unlock_ota), 254);
        return;
    }

    if (post) {
        if (subPage != 1 || !(wifiLock && otaLock)) handleSettingsSet(request, subPage);
        char s[32]; char s2[45] = "";
        switch (subPage) {
            case 1:   strcpy_P(s, PSTR("WiFi")); USER_PRINTLN("savesettings forcing reconnect"); strcpy_P(s2, PSTR("Please connect to the new IP (if changed)")); forceReconnect = true; break;
            case 2:   strcpy_P(s, PSTR("LED")); break;
            case 3:   strcpy_P(s, PSTR("UI")); break;
            case 4:   strcpy_P(s, PSTR("Sync")); break;
            case 5:   strcpy_P(s, PSTR("Time")); break;
            case 6:   strcpy_P(s, PSTR("Security")); if (doReboot) strcpy_P(s2, PSTR("Rebooting, please wait ~10 seconds...")); break;
            case 7:   strcpy_P(s, PSTR("DMX")); break;
            case 8:   strcpy_P(s, PSTR("Usermods")); break;
            case 10:  strcpy_P(s, PSTR("2D")); break;
            case 252: strcpy_P(s, correctPIN ? PSTR("PIN accepted") : PSTR("PIN rejected")); break;
            default:  s[0] = 0; break;
        }
        if (subPage == 252) {
            createEditHandler(correctPIN);
        } else {
            strcat_P(s, PSTR(" settings saved."));
        }
        if (subPage == 252 && correctPIN) {
            subPage = originalSubPage;
        } else {
            if (!s2[0]) strcpy_P(s2, s_redirecting);
            serveMessage(request, 200, s, s2,
                         (subPage == 1 || (subPage == 6 && doReboot)) ? 129 : (correctPIN ? 1 : 3));
            return;
        }
    }

    AsyncWebServerResponse* response;
    switch (subPage) {
        case 1:   response = request->beginResponse_P(200, "text/html", PAGE_settings_wifi, PAGE_settings_wifi_length); break;
        case 2:   response = request->beginResponse_P(200, "text/html", PAGE_settings_leds, PAGE_settings_leds_length); break;
        case 3:   response = request->beginResponse_P(200, "text/html", PAGE_settings_ui,   PAGE_settings_ui_length);   break;
        case 4:   response = request->beginResponse_P(200, "text/html", PAGE_settings_sync, PAGE_settings_sync_length); break;
        case 5:   response = request->beginResponse_P(200, "text/html", PAGE_settings_time, PAGE_settings_time_length); break;
        case 6:   response = request->beginResponse_P(200, "text/html", PAGE_settings_sec,  PAGE_settings_sec_length);  break;
#ifdef WLED_ENABLE_DMX
        case 7:   response = request->beginResponse_P(200, "text/html", PAGE_settings_dmx,  PAGE_settings_dmx_length);  break;
#endif
        case 8:   response = request->beginResponse_P(200, "text/html", PAGE_settings_um,   PAGE_settings_um_length);   break;
        case 9:   response = request->beginResponse_P(200, "text/html", PAGE_update,        PAGE_update_length);        break;
#ifndef WLED_DISABLE_2D
        case 10:  response = request->beginResponse_P(200, "text/html", PAGE_settings_2D,   PAGE_settings_2D_length);   break;
#endif
        case 251: {
            correctPIN = !strlen(settingsPIN);
            createEditHandler(correctPIN);
            serveMessage(request, 200,
                         strlen(settingsPIN) > 0 ? PSTR("Settings locked") : PSTR("No PIN set"),
                         FPSTR(s_redirecting), 1);
            return;
        }
        case 252: response = request->beginResponse_P(200, "text/html", PAGE_settings_pin,  PAGE_settings_pin_length);  break;
        case 253: response = request->beginResponse_P(200, "text/css",  PAGE_settingsCss,   PAGE_settingsCss_length);   break;
        case 254: serveSettingsJS(request); return;
        case 255: response = request->beginResponse_P(200, "text/html", PAGE_welcome,       PAGE_welcome_length);       break;
        default:  response = request->beginResponse_P(200, "text/html", PAGE_settings,      PAGE_settings_length);      break;
    }
    response->addHeader(FPSTR(s_content_enc), "gzip");
    setStaticContentCacheHeaders(response);
    request->send(response);
    delete response;
}

// ── createEditHandler — IDF stub (no SPIFFSEditor available) ─────────────────
void createEditHandler(bool enable) {
    // In the IDF build the FS editor is not available. Register a simple
    // informational handler on /edit.
    if (enable) {
        server.on("/edit", HTTP_ANY,
            [](AsyncWebServerRequest* request) {
                serveMessage(request, 501, "Not implemented",
                             F("The FS editor is not available in the IDF build."), 254);
            });
    } else {
        server.on("/edit", HTTP_ANY,
            [](AsyncWebServerRequest* request) {
                serveMessage(request, 500, "Access Denied", FPSTR(s_unlock_cfg), 254);
            });
    }
}

// ── XML / URL helpers (forward-declared in fcn_declare.h) ────────────────────
// These live in xml.cpp and set.cpp which compile fine in IDF build.

// ── initServer ────────────────────────────────────────────────────────────────
void initServer() {
    // CORS headers on every response
    DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Origin"),  "*");
    DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Methods"), "*");
    DefaultHeaders::Instance().addHeader(F("Access-Control-Allow-Headers"), "*");

    // ── Captive portal detection ──────────────────────────────────────────────
    auto captiveHandler = [](AsyncWebServerRequest* request) {
        if (apActive)
            request->redirect("http://" + Network.softAPIP().toString() + "/");
        else
            request->send(204);
    };
    server.on("/generate_204",        HTTP_GET, captiveHandler);
    server.on("/hotspot-detect.html", HTTP_GET, captiveHandler);
    server.on("/connecttest.txt",     HTTP_GET, captiveHandler);
    server.on("/canonical.html",      HTTP_GET, captiveHandler);

    // ── Root ──────────────────────────────────────────────────────────────────
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        USER_PRINT("Client request");
        if (captivePortal(request)) return;
        serveIndexOrWelcome(request);
        USER_PRINTLN(" done");
    });

    // ── Static pages ──────────────────────────────────────────────────────────
    server.on("/sliders", HTTP_GET, [](AsyncWebServerRequest* request) { serveIndex(request); });
    server.on("/welcome", HTTP_GET, [](AsyncWebServerRequest* request) { serveSettings(request); });

    server.on("/settings", HTTP_GET,  [](AsyncWebServerRequest* request) { serveSettings(request); });
    server.on("/settings", HTTP_POST, [](AsyncWebServerRequest* request) { serveSettings(request, true); });

    server.on("/reset", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveMessage(request, 200, F("Rebooting now..."), F("Please wait ~10 seconds..."), 129);
        doReboot = true;
    });

    // ── Style / JS assets ─────────────────────────────────────────────────────
    auto sendGzAsset = [](AsyncWebServerRequest* request,
                          const uint8_t* data, size_t len,
                          const char* type) {
        if (handleIfNoneMatchCacheHeader(request)) return;
        AsyncWebServerResponse* response = request->beginResponse_P(200, type, data, len);
        response->addHeader(FPSTR(s_content_enc), "gzip");
        setStaticContentCacheHeaders(response);
        request->send(response);
        delete response;
    };

    server.on("/style.css",       HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, PAGE_settingsCss,  PAGE_settingsCss_length,  "text/css"); });
    server.on("/iro.js",          HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, iroJs,             iroJs_length,             "application/javascript"); });
    server.on("/rangetouch.js",   HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, rangetouchJs,      rangetouchJs_length,      "application/javascript"); });
    server.on("/peek.js",         HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, PAGE_peekJs,       PAGE_peekJs_length,       "application/javascript"); });

#ifdef WLED_ENABLE_WEBSOCKETS
    server.on("/liveview",  HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, PAGE_liveviewws,   PAGE_liveviewws_length,   "text/html"); });
  #ifndef WLED_DISABLE_2D
    server.on("/liveview2D",HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, PAGE_liveviewws2D, PAGE_liveviewws2D_length, "text/html"); });
  #endif
#else
    server.on("/liveview",  HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, PAGE_liveview,     PAGE_liveview_length,     "text/html"); });
#endif

    server.on("/u",        HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, PAGE_usermod, PAGE_usermod_length, "text/html"); });
    server.on("/teapot",   HTTP_GET, [](AsyncWebServerRequest* request) {
        serveMessage(request, 418, F("418. I'm a teapot."), F("(Tangible Embedded Advanced Project Of Twinkling)"), 254);
    });

    // Ace.js editor assets
    server.on("/ace.js",          HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, aceJs,           aceJs_length,           "application/javascript"); });
    server.on("/mode-html.js",    HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, modehtmlJs,      modehtmlJs_length,      "application/javascript"); });
    server.on("/worker-html.js",  HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, workerhtmlJs,    workerhtmlJs_length,    "application/javascript"); });
    server.on("/mode-json.js",    HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, modejsonlJs,     modejsonlJs_length,     "application/javascript"); });
    server.on("/worker-json.js",  HTTP_GET, [sendGzAsset](AsyncWebServerRequest* r) { sendGzAsset(r, workerjsonJs,    workerjsonJs_length,    "application/javascript"); });

    // Favicon
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!handleFileRead(request, "/favicon.ico"))
            request->send_P(200, "image/x-icon", favicon, 156);
    });

    // ── JSON API ──────────────────────────────────────────────────────────────
    server.on("/json", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveJson(request);
    });

    // JSON POST handler (mirrors AsyncCallbackJsonWebHandler in wled_server.cpp)
    server.on("/json", HTTP_POST, [](AsyncWebServerRequest* request) {
        bool verboseResponse = false;
        bool isConfig = false;
        if (!requestJSONBufferLock(14)) return;

        DeserializationError error = deserializeJson(doc, (uint8_t*)(request->_tempObject));
        JsonObject root = doc.as<JsonObject>();
        if (error || root.isNull()) {
            releaseJSONBufferLock();
            request->send(400, "application/json", F("{\"error\":9}"));
            return;
        }
        const String& url = request->url();
        isConfig = url.indexOf("cfg") > -1;
        if (!isConfig) {
            verboseResponse = deserializeState(root);
        } else {
            verboseResponse = deserializeConfig(root);
        }
        releaseJSONBufferLock();

        if (verboseResponse) {
            if (!isConfig) {
                lastInterfaceUpdate = millis();
                interfaceUpdateCallMode = CALL_MODE_WS_SEND;
                serveJson(request);
                return;
            } else {
                doSerializeConfig = true;
            }
        }
        request->send(200, "application/json", F("{\"success\":true}"));
    });
    // We need the JSON POST handler to read the body into _tempObject before the
    // lambda above runs. Register a custom handler that does that for /json POST.
    server.addHandler(new AsyncCallbackJsonWebHandler("/json",
        [](AsyncWebServerRequest* request) {
            bool verboseResponse = false;
            bool isConfig = false;
            if (!requestJSONBufferLock(14)) return;

            DeserializationError error = deserializeJson(doc, (uint8_t*)(request->_tempObject));
            JsonObject root = doc.as<JsonObject>();
            if (error || root.isNull()) {
                releaseJSONBufferLock();
                request->send(400, "application/json", F("{\"error\":9}"));
                return;
            }
            const String& url = request->url();
            isConfig = url.indexOf("cfg") > -1;
            if (!isConfig) {
                verboseResponse = deserializeState(root);
            } else {
                verboseResponse = deserializeConfig(root);
            }
            releaseJSONBufferLock();

            if (verboseResponse) {
                if (!isConfig) {
                    lastInterfaceUpdate = millis();
                    interfaceUpdateCallMode = CALL_MODE_WS_SEND;
                    serveJson(request);
                    return;
                } else {
                    doSerializeConfig = true;
                }
            }
            request->send(200, "application/json", F("{\"success\":true}"));
        }
    ));

    // JSON PUT (same as POST)
    server.on("/json", HTTP_PUT, [](AsyncWebServerRequest* request) {
        serveJson(request);
    });

    // ── Misc endpoints ────────────────────────────────────────────────────────
    server.on("/version",  HTTP_GET, [](AsyncWebServerRequest* request) { request->send(200, "text/plain", String(VERSION)); });
    server.on("/uptime",   HTTP_GET, [](AsyncWebServerRequest* request) { request->send(200, "text/plain", String(millis())); });
    server.on("/freeheap", HTTP_GET, [](AsyncWebServerRequest* request) { request->send(200, "text/plain", String(ESP.getFreeHeap())); });
    server.on("/getflash", HTTP_GET, [](AsyncWebServerRequest* request) { request->send(200, "text/plain", String(ESP.getFlashChipSize())); });

    server.on("/url",      HTTP_GET, [](AsyncWebServerRequest* request) { URL_response(request); });

#ifdef WLED_ENABLE_DMX
    server.on("/dmxmap", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send_P(200, "text/html", PAGE_dmxmap, dmxProcessor);
    });
#else
    server.on("/dmxmap", HTTP_GET, [](AsyncWebServerRequest* request) {
        serveMessage(request, 501, "Not implemented", F("DMX support is not enabled in this build."), 254);
    });
#endif

#ifdef WLED_ENABLE_SIMPLE_UI
    server.on("/simple.htm", HTTP_GET, [sendGzAsset](AsyncWebServerRequest* request) {
        if (handleFileRead(request, "/simple.htm")) return;
        if (handleIfNoneMatchCacheHeader(request)) return;
        AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", PAGE_simple, PAGE_simple_L);
        response->addHeader(FPSTR(s_content_enc), "gzip");
        setStaticContentCacheHeaders(response);
        request->send(response);
        delete response;
    });
#endif

#ifdef WLED_ENABLE_PIXART
    server.on("/pixart.htm", HTTP_GET, [sendGzAsset](AsyncWebServerRequest* request) {
        if (handleFileRead(request, "/pixart.htm")) return;
        if (handleIfNoneMatchCacheHeader(request)) return;
        AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", PAGE_pixart, PAGE_pixart_L);
        response->addHeader(FPSTR(s_content_enc), "gzip");
        setStaticContentCacheHeaders(response);
        request->send(response);
        delete response;
    });
#endif

    server.on("/cpal.htm", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (handleFileRead(request, "/cpal.htm")) return;
        if (handleIfNoneMatchCacheHeader(request)) return;
        AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", PAGE_cpal, PAGE_cpal_L);
        response->addHeader(FPSTR(s_content_enc), "gzip");
        setStaticContentCacheHeaders(response);
        request->send(response);
        delete response;
    });

    // OTA update page (informational only in IDF build — actual OTA via ota_littlefs.h)
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (otaLock) {
            serveMessage(request, 500, "Access Denied", FPSTR(s_unlock_ota), 254);
        } else {
            serveSettings(request);
        }
    });
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest* request) {
        serveMessage(request, 501, "Not implemented",
                     F("Use /json/cfg or OTA via the native IDF mechanism."), 254);
    });

    // File upload (presets.json, cfg.json, etc.)
    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest* request) {},  // response sent by upload handler
        [](AsyncWebServerRequest* request, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
            handleUpload(request, filename, index, data, len, final);
        }
    );

    // Edit handler (FS editor — stubbed in IDF build)
    createEditHandler(correctPIN);

    // ── Not-found / catch-all ─────────────────────────────────────────────────
    server.onNotFound([](AsyncWebServerRequest* request) {
        DEBUG_PRINT("Not-Found HTTP call: ");
        DEBUG_PRINTLN("URI: " + request->url());
        if (captivePortal(request)) return;

        // CORS pre-flight
        if (request->method() == HTTP_OPTIONS) {
            AsyncWebServerResponse* response = request->beginResponse(200);
            response->addHeader(F("Access-Control-Max-Age"), F("7200"));
            request->send(response);
            delete response;
            return;
        }

        // /win URL API
        if (handleSet(request, request->url())) return;
        // Try LittleFS
        if (handleFileRead(request, request->url())) return;

        // 404
        AsyncWebServerResponse* response =
            request->beginResponse_P(404, "text/html", PAGE_404, PAGE_404_length);
        response->addHeader(FPSTR(s_content_enc), "gzip");
        setStaticContentCacheHeaders(response);
        request->send(response);
        delete response;
    });

    // ── Start server ──────────────────────────────────────────────────────────
    server.begin();
}

// ── handleUpload (file upload via HTTP POST /upload) ─────────────────────────
// Called from the upload handler registered above.
void handleUpload(AsyncWebServerRequest* request, const String& filename,
                  size_t index, uint8_t* data, size_t len, bool final) {
    if (!correctPIN) {
        if (final) request->send(500, "text/plain", FPSTR(s_unlock_cfg));
        return;
    }
    if (!index) {
        String finalname = filename;
        if (finalname.charAt(0) != '/') finalname = '/' + finalname;

        // Open file for writing via POSIX
        String abspath = String(LITTLEFS_BASE) + finalname;
        FILE* fp = fopen(abspath.c_str(), "wb");
        request->_tempFile = fp;  // store as void*
        DEBUG_PRINT(F("Uploading "));
        DEBUG_PRINTLN(finalname);
        if (finalname.equals("/presets.json")) presetsModifiedTime = toki.second();
    }
    if (len && request->_tempFile) {
        FILE* fp = static_cast<FILE*>(request->_tempFile);
        fwrite(data, 1, len, fp);
    }
    if (final) {
        if (request->_tempFile) {
            fclose(static_cast<FILE*>(request->_tempFile));
            request->_tempFile = nullptr;
        }
        USER_PRINT(F("File uploaded: "));
        USER_PRINTLN(filename);
        invalidateFileNameCache();
        if (filename.equalsIgnoreCase("/cfg.json") || filename.equalsIgnoreCase("cfg.json")) {
            request->send(200, "text/plain", F("Configuration restore successful.\nRebooting..."));
            doReboot = true;
        } else if (filename.equals("/presets.json") || filename.equals("presets.json")) {
            request->send(200, "text/plain", F("Presets File Uploaded!"));
        } else {
            request->send(200, "text/plain", F("File Uploaded!"));
        }
        cacheInvalidate++;
    }
}

#endif // WLED_IDF_BUILD
