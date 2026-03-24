// WiFiUDP.h — Arduino WiFiUDP shim for pure ESP-IDF builds.
// NOTE: This file must be included AFTER idf_compat.h (which defines IPAddress).
// It is included via wled.h, not via idf_compat.h, to avoid a circular dependency.
#pragma once
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <string.h>
#include <stdlib.h>

class WiFiUDP {
public:
    WiFiUDP() : _sock(-1), _localPort(0) {
        memset(&_remote, 0, sizeof(_remote));
        memset(&_dest,   0, sizeof(_dest));
        _rxBuf = nullptr; _rxLen = 0; _rxOff = 0;
        _txBuf = nullptr; _txLen = 0; _txCap = 0;
    }
    ~WiFiUDP() { stop(); free(_rxBuf); free(_txBuf); }

    uint8_t begin(uint16_t port) { _localPort = port; return _openSocket(); }
    uint8_t beginMulticast(IPAddress multicast, uint16_t port) {
        _localPort = port;
        if (!_openSocket()) return 0;
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = (uint32_t)multicast;
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        return 1;
    }
    void stop() {
        if (_sock >= 0) { closesocket(_sock); _sock = -1; }
        _localPort = 0; _clearRx();
    }
    int parsePacket() {
        if (_sock < 0 && _localPort) _openSocket();
        if (_sock < 0) return 0;
        _clearRx();
        struct sockaddr_in sender; socklen_t slen = sizeof(sender);
        char peek[1];
        if (recvfrom(_sock, peek, 1, MSG_PEEK | MSG_DONTWAIT, (struct sockaddr*)&sender, &slen) <= 0) return 0;
        _rxBuf = (uint8_t*)malloc(1500);
        if (!_rxBuf) return 0;
        slen = sizeof(sender);
        int r = recvfrom(_sock, _rxBuf, 1500, MSG_DONTWAIT, (struct sockaddr*)&sender, &slen);
        if (r <= 0) { free(_rxBuf); _rxBuf = nullptr; return 0; }
        _rxLen = r; _rxOff = 0; _remote = sender;
        return r;
    }
    int read(uint8_t* buf, size_t len) {
        if (!_rxBuf || _rxOff >= _rxLen) return 0;
        size_t n = len < (_rxLen-_rxOff) ? len : (_rxLen-_rxOff);
        memcpy(buf, _rxBuf+_rxOff, n); _rxOff += n; return (int)n;
    }
    int read(char* buf, size_t len) { return read((uint8_t*)buf, len); }
    void flush() {
        _clearRx();
        if (_sock < 0) return;
        char tmp[64]; struct sockaddr_in s; socklen_t sl = sizeof(s);
        while (recvfrom(_sock, tmp, sizeof(tmp), MSG_DONTWAIT, (struct sockaddr*)&s, &sl) > 0) {}
    }
    IPAddress remoteIP()   const { return IPAddress(_remote.sin_addr.s_addr); }
    uint16_t  remotePort() const { return ntohs(_remote.sin_port); }
    uint16_t  localPort()  const { return _localPort; }

    int beginPacket(IPAddress ip, uint16_t port) {
        memset(&_dest, 0, sizeof(_dest));
        _dest.sin_family = AF_INET;
        _dest.sin_addr.s_addr = (uint32_t)ip;
        _dest.sin_port = htons(port);
        _txLen = 0;
        if (_sock < 0) _openSocket();
        return (_sock >= 0) ? 1 : 0;
    }
    size_t write(const uint8_t* buf, size_t len) {
        if (!_txGrow(len)) return 0;
        memcpy(_txBuf+_txLen, buf, len); _txLen += len; return len;
    }
    size_t write(uint8_t b) { return write(&b, 1); }
    int endPacket() {
        if (_sock < 0 || !_txBuf || _txLen == 0) return 0;
        int b = 1; setsockopt(_sock, SOL_SOCKET, SO_BROADCAST, &b, sizeof(b));
        int r = sendto(_sock, _txBuf, _txLen, 0, (struct sockaddr*)&_dest, sizeof(_dest));
        _txLen = 0; return (r > 0) ? 1 : 0;
    }

private:
    int _sock; uint16_t _localPort;
    struct sockaddr_in _remote, _dest;
    uint8_t *_rxBuf, *_txBuf;
    size_t _rxLen, _rxOff, _txLen, _txCap;

    void _clearRx() { free(_rxBuf); _rxBuf = nullptr; _rxLen = _rxOff = 0; }
    bool _txGrow(size_t extra) {
        size_t need = _txLen + extra;
        if (need <= _txCap) return true;
        size_t nc = need < 1500 ? 1500 : need+256;
        uint8_t* p = (uint8_t*)realloc(_txBuf, nc);
        if (!p) return false;
        _txBuf = p; _txCap = nc; return true;
    }
    uint8_t _openSocket() {
        if (_sock >= 0) return 1;
        _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_sock < 0) return 0;
        struct timeval tv = {0,0}; setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int r = 1; setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &r, sizeof(r));
        if (_localPort) {
            struct sockaddr_in a = {}; a.sin_family = AF_INET;
            a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(_localPort);
            if (bind(_sock, (struct sockaddr*)&a, sizeof(a)) < 0) { closesocket(_sock); _sock=-1; return 0; }
        }
        return 1;
    }
};
