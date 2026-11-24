// arduino_udp.cpp - the Arduino UDP class, over lwIP sockets.
//
// The Arduino core's WiFiUDP is a socket with an Arduino-shaped API.  This is
// the same thing: a lwIP socket that WLED's Art-Net, E1.31 and NTP code can
// use without knowing that there is no radio involved.

#include "Arduino.h"
#include "WiFiUdp.h"

extern "C" {
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "esp_netif.h"
#include "esp_log.h"
}

#include <fcntl.h>
#include <unistd.h>

namespace {

// Where a packet that came in is put until the application reads it.
constexpr size_t RX_BUFFER_SIZE = 1472;

int addrToSockaddr(const IPAddress &addr, uint16_t port, struct sockaddr_in &out)
{
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    out.sin_addr.s_addr = htonl((uint32_t)addr);
    return 1;
}

} // namespace

uint8_t WiFiUDP::begin(uint16_t port)
{
    stop();

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return 0;
    }
    // Broadcast has to be allowed: Art-Net and E1.31 both send to it.
    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return 0;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    _fd = fd;
    _rxLength = 0;
    _rxIndex = 0;
    _localPort = port;
    return 1;
}

void WiFiUDP::stop()
{
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

int WiFiUDP::beginPacket(IPAddress ip, uint16_t port)
{
    struct sockaddr_in addr;
    addrToSockaddr(ip, port, addr);
    _remoteAddr = addr;
    _remoteIP = ip;
    _remotePort = port;
    return 1;
}

int WiFiUDP::beginPacket(const char *host, uint16_t port)
{
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return 0;
    }
    _remoteAddr = addr;
    _remoteIP = IPAddress(addr.sin_addr.s_addr);
    _remotePort = port;
    return 1;
}

int WiFiUDP::beginPacket(uint32_t host, uint16_t port)
{
    return beginPacket(IPAddress(host), port);
}

int WiFiUDP::endPacket()
{
    if (_fd < 0 || _txLength == 0) {
        return 1;
    }
    ssize_t sent = sendto(_fd, _txBuffer, _txLength, 0,
                           (struct sockaddr *)&_remoteAddr, sizeof(_remoteAddr));
    _txLength = 0;
    return sent < 0 ? 0 : 1;
}

int WiFiUDP::parsePacket()
{
    if (_fd < 0) {
        return -1;
    }
    if (_rxIndex < _rxLength) {
        return (int)(_rxLength - _rxIndex);
    }
    struct sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    ssize_t got = recvfrom(_fd, _rxBuffer, sizeof(_rxBuffer), 0,
                           (struct sockaddr *)&from, &fromLen);
    if (got <= 0) {
        return 0;
    }
    _rxLength = (size_t)got;
    _rxIndex = 0;
    _remoteIP = IPAddress(from.sin_addr.s_addr);
    _remotePort = ntohs(from.sin_port);
    return (int)_rxLength;
}

int WiFiUDP::available()
{
    return (int)(_rxLength - _rxIndex);
}

int WiFiUDP::read()
{
    if (_rxIndex >= _rxLength) {
        return -1;
    }
    return (int)_rxBuffer[_rxIndex++];
}

int WiFiUDP::peek()
{
    if (_rxIndex >= _rxLength) {
        return -1;
    }
    return (int)_rxBuffer[_rxIndex];
}

void WiFiUDP::flush()
{
    endPacket();
}

size_t WiFiUDP::write(uint8_t c)
{
    return write(&c, 1);
}

size_t WiFiUDP::write(const uint8_t *buffer, size_t size)
{
    if (!buffer || !size) {
        return 0;
    }
    // The Arduino core buffers the packet until endPacket().
    size_t room = sizeof(_txBuffer) - _txLength;
    size_t n = size < room ? size : room;
    memcpy(_txBuffer + _txLength, buffer, n);
    _txLength += n;
    return size;
}

IPAddress WiFiUDP::remoteIP()
{
    return _remoteIP;
}

uint16_t WiFiUDP::remotePort()
{
    return _remotePort;
}

IPAddress WiFiUDP::destinationIP()
{
    return _remoteIP;
}

uint16_t WiFiUDP::localPort()
{
    return _localPort;
}
