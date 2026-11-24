// WiFiUdp.h - the Arduino UDP class, over lwIP sockets.
//
// The Arduino core's WiFiUDP is a thin wrapper over a UDP socket.  So is
// this one, and it does not need a radio: it is a socket on the board.

#pragma once

#include "Arduino.h"
#include "IPAddress.h"
#include "Stream.h"

extern "C" {
#include "lwip/sockets.h"
}

class WiFiUDP : public Stream {
public:
    uint8_t begin(uint16_t port);
    void stop();
    int beginPacket(IPAddress ip, uint16_t port);
    int beginPacket(const char *host, uint16_t port);
    int beginPacket(uint32_t host, uint16_t port);
    int endPacket();
    int parsePacket();
    int available();
    int read();
    int peek();
    void flush();
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    IPAddress remoteIP();
    uint16_t remotePort();
    IPAddress destinationIP();
    uint16_t localPort();

private:
    int _fd = -1;
    uint8_t _rxBuffer[1472];
    size_t _rxLength = 0;
    size_t _rxIndex = 0;
    uint8_t _txBuffer[1472];
    size_t _txLength = 0;
    struct sockaddr_in _remoteAddr;
    IPAddress _remoteIP;
    uint16_t _remotePort = 0;
    uint16_t _localPort = 0;
};
