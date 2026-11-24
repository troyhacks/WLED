// DNSServer.h - the Arduino DNS server class.
//
// The DNS server answers the captive portal's name requests.  On the P4 there
// is no access point - the board runs Ethernet only - so there is nothing to
// answer for.  The class has the core's shape so that WLED's code compiles,
// and it does nothing.

#pragma once

#include "Arduino.h"
#include "IPAddress.h"

enum class DNSReplyCode : uint16_t {
    NoError = 0,
    FormError = 1,
    ServFail = 2,
    NXDomain = 3,
    NotImp = 4,
    Refused = 5,
};

class DNSServer {
public:
    bool start();
    bool start(uint16_t port, const String &domainName, const IPAddress &resolvedIP);
    void stop();
    void processNextRequest();
    void setErrorReplyCode(const DNSReplyCode &replyCode);
};
