#pragma once
// WiFiClient shim for pure IDF builds — backed by POSIX TCP sockets (lwIP).
// Provides the subset of the Arduino WiFiClient API used by pioneer_prolink and similar.

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "idf_shims/IPAddress.h"

class WiFiClient {
public:
    WiFiClient() : _sock(-1) {}
    ~WiFiClient() { stop(); }

    bool connect(IPAddress ip, uint16_t port) {
        stop();
        _sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_sock < 0) return false;
        int flag = 1;
        ::setsockopt(_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        struct timeval tv = { 5, 0 };  // 5-second connect/read timeout
        ::setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        addr.sin_addr.s_addr = htonl(
            ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
            ((uint32_t)ip[2] <<  8) |  (uint32_t)ip[3]);
        if (::connect(_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            ::close(_sock); _sock = -1; return false;
        }
        return true;
    }

    void stop() {
        if (_sock >= 0) { ::close(_sock); _sock = -1; }
    }

    bool connected() {
        if (_sock < 0) return false;
        // Peek with MSG_DONTWAIT — if recv returns 0, peer closed.
        char b; int r = ::recv(_sock, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) { stop(); return false; }
        return true;
    }

    int available() {
        if (_sock < 0) return 0;
        fd_set fds; FD_ZERO(&fds); FD_SET(_sock, &fds);
        struct timeval tv = {0, 0};
        return ::select(_sock + 1, &fds, nullptr, nullptr, &tv) > 0 ? 1 : 0;
    }

    int peek() {
        if (_sock < 0) return -1;
        uint8_t b; return (::recv(_sock, &b, 1, MSG_PEEK) == 1) ? (int)b : -1;
    }

    int read() {
        if (_sock < 0) return -1;
        uint8_t b; return (::recv(_sock, &b, 1, 0) == 1) ? (int)b : -1;
    }

    int read(uint8_t* buf, int len) {
        if (_sock < 0 || !buf || len <= 0) return -1;
        return (int)::recv(_sock, buf, (size_t)len, 0);
    }

    size_t write(uint8_t b) {
        if (_sock < 0) return 0;
        return (::send(_sock, &b, 1, 0) == 1) ? 1 : 0;
    }

    size_t write(const uint8_t* buf, size_t len) {
        if (_sock < 0 || !buf) return 0;
        int sent = ::send(_sock, buf, len, 0);
        return (sent < 0) ? 0 : (size_t)sent;
    }

    // Convenience: write a const char* (no null terminator)
    size_t write(const char* buf, size_t len) {
        return write((const uint8_t*)buf, len);
    }

private:
    int _sock;
};
