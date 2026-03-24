// IPAddress.h shim for WLED IDF builds.
// Provides a minimal Arduino-compatible IPAddress class using lwIP types.
// Supports both IPv4 and IPv6 as used by FastUDP.cpp.
#pragma once
#include "idf_compat.h"
#include <lwip/ip_addr.h>
#include <string.h>

// Arduino IPAddress type enum
enum IPType { IPv4 = 0, IPv6 = 1 };

class IPAddress {
    ip_addr_t _addr;
public:
    // ── Constructors ─────────────────────────────────────────────────────────
    IPAddress() {
        memset(&_addr, 0, sizeof(_addr));
        IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V4);
    }
    IPAddress(uint32_t addr) {
        IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V4);
        _addr.u_addr.ip4.addr = addr;
    }
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V4);
        _addr.u_addr.ip4.addr = (uint32_t)a | ((uint32_t)b << 8) |
                                ((uint32_t)c << 16) | ((uint32_t)d << 24);
    }
    IPAddress(const ip4_addr_t& addr) {
        IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V4);
        _addr.u_addr.ip4.addr = addr.addr;
    }
    IPAddress(const ip_addr_t& addr) : _addr(addr) {}

    // IPv6 constructors (used by FastUDP)
    IPAddress(IPType type) {
        memset(&_addr, 0, sizeof(_addr));
        IP_SET_TYPE_VAL(_addr, (type == IPv6) ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
    }
    IPAddress(IPType type, const uint8_t* bytes, uint8_t zone = 0) {
        memset(&_addr, 0, sizeof(_addr));
        if (type == IPv6) {
            IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V6);
            memcpy(_addr.u_addr.ip6.addr, bytes, 16);
#if LWIP_IPV6_SCOPES
            _addr.u_addr.ip6.zone = zone;
#else
            (void)zone;
#endif
        } else {
            IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V4);
            memcpy(&_addr.u_addr.ip4.addr, bytes, 4);
        }
    }

    // ── Type queries ─────────────────────────────────────────────────────────
    bool isIPv6() const { return IP_IS_V6_VAL(_addr); }
    IPType type() const { return isIPv6() ? IPv6 : IPv4; }

    // ── Conversions ──────────────────────────────────────────────────────────
    operator uint32_t() const { return _addr.u_addr.ip4.addr; }

    // Convert to lwIP ip_addr_t (used by FastUDP)
    void to_ip_addr_t(ip_addr_t* out) const { *out = _addr; }
    const ip_addr_t& raw_addr() const { return _addr; }

    // ── Comparison ───────────────────────────────────────────────────────────
    bool operator==(const IPAddress& o) const {
        if (IP_GET_TYPE(&_addr) != IP_GET_TYPE(&o._addr)) return false;
        if (IP_IS_V6_VAL(_addr))
            return memcmp(_addr.u_addr.ip6.addr, o._addr.u_addr.ip6.addr, 16) == 0;
        return _addr.u_addr.ip4.addr == o._addr.u_addr.ip4.addr;
    }
    bool operator!=(const IPAddress& o) const { return !(*this == o); }
    // Explicit uint32_t comparisons to avoid ambiguity with lwIP u32_t conversions
    bool operator==(uint32_t addr) const { return _addr.u_addr.ip4.addr == addr; }
    bool operator!=(uint32_t addr) const { return _addr.u_addr.ip4.addr != addr; }

    // ── Subscript (IPv4 octet access) ────────────────────────────────────────
    uint8_t operator[](int i) const {
        return (_addr.u_addr.ip4.addr >> (i * 8)) & 0xFF;
    }
    // Non-const subscript returns a proxy so that ip[i] = val works as an lvalue.
    struct ByteRef {
        ip_addr_t& _a; int _i;
        ByteRef(ip_addr_t& a, int i) : _a(a), _i(i) {}
        operator uint8_t() const { return (_a.u_addr.ip4.addr >> (_i * 8)) & 0xFF; }
        ByteRef& operator=(uint8_t v) {
            uint32_t& w = _a.u_addr.ip4.addr;
            uint32_t mask = 0xFFU << (_i * 8);
            w = (w & ~mask) | ((uint32_t)v << (_i * 8));
            return *this;
        }
        // Allow assignment from any integer-convertible type (e.g. ArduinoJson variants)
        template<typename T>
        ByteRef& operator=(T v) { return operator=((uint8_t)(int)v); }
    };
    ByteRef operator[](int i) { return ByteRef(_addr, i); }

    // ── Parse from dotted-decimal string ─────────────────────────────────────
    bool fromString(const char* s) {
        if (!s) return false;
        unsigned a, b, c, d;
        if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        IP_SET_TYPE_VAL(_addr, IPADDR_TYPE_V4);
        _addr.u_addr.ip4.addr = (uint32_t)a | ((uint32_t)b << 8) |
                                ((uint32_t)c << 16) | ((uint32_t)d << 24);
        return true;
    }
    bool fromString(const String& s) { return fromString(s.c_str()); }

    // ── String representation ────────────────────────────────────────────────
    String toString() const {
        char buf[46];
        if (IP_IS_V6_VAL(_addr)) {
            const uint8_t* b = (const uint8_t*)_addr.u_addr.ip6.addr;
            snprintf(buf, sizeof(buf),
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
        } else {
            const uint32_t a = _addr.u_addr.ip4.addr;
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
        }
        return String(buf);
    }
};

// INADDR_NONE: BSD sockets constant — lwIP uses IPADDR_NONE instead.
#ifndef INADDR_NONE
#define INADDR_NONE IPADDR_NONE
#endif
