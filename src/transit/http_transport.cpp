// Minimal working implementation (not a stub — no retry, but functionally
// real) backing HttpTransport for [env:xteink_x4]. Unit 2 may extend this
// (e.g. with cert pinning) or replace WiFiClientSecure with FreeInk's
// SecureNet if plain TLS 1.2 ever proves insufficient — see platformio.ini's
// note on why SecureNet/wolfSSL isn't the default here.

#include "transit/http_transport.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

namespace transit {

namespace {

// Absolute cap on top of whatever Content-Length claims: this is untrusted
// network input, and a malformed/malicious/misbehaving server sending an
// unbounded or falsely-small Content-Length shouldn't be able to grow
// response.body without limit.
constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;

// Bounds the "connected but nothing available yet" wait: a server that
// sends no Content-Length and then keeps the connection open without ever
// sending more data (or closing it) would otherwise spin forever —
// http.connected() alone doesn't guarantee more bytes are coming. Reset on
// every successful read; only a genuine stall trips it. A battery device
// blocked here never reaches enterDeepSleep().
constexpr uint32_t kStallTimeoutMs = 15000;

// Reads the response body once the request has been sent. Streams directly
// into response.body rather than using http.getString().c_str(): getString()
// builds a whole separate Arduino String first, so a std::string assignment
// from it holds two full copies of the body in RAM at the moment of the
// copy. That's fine for the small JSON responses api_client/icon_cache
// fetch, but sta_client.cpp's STA feed is ~190KB — a second full copy there
// is exactly the kind of spike its free-heap guard (kMinFreeHeapBytes) is
// trying to rule out. WiFiClient::read() is the same primitive FreeInk's own
// SecureClient wraps for streamed reads (freeink-sdk/libs/network/SecureNet).
void readBody(HTTPClient& http, HttpResponse& response) {
  const int contentLength = http.getSize();  // -1 if unknown (chunked)
  // Clamped to the same cap the read loop enforces, and deliberately so:
  // Content-Length is attacker-controlled input, exceptions are off on this
  // build (-fno-exceptions), and reserving a bogus multi-gigabyte length
  // would abort the device outright rather than fail the request. That
  // mattered less when every request went to the Transit API over TLS; the
  // captive-portal path talks plain HTTP to whatever is running the
  // network, so it matters now.
  if (contentLength > 0) {
    const size_t reserveBytes =
        static_cast<size_t>(contentLength) > kMaxBodyBytes ? kMaxBodyBytes : static_cast<size_t>(contentLength);
    response.body.reserve(reserveBytes);
  }

  // auto*, not WiFiClient*: this project's pinned Arduino-ESP32 core (3.3.7,
  // see platformio.ini) resolves the networking stack through its newer
  // unified Network/NetworkClientSecure types rather than the classic
  // WiFiClient/WiFiClientSecure names, and getStreamPtr()'s exact return
  // type follows that — available()/readBytes() below are both part of the
  // standard Arduino Stream/Client interface either way, so there's no
  // reason to hardcode a concrete class name here.
  auto* stream = http.getStreamPtr();
  if (stream == nullptr) return;

  uint8_t buffer[512];
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

// The Transit API and the icon CDN are both https://, but captive_portal.h's
// canary probes are deliberately plain http:// (a portal can't intercept TLS
// without a certificate error, so an https probe can't tell "captured" apart
// from "offline"). A WiFiClientSecure handed an http:// URL fails the
// handshake, so the scheme has to pick the client.
//
// The client is chosen from the *requested* URL, which means a redirect
// that changes scheme can't be followed within one request: an http canary
// pointed at an https splash page needs a TLS client the plain-http request
// never created. That's why response.redirectLocation is surfaced even when
// following failed -- the caller reissues it as a fresh request, which
// comes back through here and picks the right client for the new scheme.
bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

// Templated on the client type rather than taking a common base reference:
// WiFiClient/WiFiClientSecure are aliases onto the Arduino-ESP32 3.x
// Network* stack whose exact inheritance this code has no reason to depend
// on. Letting http.begin() resolve its own overload against the concrete
// type keeps the call shape identical to what this file already compiled
// with before the http:// branch existed.
template <typename ClientT>
HttpResponse performRequest(ClientT& client, const std::string& url,
                            const std::vector<std::pair<std::string, std::string>>& headers,
                            const std::string* body) {
  HttpResponse response;

  HTTPClient http;
  if (!http.begin(client, url.c_str())) {
    return response;
  }
  // Captive-portal splash pages almost always arrive via a redirect off the
  // canary URL, and following it here is what lets classifyProbeResponse()
  // see the portal's actual HTML instead of a bare 302 with no body. Harmless
  // for the API/CDN calls, which don't redirect.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  for (const auto& header : headers) {
    http.addHeader(header.first.c_str(), header.second.c_str());
  }

  const int statusCode = body == nullptr ? http.GET() : http.POST(body->c_str());
  response.transportOk = statusCode > 0;
  response.statusCode = statusCode > 0 ? statusCode : 0;
  // Whatever Location was last seen, whether or not the redirect was
  // actually followed -- see HttpResponse::redirectLocation on why
  // captive_portal.h needs it either way.
  response.redirectLocation = http.getLocation().c_str();
  if (response.transportOk) readBody(http, response);
  http.end();
  return response;
}

HttpResponse dispatchRequest(const std::string& url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const std::string* body) {
  if (isHttpsUrl(url)) {
    WiFiClientSecure client;
    // TODO(unit 2): pin the Transit API's CA cert instead of skipping
    // verification once the transit.app CA chain is confirmed.
    client.setInsecure();
    return performRequest(client, url, headers, body);
  }
  WiFiClient client;
  return performRequest(client, url, headers, body);
}

}  // namespace

HttpResponse WifiHttpTransport::get(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers) {
  return dispatchRequest(url, headers, nullptr);
}

HttpResponse WifiHttpTransport::post(const std::string& url,
                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                     const std::string& body) {
  return dispatchRequest(url, headers, &body);
}

}  // namespace transit
