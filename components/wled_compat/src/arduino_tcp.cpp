// arduino_tcp.cpp - the Arduino core's TCP client, over lwIP sockets.

#include "Arduino.h"
#include "NetworkClient.h"

extern "C" {
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
}

NetworkClient::~NetworkClient()
{
    stop();
}

int NetworkClient::connect(const char *host, uint16_t port)
{
    if (!host || !*host) {
        return 0;
    }
    stop();

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    struct addrinfo *result = nullptr;
    if (getaddrinfo(host, service, &hints, &result) != 0 || !result) {
        return 0;
    }

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd >= 0) {
        if (::connect(fd, result->ai_addr, result->ai_addrlen) == 0) {
            _fd = fd;
        } else {
            ::close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(result);
    return fd >= 0 ? 1 : 0;
}

int NetworkClient::connect(IPAddress ip, uint16_t port)
{
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl((uint32_t)ip);

    stop();
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }
    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    _fd = fd;
    return 1;
}

int NetworkClient::connect(uint32_t host, uint16_t port)
{
    return connect(IPAddress(host), port);
}

void NetworkClient::stop()
{
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

bool NetworkClient::connected() const
{
    return _fd >= 0;
}

size_t NetworkClient::write(uint8_t c)
{
    return write(&c, 1);
}

size_t NetworkClient::write(const uint8_t *buffer, size_t size)
{
    if (_fd < 0 || !buffer || !size) {
        return 0;
    }
    ssize_t sent = ::send(_fd, buffer, size, 0);
    return sent < 0 ? 0 : (size_t)sent;
}

int NetworkClient::available()
{
    if (_fd < 0) {
        return 0;
    }
    uint8_t byte = 0;
    ssize_t got = ::recv(_fd, &byte, 1, MSG_PEEK);
    return got > 0 ? 1 : 0;
}

int NetworkClient::read()
{
    if (_fd < 0) {
        return -1;
    }
    uint8_t byte = 0;
    ssize_t got = ::recv(_fd, &byte, 1, 0);
    return got == 1 ? (int)byte : -1;
}

int NetworkClient::read(unsigned char *buffer, size_t len)
{
    if (_fd < 0 || !buffer || !len) {
        return 0;
    }
    ssize_t got = ::recv(_fd, buffer, len, 0);
    return got < 0 ? 0 : (int)got;
}

int NetworkClient::peek()
{
    if (_fd < 0) {
        return -1;
    }
    uint8_t byte = 0;
    ssize_t got = ::recv(_fd, &byte, 1, MSG_PEEK);
    return got == 1 ? (int)byte : -1;
}

void NetworkClient::flush()
{
    // The socket writes straight out; there is nothing buffered here.
}
