// Minimal working implementation (not a stub — GET-only, no retry/streaming,
// but functionally real) backing HttpTransport for [env:xteink_x4]. Unit 2
// may extend this (e.g. with cert pinning) or replace WiFiClientSecure with
// FreeInk's SecureNet if plain TLS 1.2 ever proves insufficient — see
// platformio.ini's note on why SecureNet/wolfSSL isn't the default here.

#include "transit/http_transport.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace transit {

HttpResponse WifiHttpTransport::get(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers) {
  HttpResponse response;

  WiFiClientSecure client;
  // TODO(unit 2): pin the Transit API's CA cert instead of skipping
  // verification once the transit.app CA chain is confirmed.
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url.c_str())) {
    return response;
  }
  for (const auto& header : headers) {
    http.addHeader(header.first.c_str(), header.second.c_str());
  }

  int statusCode = http.GET();
  response.transportOk = statusCode > 0;
  response.statusCode = statusCode > 0 ? statusCode : 0;
  if (response.transportOk) {
    response.body = http.getString().c_str();
  }
  http.end();
  return response;
}

}  // namespace transit
