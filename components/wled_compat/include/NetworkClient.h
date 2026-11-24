// NetworkClient.h - the Arduino core's TCP client.
//
// The core has it in libraries/Network/src/NetworkClient.cpp, on top of a
// socket.  This is the same thing: a lwIP socket with the Arduino's spelling.
//
// The Pioneer ProLink usermod talks to the deck's database server over it.

#pragma once

#include "Arduino.h"
#include "Print.h"
#include "Stream.h"

class NetworkClient : public Stream {
public:
    NetworkClient() = default;
    ~NetworkClient() override;

    int connect(const char *host, uint16_t port);
    int connect(IPAddress ip, uint16_t port);
    int connect(uint32_t host, uint16_t port);
    void stop();
    bool connected() const;

    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    int available() override;
    int read() override;
    int read(unsigned char *buffer, size_t len);
    int peek() override;
    void flush() override;

    operator bool() const { return _fd >= 0; }

    // Overriding write() hides the ones Print has, and the Arduino core
    // brings them back the same way.
    using Print::write;

private:
    int _fd = -1;
};

typedef NetworkClient WiFiClient;
