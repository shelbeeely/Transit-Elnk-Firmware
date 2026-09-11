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
    // Read the stream directly into response.body rather than
    // http.getString().c_str(): getString() builds a whole separate Arduino
    // String first, so a std::string assignment from it holds two full
    // copies of the body in RAM at once at the moment of the copy. That's
    // fine for the small JSON responses api_client/icon_cache fetch, but
    // sta_client.cpp's STA feed is ~190KB -- a second full copy there is
    // exactly the kind of spike its free-heap guard (kMinFreeHeapBytes) is
    // trying to rule out, so this reads it once instead. WiFiClient::read()
    // is the same primitive FreeInk's own SecureClient wraps for streamed
    // reads (freeink-sdk/libs/network/SecureNet).
    const int contentLength = http.getSize();  // -1 if unknown (chunked)
    if (contentLength > 0) response.body.reserve(static_cast<size_t>(contentLength));

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[512];
    // Absolute cap on top of whatever Content-Length claims: this is
    // untrusted network input, and a malformed/malicious/misbehaving server
    // sending an unbounded or falsely-small Content-Length shouldn't be able
    // to make this loop grow response.body without limit.
    constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;
    // Bounds the "connected but nothing available yet" wait: a server that
    // sends no Content-Length and then keeps the connection open without
    // ever sending more data (or closing it) would otherwise spin here
    // forever — http.connected() alone doesn't guarantee more bytes are
    // coming. Reset on every successful read; only a genuine stall trips it.
    // A battery device blocked here never reaches enterDeepSleep().
    constexpr uint32_t kStallTimeoutMs = 15000;
    uint32_t lastProgressMs = millis();
    while (http.connected() && response.body.size() < kMaxBodyBytes) {
      size_t available = stream->available();
      if (available == 0) {
        if (!http.connected()) break;
        if (millis() - lastProgressMs > kStallTimeoutMs) break;
        delay(1);
        continue;
      }
      size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
      int bytesRead = stream->readBytes(reinterpret_cast<char*>(buffer), toRead);
      if (bytesRead <= 0) break;
      response.body.append(reinterpret_cast<char*>(buffer), static_cast<size_t>(bytesRead));
      lastProgressMs = millis();
      if (contentLength > 0 && response.body.size() >= static_cast<size_t>(contentLength)) break;
    }
  }
  http.end();
  return response;
}

}  // namespace transit
