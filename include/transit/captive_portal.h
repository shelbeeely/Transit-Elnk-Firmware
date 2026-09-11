#pragma once

// Transit-Elnk-Firmware — joining an open Wi-Fi network that puts a captive
// portal in front of the internet (the onboard Wi-Fi on a transit vehicle
// being the case this was built for).
//
// The problem this solves: WiFi.begin() to an open SSID reports
// WL_CONNECTED long before there is any actual internet. A captive portal
// sits in the middle and answers every request with its own splash page
// until something submits the form on it. From the firmware's point of
// view an un-dismissed portal looks exactly like a working network that
// returns garbage for every API call.
//
// The approach here is the same one every phone OS uses, run in reverse of
// how setup_flow.cpp uses it: request a URL whose correct answer is known
// in advance (a "canary" — HTTP 204 with an empty body), and if the answer
// is anything else, something is intercepting. See kDefaultProbeUrls below;
// they're the same probe URLs setup_flow.cpp's own portal answers when this
// board is the one hosting the AP.
//
// On the submission side, there is a hard limit on what can be built
// without a live capture of a specific portal, and this module is shaped
// around that honestly rather than guessing at one vendor's form:
//
//   * discoverLoginForm() parses the splash page's own HTML for its <form>
//     action/method and input fields. For the common "type your email and
//     press Connect" portal this is enough to log in with no per-network
//     configuration at all, because the page tells you what it wants.
//   * When discovery isn't enough (a portal that builds its form in
//     JavaScript, or wants a field this can't infer), ConfigStore carries a
//     manual override — a submit URL and field name the user reads off one
//     capture — and that wins over anything discovered.
//
// Either way the firmware verifies by re-probing afterward and reports
// whether the internet actually opened up, instead of assuming a 200 on the
// login POST meant success.
//
// Pure logic plus HttpTransport calls only — no WiFi/Arduino dependency, so
// all of it builds and is unit-tested under [env:native]. Associating to the
// SSID itself is the caller's job (main.cpp), since that genuinely does need
// the radio.

#include <string>
#include <vector>

#include "transit/api_client.h"

namespace transit {

// Canary URLs, in the order they're tried. Each is a well-known endpoint
// whose uncaptured response is an empty HTTP 204 -- the same contract
// setup_flow.cpp's probeUrls[] honors from the other side. Plain http://,
// not https://: a portal can't transparently intercept TLS without a
// certificate error, so an https probe can't distinguish "captured" from
// "offline."
extern const char* const kDefaultProbeUrls[];
extern const int kDefaultProbeUrlCount;

enum class NetworkReachability {
  // Canary answered exactly as expected -- real internet.
  kOpen,
  // Canary answered, but not the way it should have: a redirect, or a 200
  // with a body. Something is in the middle.
  kCaptured,
  // No answer at all (DNS/connect failure). Not a portal, just no network.
  kUnreachable,
};

// Classifies a single canary response. Public because it's the one piece of
// this module's judgement most worth testing directly.
NetworkReachability classifyProbeResponse(const HttpResponse& response);

// A login form as read off a portal's splash page, or as configured by
// hand. `action` is absolute by the time discoverLoginForm() returns it.
struct LoginForm {
  std::string action;
  std::string method = "POST";  // uppercased
  // Every input the form carries, in document order, as name/value pairs.
  // Hidden inputs keep their value (a portal's CSRF/session token lives
  // here); the field chosen to receive the user's identity is recorded
  // separately below rather than filled in here, so buildRequestBody() can
  // substitute it without re-parsing.
  std::vector<std::pair<std::string, std::string>> fields;
  // Name of the field the user's email/phone belongs in -- the first input
  // whose type or name looks like an identity field. Empty when the page
  // had no such input, which is itself a useful signal (that portal wants
  // something this can't supply).
  std::string identityField;
};

// Percent-encodes for application/x-www-form-urlencoded (space as '+',
// everything outside the unreserved set as %XX).
std::string urlEncodeForm(const std::string& value);

// Resolves a possibly-relative form action against the page it came from.
// Handles absolute URLs (returned unchanged), protocol-relative ("//host/x"),
// root-relative ("/login"), and plain relative ("login") paths.
std::string resolveUrl(const std::string& pageUrl, const std::string& reference);

// Best-effort scrape of the first <form> in html. Returns false when the
// page has no form at all. Deliberately a small tag scanner rather than a
// real HTML parser: portal splash pages are tiny and hand-written, and
// pulling in a parser for this would cost more flash than the whole
// feature. Known limits (documented in docs/OFFLINE_AND_BUS_WIFI.md):
// forms built at runtime by JavaScript are invisible to this, and only the
// first form on the page is considered.
bool discoverLoginForm(const std::string& html, const std::string& pageUrl, LoginForm& out);

// Builds an application/x-www-form-urlencoded body from a form, putting
// identity into form.identityField (when set) and leaving every other
// field at the value the page supplied. Checkbox-style terms-acceptance
// inputs that the page left valueless are sent as "on", which is what a
// browser would send for a checked box.
std::string buildRequestBody(const LoginForm& form, const std::string& identity);

// What the caller knows before any of this runs.
struct CaptivePortalConfig {
  // The user's email or phone -- whatever the portal's one field wants.
  std::string identity;
  // Manual overrides from one live capture, used when form discovery isn't
  // enough. Empty = rely on discovery. See the header comment.
  std::string overrideSubmitUrl;
  std::string overrideFieldName;
};

struct CaptivePortalResult {
  // True only when a probe AFTER the login attempt came back kOpen. A 200
  // on the submission itself proves nothing -- a portal happily returns 200
  // for a rejected login.
  bool online = false;
  // Set when the network was already open and nothing had to be done.
  bool alreadyOpen = false;
  // Short, user-facing explanation for the board's status line, ASCII only
  // (the bundled Noto Sans subset renders non-ASCII as tofu -- see
  // render_engine.cpp).
  std::string message;
};

// Runs the whole sequence against an injected transport: probe, and if
// captured, fetch the portal page, work out the form, submit it, re-probe.
// Never throws and never blocks indefinitely -- every step is one transport
// call, and the transport owns its own timeouts.
// What one round of canary probing found.
struct ProbeOutcome {
  NetworkReachability reachability = NetworkReachability::kUnreachable;
  // The canary that produced the verdict.
  std::string probeUrl;
  // Where the splash page actually lives, once the probe's redirect (if
  // any) is resolved against probeUrl. This -- not probeUrl -- is what a
  // relative form action must be resolved against, or the login POST ends
  // up aimed at the canary's own host instead of the portal.
  std::string pageUrl;
  // The splash page body, when the probe came back carrying one. Empty
  // when the redirect couldn't be followed (an http canary pointed at an
  // https page); the caller then fetches pageUrl itself.
  std::string body;
};

class CaptivePortalClient {
 public:
  explicit CaptivePortalClient(HttpTransport& transport);

  // Probes the canaries in order and stops at the first definite answer
  // (kOpen or kCaptured); kUnreachable only when every probe fails -- one
  // blocked host is not a dead network.
  ProbeOutcome probe();

  CaptivePortalResult connect(const CaptivePortalConfig& config);

 private:
  HttpTransport& transport_;
};

}  // namespace transit
