// AsyncWebServer.h — IDF shim for ESPAsyncWebServer
// Backs the WLED HTTP server with esp_http_server for pure IDF builds.
// Provides AsyncWebServerRequest, AsyncWebServerResponse, AsyncAbstractResponse,
// AsyncWebHandler, AsyncWebServer compatible with WLED's usage patterns.
//
// NAMING CONFLICT: esp_http_server.h defines HTTP_GET=1, HTTP_POST=3, etc. as
// httpd_method_t enum values. ESPAsyncWebServer uses different bitmask values.
// We include esp_http_server.h first, save the IDF values, then redefine the
// names as bitmasks matching ESPAsyncWebServer convention.

#pragma once
#define ASYNC_WEB_SERVER_SHIM_H_INCLUDED

// ── IDF includes first (before any HTTP_GET etc. redefinition) ───────────────
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

// Save IDF httpd_method_t values before we redefine the names
static const httpd_method_t HTTPD_M_GET     = HTTP_GET;
static const httpd_method_t HTTPD_M_POST    = HTTP_POST;
static const httpd_method_t HTTPD_M_PUT     = HTTP_PUT;
static const httpd_method_t HTTPD_M_DELETE  = HTTP_DELETE;
static const httpd_method_t HTTPD_M_OPTIONS = HTTP_OPTIONS;
static const httpd_method_t HTTPD_M_HEAD    = HTTP_HEAD;

// Undefine IDF method names so we can redefine as bitmasks
#undef HTTP_GET
#undef HTTP_POST
#undef HTTP_PUT
#undef HTTP_DELETE
#undef HTTP_HEAD
#undef HTTP_OPTIONS
#undef HTTP_ANY

// ── ESPAsyncWebServer-compatible method bitmasks ──────────────────────────────
#define HTTP_GET     0x01
#define HTTP_POST    0x02
#define HTTP_DELETE  0x04
#define HTTP_PUT     0x08
#define HTTP_PATCH   0x10
#define HTTP_HEAD    0x20
#define HTTP_OPTIONS 0x40
#define HTTP_ANY     0x7F

typedef uint8_t WebRequestMethodComposite;

// ── C++ standard includes ─────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <functional>
#include <list>
#include <map>
#include <cstring>
#include <cstdlib>
#include <cctype>

// Pull in the String shim
#include "idf_compat.h"

// ── Forward declarations ──────────────────────────────────────────────────────
class AsyncWebServer;
class AsyncWebServerRequest;
class AsyncWebServerResponse;
class AsyncAbstractResponse;
class AsyncWebHandler;
class AsyncWebParameter;
class AsyncWebHeader;

// ── URL decode helper ─────────────────────────────────────────────────────────
static inline String _aws_urlDecode(const char* src) {
    String out;
    if (!src) return out;
    size_t srcLen = strlen(src);
    out.reserve(srcLen);
    for (size_t i = 0; src[i]; i++) {
        if (src[i] == '%' && i + 2 < srcLen &&
            isxdigit((unsigned char)src[i+1]) &&
            isxdigit((unsigned char)src[i+2])) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

// ── AsyncWebParameter ─────────────────────────────────────────────────────────
class AsyncWebParameter {
public:
    String _name;
    String _value;
    bool   _isPost;
    AsyncWebParameter(const String& name, const String& value, bool isPost = false)
        : _name(name), _value(value), _isPost(isPost) {}
    const String& name()  const { return _name; }
    const String& value() const { return _value; }
    bool isPost() const { return _isPost; }
};

// ── AsyncWebHeader ────────────────────────────────────────────────────────────
class AsyncWebHeader {
public:
    String _name;
    String _value;
    AsyncWebHeader() {}
    AsyncWebHeader(const String& name, const String& value) : _name(name), _value(value) {}
    const String& name()  const { return _name; }
    const String& value() const { return _value; }
};

// ── AsyncAbstractResponse ─────────────────────────────────────────────────────
class AsyncAbstractResponse {
public:
    int    _code          = 200;
    String _contentType   = "text/plain";
    size_t _contentLength = 0;
    size_t _sentLength    = 0;
    bool   _isValid       = true;

    std::vector<std::pair<String,String>> _headers;

    virtual ~AsyncAbstractResponse() {}

    void addHeader(const String& name, const String& value) {
        _headers.push_back({name, value});
    }
    void addHeader(const char* name, const char* value) {
        _headers.push_back({String(name), String(value)});
    }
    void addHeader(const char* name, const String& value) {
        _headers.push_back({String(name), value});
    }

    bool _sourceValid() const { return _isValid; }

    // Subclasses override to fill a buffer chunk at position _sentLength.
    // Returns bytes written.
    virtual size_t _fillBuffer(uint8_t* buf, size_t len) {
        (void)buf; (void)len;
        return 0;
    }
};

// ── AsyncWebServerResponse ────────────────────────────────────────────────────
class AsyncWebServerResponse : public AsyncAbstractResponse {
public:
    String         _content;
    const uint8_t* _progmem    = nullptr;
    size_t         _progmemLen = 0;
    std::function<String(const String&)> _processor;

    AsyncWebServerResponse() {}

    AsyncWebServerResponse(int code, const String& type, const String& content) {
        _code = code; _contentType = type; _content = content;
        _contentLength = content.length(); _isValid = true;
    }
    AsyncWebServerResponse(int code, const String& type,
                           const uint8_t* data, size_t len) {
        _code = code; _contentType = type;
        _progmem = data; _progmemLen = len;
        _contentLength = len; _isValid = true;
    }

    virtual size_t _fillBuffer(uint8_t* buf, size_t len) override {
        const uint8_t* src   = _progmem ? _progmem        : (const uint8_t*)_content.c_str();
        size_t         total = _progmem ? _progmemLen      : _content.length();
        if (_sentLength >= total) return 0;
        size_t remaining = total - _sentLength;
        size_t toWrite   = (len < remaining) ? len : remaining;
        memcpy(buf, src + _sentLength, toWrite);
        return toWrite;
    }
};

// ── DefaultHeaders singleton ──────────────────────────────────────────────────
class DefaultHeaders {
public:
    std::vector<std::pair<String,String>> _headers;
    static DefaultHeaders& Instance() {
        static DefaultHeaders inst;
        return inst;
    }
    void addHeader(const String& name, const String& value) {
        _headers.push_back({name, value});
    }
    void addHeader(const char* name, const char* value) {
        _headers.push_back({String(name), String(value)});
    }
    // Apply stored headers to an IDF request (before sending response)
    void applyTo(httpd_req_t* req) const {
        for (auto& h : _headers)
            httpd_resp_set_hdr(req, h.first.c_str(), h.second.c_str());
    }
};

// ── AsyncWebServerRequest ─────────────────────────────────────────────────────
class AsyncWebServerRequest {
public:
    httpd_req_t* _req;
    void*        _tempObject = nullptr; // used by AsyncCallbackJsonWebHandler
    void*        _tempFile   = nullptr; // used by file upload handlers

private:
    String _url;
    String _queryString;
    std::vector<AsyncWebParameter*> _params;
    int    _method = HTTP_GET;
    bool   _parsed = false;

    void _parseUri() {
        if (_parsed) return;
        _parsed = true;
        const char* uri = _req->uri;
        const char* q   = strchr(uri, '?');
        if (q) {
            _url.assign(uri, (size_t)(q - uri));
            _queryString = String(q + 1);
        } else {
            _url = String(uri);
        }
        _parseQueryString(_queryString.c_str(), false);

        switch (_req->method) {
            case HTTPD_M_GET:     _method = HTTP_GET;     break;
            case HTTPD_M_POST:    _method = HTTP_POST;    break;
            case HTTPD_M_PUT:     _method = HTTP_PUT;     break;
            case HTTPD_M_DELETE:  _method = HTTP_DELETE;  break;
            case HTTPD_M_OPTIONS: _method = HTTP_OPTIONS; break;
            case HTTPD_M_HEAD:    _method = HTTP_HEAD;    break;
            default:              _method = HTTP_GET;     break;
        }
    }

    void _parseQueryString(const char* qs, bool isPost) {
        if (!qs || !*qs) return;
        const char* p = qs;
        while (*p) {
            const char* eq  = strchr(p, '=');
            const char* amp = strchr(p, '&');
            if (!amp) amp = p + strlen(p); // points to NUL
            if (eq && eq < amp) {
                String name  = _aws_urlDecode(std::string(p, (size_t)(eq - p)).c_str());
                String value = _aws_urlDecode(std::string(eq + 1, (size_t)(amp - eq - 1)).c_str());
                _params.push_back(new AsyncWebParameter(name, value, isPost));
            } else if (amp > p) {
                String name = _aws_urlDecode(std::string(p, (size_t)(amp - p)).c_str());
                _params.push_back(new AsyncWebParameter(name, "", isPost));
            }
            if (!*amp) break;
            p = amp + 1;
        }
    }

public:
    explicit AsyncWebServerRequest(httpd_req_t* req) : _req(req) {
        _parseUri();
    }
    ~AsyncWebServerRequest() {
        for (auto* p : _params) delete p;
        if (_tempObject) { free(_tempObject); _tempObject = nullptr; }
    }

    // Add parsed POST body as form params
    void _addPostBody(const uint8_t* data, size_t len) {
        String body(std::string(reinterpret_cast<const char*>(data), len));
        _parseQueryString(body.c_str(), true);
    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    const String& url()    const { return _url; }
    int           method() const { return _method; }

    String host() const {
        char buf[128] = "";
        if (httpd_req_get_hdr_value_str(_req, "Host", buf, sizeof(buf)) == ESP_OK)
            return String(buf);
        return String();
    }

    // Params
    size_t args() const { return _params.size(); }

    String arg(const String& name) const {
        for (auto* p : _params)
            if (p->_name == name) return p->_value;
        return String();
    }
    String arg(const char* name) const { return arg(String(name)); }

    bool hasArg(const String& name) const {
        for (auto* p : _params)
            if (p->_name == name) return true;
        return false;
    }
    bool hasArg(const char* name) const { return hasArg(String(name)); }

    String argName(size_t i) const {
        return (i < _params.size()) ? _params[i]->_name : String();
    }

    AsyncWebParameter* getParam(const String& name, bool isPost = false) const {
        // prefer matching isPost
        for (auto* p : _params)
            if (p->_name == name && p->_isPost == isPost) return p;
        // fallback: any param with that name
        for (auto* p : _params)
            if (p->_name == name) return p;
        return nullptr;
    }
    bool hasParam(const String& name, bool isPost = false) const {
        return getParam(name, isPost) != nullptr;
    }

    // By-index accessors (used by set.cpp WLEDMM pin handling)
    size_t params() const { return _params.size(); }
    AsyncWebParameter* getParam(size_t index) const {
        if (index < _params.size()) return _params[index];
        return nullptr;
    }

    // Headers
    bool hasHeader(const String& name) const {
        char buf[4] = "";
        // Check if header exists (even with empty value)
        size_t hlen = 0;
        esp_err_t e = httpd_req_get_hdr_value_len(_req, name.c_str());
        return (e > 0);
    }
    bool hasHeader(const char* name) const { return hasHeader(String(name)); }

    // Returns header value (static storage — do not store pointer long-term)
    AsyncWebHeader* getHeader(const String& name) const {
        static AsyncWebHeader tmp;
        size_t hlen = httpd_req_get_hdr_value_len(_req, name.c_str());
        if (hlen == 0) return nullptr;
        char* buf = (char*)malloc(hlen + 1);
        if (!buf) return nullptr;
        if (httpd_req_get_hdr_value_str(_req, name.c_str(), buf, hlen + 1) == ESP_OK) {
            tmp._name  = name;
            tmp._value = String(buf);
            free(buf);
            return &tmp;
        }
        free(buf);
        return nullptr;
    }
    AsyncWebHeader* getHeader(const char* name) const { return getHeader(String(name)); }

    void addInterestingHeader(const char*) {} // no-op

    // ── Sending responses ─────────────────────────────────────────────────────

    // Apply default headers helper
    void _applyDefaultHeaders() {
        DefaultHeaders::Instance().applyTo(_req);
    }

    void _setStatus(int code) {
        // Map common codes; fall back to numeric string
        switch (code) {
            case 200: httpd_resp_set_status(_req, "200 OK");                   return;
            case 204: httpd_resp_set_status(_req, "204 No Content");           return;
            case 301: httpd_resp_set_status(_req, "301 Moved Permanently");    return;
            case 302: httpd_resp_set_status(_req, "302 Found");                return;
            case 304: httpd_resp_set_status(_req, "304 Not Modified");         return;
            case 400: httpd_resp_set_status(_req, "400 Bad Request");          return;
            case 403: httpd_resp_set_status(_req, "403 Forbidden");            return;
            case 404: httpd_resp_set_status(_req, "404 Not Found");            return;
            case 405: httpd_resp_set_status(_req, "405 Method Not Allowed");   return;
            case 413: httpd_resp_set_status(_req, "413 Payload Too Large");    return;
            case 418: httpd_resp_set_status(_req, "418 I'm a teapot");         return;
            case 500: httpd_resp_set_status(_req, "500 Internal Server Error");return;
            case 501: httpd_resp_set_status(_req, "501 Not Implemented");      return;
            case 503: httpd_resp_set_status(_req, "503 Service Unavailable");  return;
            default:  { char s[16]; snprintf(s, sizeof(s), "%d", code);
                        httpd_resp_set_status(_req, s); }
        }
    }

    // send integer code only
    void send(int code) {
        _applyDefaultHeaders();
        _setStatus(code);
        httpd_resp_set_type(_req, "text/plain");
        httpd_resp_send(_req, nullptr, 0);
    }

    // send code + type + string body
    void send(int code, const char* type, const String& content) {
        _applyDefaultHeaders();
        _setStatus(code);
        httpd_resp_set_type(_req, type ? type : "text/plain");
        httpd_resp_send(_req, content.c_str(), (int)content.length());
    }
    void send(int code, const String& type, const String& content) {
        send(code, type.c_str(), content);
    }
    void send(int code, const char* type, const char* content) {
        send(code, type, String(content ? content : ""));
    }

    // send_P — null-terminated C-string variant (e.g. JSON_palette_names)
    void send_P(int code, const char* type, const char* data) {
        _applyDefaultHeaders();
        _setStatus(code);
        httpd_resp_set_type(_req, type ? type : "text/plain");
        httpd_resp_send(_req, data, data ? (int)strlen(data) : 0);
    }

    // send_P — data from ROM/PROGMEM (no template processor)
    void send_P(int code, const char* type, const uint8_t* data, size_t len) {
        _applyDefaultHeaders();
        _setStatus(code);
        httpd_resp_set_type(_req, type ? type : "application/octet-stream");
        httpd_resp_send(_req, reinterpret_cast<const char*>(data), (int)len);
    }

    // send_P with template processor — const char* variant
    void send_P(int code, const char* type, const char* data,
                std::function<String(const String&)> processor) {
        send_P(code, type, reinterpret_cast<const uint8_t*>(data), processor);
    }

    // send_P with template processor (used by serveMessage)
    void send_P(int code, const char* type, const uint8_t* data,
                std::function<String(const String&)> processor) {
        // Expand %VARNAME% placeholders
        String src(reinterpret_cast<const char*>(data));
        String result;
        result.reserve(src.length() + 512);
        size_t pos = 0;
        while (pos < src.length()) {
            int pct = src.indexOf('%', pos);
            if (pct < 0) { result += src.substring(pos); break; }
            result += src.substring(pos, pct);
            int end = src.indexOf('%', pct + 1);
            if (end < 0) { result += src.substring(pct); break; }
            String var = src.substring(pct + 1, end);
            result += processor(var);
            pos = (size_t)(end + 1);
        }
        send(code, type, result);
    }

    // send AsyncAbstractResponse*
    void send(AsyncAbstractResponse* response) {
        if (!response) { send(500); return; }
        _applyDefaultHeaders();
        _setStatus(response->_code);
        httpd_resp_set_type(_req, response->_contentType.c_str());
        for (auto& h : response->_headers)
            httpd_resp_set_hdr(_req, h.first.c_str(), h.second.c_str());

        if (response->_contentLength > 0) {
            // Known length — fill in one shot (up to 512 KB; large enough for WLED)
            size_t maxLen = response->_contentLength;
            // Cap at 512KB for safety
            if (maxLen > 524288) maxLen = 524288;
            uint8_t* buf = (uint8_t*)malloc(maxLen + 1);
            if (buf) {
                response->_sentLength = 0;
                size_t n = response->_fillBuffer(buf, maxLen);
                httpd_resp_send(_req, reinterpret_cast<const char*>(buf), (int)n);
                free(buf);
                return;
            }
        }
        // Unknown length or malloc failed — streaming approach
        {
            const size_t CHUNK = 4096;
            uint8_t* buf = (uint8_t*)malloc(CHUNK);
            if (!buf) { httpd_resp_send(_req, nullptr, 0); return; }
            // Collect into growable buffer
            size_t total = 0, capacity = CHUNK * 4;
            uint8_t* full = (uint8_t*)malloc(capacity);
            if (!full) { free(buf); httpd_resp_send(_req, nullptr, 0); return; }
            response->_sentLength = 0;
            while (true) {
                size_t n = response->_fillBuffer(buf, CHUNK);
                if (n == 0) break;
                if (total + n > capacity) {
                    capacity = (total + n) * 2;
                    uint8_t* nb = (uint8_t*)realloc(full, capacity);
                    if (!nb) { free(full); free(buf); httpd_resp_send(_req, nullptr, 0); return; }
                    full = nb;
                }
                memcpy(full + total, buf, n);
                total += n;
                response->_sentLength = total;
                if (n < CHUNK) break;
            }
            free(buf);
            httpd_resp_send(_req, reinterpret_cast<const char*>(full), (int)total);
            free(full);
        }
    }
    void send(AsyncWebServerResponse* response) {
        send(static_cast<AsyncAbstractResponse*>(response));
    }

    // redirect
    void redirect(const String& url) {
        _applyDefaultHeaders();
        _setStatus(302);
        httpd_resp_set_hdr(_req, "Location", url.c_str());
        httpd_resp_send(_req, nullptr, 0);
    }

    // beginResponse factories (caller owns the pointer; must delete or pass to send())
    AsyncWebServerResponse* beginResponse(int code,
                                          const String& type    = "text/plain",
                                          const String& content = "") {
        return new AsyncWebServerResponse(code, type, content);
    }
    AsyncWebServerResponse* beginResponse_P(int code, const String& type,
                                             const uint8_t* data, size_t len) {
        return new AsyncWebServerResponse(code, type, data, len);
    }
};

// ── AsyncWebHandler ───────────────────────────────────────────────────────────
class AsyncWebHandler {
public:
    virtual ~AsyncWebHandler() {}
    virtual bool canHandle(AsyncWebServerRequest*)  { return false; }
    virtual void handleRequest(AsyncWebServerRequest*) {}
    virtual void handleBody(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t) {}
    virtual void handleUpload(AsyncWebServerRequest*, const String&, size_t,
                              uint8_t*, size_t, bool) {}
    virtual bool isRequestHandlerTrivial() { return true; }
};

// ── AsyncWebServer ────────────────────────────────────────────────────────────

// Route entry
struct _AwsRoute {
    std::string uri;
    int         methodMask;
    std::function<void(AsyncWebServerRequest*)> handler;
};

class AsyncWebServer {
public:
    int            _port;
    httpd_handle_t _httpd = nullptr;

    std::list<_AwsRoute>          _routes;
    std::vector<AsyncWebHandler*> _handlers;
    std::function<void(AsyncWebServerRequest*)> _notFoundHandler;

    // Stable storage for httpd_uri_t (pointers given to IDF must remain valid)
    struct _UriEntry {
        std::string  uri_str;
        httpd_uri_t  uri_cfg;
    };
    std::list<_UriEntry> _uriEntries;

    explicit AsyncWebServer(int port) : _port(port) {}

    // Register a callback route (returns dummy handler ref to match original API)
    AsyncWebHandler& on(const char* uri, int methodMask,
        std::function<void(AsyncWebServerRequest*)> handler,
        std::function<void(AsyncWebServerRequest*, const String&, size_t,
                           uint8_t*, size_t, bool)> /*uploadHandler*/ = nullptr)
    {
        _routes.push_back({std::string(uri), methodMask, handler});
        static AsyncWebHandler dummy;
        return dummy;
    }

    // Register a custom handler
    AsyncWebHandler& addHandler(AsyncWebHandler* h) {
        if (h) _handlers.push_back(h);
        static AsyncWebHandler dummy;
        return dummy;
    }

    void removeHandler(AsyncWebHandler* /*h*/) {}

    void onNotFound(std::function<void(AsyncWebServerRequest*)> fn) {
        _notFoundHandler = fn;
    }

    void begin();

    // Central dispatch called from IDF URI handlers and the 404 handler
    void _dispatch(httpd_req_t* req) {
        DefaultHeaders::Instance().applyTo(req);
        AsyncWebServerRequest request(req);
        const String& url    = request.url();
        int           method = request.method();

        // Read POST/PUT body before route handlers run.
        // form bodies  → parse into request params (arg() works)
        // JSON bodies  → store in _tempObject (JSON route handlers use it)
        if ((method & (HTTP_POST | HTTP_PUT)) && req->content_len > 0 && req->content_len <= 65536) {
            bool isJson = false;
            size_t ctLen = httpd_req_get_hdr_value_len(req, "Content-Type");
            if (ctLen > 0 && ctLen < 128) {
                char ctBuf[129];
                if (httpd_req_get_hdr_value_str(req, "Content-Type", ctBuf, sizeof(ctBuf)) == ESP_OK) {
                    isJson = (strstr(ctBuf, "json") != nullptr);
                }
            }
            uint8_t* body = (uint8_t*)malloc(req->content_len + 1);
            if (body) {
                int r = httpd_req_recv(req, reinterpret_cast<char*>(body), req->content_len);
                if (r > 0) {
                    body[r] = '\0';
                    if (isJson) {
                        request._tempObject = body;  // caller must not free; ~AsyncWebServerRequest does
                        body = nullptr;              // ownership transferred
                    } else {
                        request._addPostBody(body, (size_t)r);
                    }
                }
                if (body) free(body);
            }
        }

        // 1. Check registered routes — exact match first, then prefix match
        // ESPAsyncWebServer matches /settings against /settings/wifi via startsWith("/settings/")
        // Try exact matches first, then fall back to prefix matches.
        for (int pass = 0; pass < 2; pass++) {
            for (auto& route : _routes) {
                if (!(route.methodMask & method)) continue;
                bool matches;
                if (pass == 0) {
                    matches = (route.uri == url.c_str());
                } else {
                    // Prefix match: registered "/foo" matches "/foo/bar"
                    std::string prefix = route.uri + "/";
                    matches = (url.length() > prefix.length() &&
                               url.startsWith(prefix.c_str()));
                }
                if (!matches) continue;
                route.handler(&request);
                return;
            }
        }

        // 2. Check custom handlers (e.g. AsyncCallbackJsonWebHandler)
        for (auto* h : _handlers) {
            if (h->canHandle(&request)) {
                // Body already read above — use _tempObject if set (JSON), else nothing to do
                if (request._tempObject && req->content_len > 0) {
                    h->handleBody(&request,
                                  static_cast<uint8_t*>(request._tempObject),
                                  req->content_len, 0, req->content_len);
                }
                h->handleRequest(&request);
                return;
            }
        }

        // 3. Not-found
        if (_notFoundHandler) {
            _notFoundHandler(&request);
        } else {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        }
    }

    // IDF URI handler trampoline
    static esp_err_t _uri_handler_cb(httpd_req_t* req) {
        AsyncWebServer* srv = static_cast<AsyncWebServer*>(req->user_ctx);
        if (srv) srv->_dispatch(req);
        return ESP_OK;
    }
};

// ── Global pointer for 404 error handler ─────────────────────────────────────
// IDF error handlers don't carry user_ctx, so we use a file-static.
// WLED has exactly one AsyncWebServer instance.
extern AsyncWebServer* _g_aws_instance;

// ── AsyncWebServer::begin() ───────────────────────────────────────────────────
inline void AsyncWebServer::begin() {
    _g_aws_instance = this;

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = (uint16_t)_port;
    config.max_uri_handlers = 50;
    config.stack_size      = 12288;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE("AsyncWebServer", "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }

    // Collect unique (uri, method_bit) pairs and register IDF handlers
    static const struct { int bit; httpd_method_t idf; } MM[] = {
        { HTTP_GET,     HTTPD_M_GET     },
        { HTTP_POST,    HTTPD_M_POST    },
        { HTTP_PUT,     HTTPD_M_PUT     },
        { HTTP_DELETE,  HTTPD_M_DELETE  },
        { HTTP_OPTIONS, HTTPD_M_OPTIONS },
        { HTTP_HEAD,    HTTPD_M_HEAD    },
    };

    // Build a map: uri_string → methodMask
    std::map<std::string, int> uriMethods;
    for (auto& r : _routes)
        uriMethods[r.uri] |= r.methodMask;

    for (auto& kv : uriMethods) {
        for (auto& mm : MM) {
            if (!(kv.second & mm.bit)) continue;

            _uriEntries.push_back(_UriEntry{});
            _UriEntry& ue = _uriEntries.back();
            ue.uri_str = kv.first;
            memset(&ue.uri_cfg, 0, sizeof(ue.uri_cfg));
            ue.uri_cfg.uri      = ue.uri_str.c_str();
            ue.uri_cfg.method   = mm.idf;
            ue.uri_cfg.handler  = _uri_handler_cb;
            ue.uri_cfg.user_ctx = this;

            err = httpd_register_uri_handler(_httpd, &ue.uri_cfg);
            if (err != ESP_OK && err != ESP_ERR_HTTPD_HANDLER_EXISTS) {
                ESP_LOGW("AsyncWebServer", "Register uri %s method=%d: %s",
                         kv.first.c_str(), mm.bit, esp_err_to_name(err));
            }
        }
    }

    // Register 404 error handler so un-matched URIs go through our not-found path
    // (which also handles /win, file reads, handleSet, etc.)
    httpd_register_err_handler(_httpd, HTTPD_404_NOT_FOUND,
        [](httpd_req_t* req, httpd_err_code_t /*e*/) -> esp_err_t {
            if (_g_aws_instance) {
                _g_aws_instance->_dispatch(req);
            } else {
                httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
            }
            return ESP_OK;
        });

    ESP_LOGI("AsyncWebServer", "HTTP server started on port %d", _port);
}
