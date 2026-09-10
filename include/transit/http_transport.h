#pragma once

// Transit-Elnk-Firmware — HttpTransport implementation for [env:xteink_x4].
//
// Uses the Arduino-ESP32 core's own WiFiClientSecure/HTTPClient rather than
// FreeInk's SecureNet (opt-in wolfSSL, meant for TLS-1.3-only servers —
// external.transitapp.com is a standard TLS 1.2 REST API). Shared by the
// API client (unit 2) and icon cache (unit 6).
//
// Hardware-dependent — only buildable under [env:xteink_x4], not [env:native].

#include "transit/api_client.h"

namespace transit {

class WifiHttpTransport : public HttpTransport {
 public:
  HttpResponse get(const std::string& url,
                    const std::vector<std::pair<std::string, std::string>>& headers) override;
};

}  // namespace transit
