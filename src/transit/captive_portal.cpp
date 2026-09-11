// Captive-portal detection and auto-login — see
// include/transit/captive_portal.h for the contract and for why form
// discovery plus a manual override is the honest shape for this feature.

#include "transit/captive_portal.h"

#include <cctype>

namespace transit {

// All four answer an uncaptured request with a bare HTTP 204 and no body,
// which is what makes classifyProbeResponse() below a one-line judgement.
// Deliberately not Apple's /hotspot-detect.html or Microsoft's
// /connecttest.txt: those answer 200 with a specific body, so each would
// need its own expected-content check, and a portal serving a 200 splash
// page would be indistinguishable from success without one. Two operators
// (Google, Cloudflare) rather than four Google hostnames, so one provider
// being blocked on a given network doesn't take out the whole list.
const char* const kDefaultProbeUrls[] = {
    "http://connectivitycheck.gstatic.com/generate_204",
    "http://cp.cloudflare.com/generate_204",
    "http://clients3.google.com/generate_204",
};
const int kDefaultProbeUrlCount = 3;

namespace {

char lowerAscii(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string toLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = lowerAscii(c);
  return out;
}

std::string toUpper(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// Case-insensitive find, since HTML tag/attribute names are not
// case-sensitive and portal pages are hand-written enough to prove it.
size_t findCaseInsensitive(const std::string& haystack, const std::string& needle, size_t from) {
  if (needle.empty() || needle.size() > haystack.size()) return std::string::npos;
  const std::string lowerHay = toLower(haystack);
  return lowerHay.find(toLower(needle), from);
}

bool isHtmlNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ':' || c == '.';
}

// Reads one attribute's value out of a tag body, e.g. attributeValue(
// "input type=\"email\" name=q", "name") -> "q". Handles double-quoted,
// single-quoted, and bare values. Returns false when the attribute isn't
// present (distinct from present-but-empty, which matters for a valueless
// checkbox -- see buildRequestBody()).
bool attributeValue(const std::string& tag, const std::string& attribute, std::string& out) {
  size_t pos = 0;
  while (pos < tag.size()) {
    const size_t found = findCaseInsensitive(tag, attribute, pos);
    if (found == std::string::npos) return false;
    pos = found + attribute.size();

    // Must be a whole attribute name, not a suffix of a longer one
    // ("name" must not match inside "formname" or "names").
    const bool leftOk = found == 0 || !isHtmlNameChar(tag[found - 1]);
    size_t after = pos;
    while (after < tag.size() && std::isspace(static_cast<unsigned char>(tag[after]))) ++after;
    const bool rightOk = after < tag.size() && tag[after] == '=';
    if (!leftOk || !rightOk) continue;

    ++after;  // past '='
    while (after < tag.size() && std::isspace(static_cast<unsigned char>(tag[after]))) ++after;
    if (after >= tag.size()) {
      out.clear();
      return true;
    }

    const char quote = tag[after];
    if (quote == '"' || quote == '\'') {
      const size_t end = tag.find(quote, after + 1);
      if (end == std::string::npos) {
        out = tag.substr(after + 1);
      } else {
        out = tag.substr(after + 1, end - after - 1);
      }
      return true;
    }
    size_t end = after;
    while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end]))) ++end;
    out = tag.substr(after, end - after);
    return true;
  }
  return false;
}

// Whether an <input>'s type/name suggests it's where the user's email or
// phone number goes. Checked in that order: an explicit type="email" or
// type="tel" is a far stronger signal than a name that merely contains a
// suggestive substring.
bool looksLikeIdentityField(const std::string& type, const std::string& name) {
  const std::string t = toLower(type);
  if (t == "email" || t == "tel") return true;
  if (t == "hidden" || t == "checkbox" || t == "radio" || t == "submit" || t == "button" ||
      t == "reset" || t == "image" || t == "password") {
    return false;
  }
  const std::string n = toLower(name);
  static const char* const kHints[] = {"email", "e-mail", "mail", "phone", "tel",
                                       "mobile", "msisdn", "username", "user", "login"};
  for (const char* hint : kHints) {
    if (n.find(hint) != std::string::npos) return true;
  }
  return false;
}

// Splits "http://host/a/b?q" into scheme+host ("http://host") and path
// ("/a/b"). Used by resolveUrl() only.
void splitUrl(const std::string& url, std::string& origin, std::string& path) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) {
    origin.clear();
    path = url;
    return;
  }
  const size_t hostStart = schemeEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) {
    origin = url;
    path = "/";
    return;
  }
  origin = url.substr(0, hostEnd);
  path = url.substr(hostEnd);
}

}  // namespace

NetworkReachability classifyProbeResponse(const HttpResponse& response) {
  if (!response.transportOk || response.statusCode == 0) return NetworkReachability::kUnreachable;
  // The canaries in kDefaultProbeUrls all answer 204/empty when nothing is
  // in the way. Anything else -- a redirect to a splash page, a 200 with
  // HTML, an error page from a proxy -- means something answered on their
  // behalf.
  if (response.statusCode == 204 && response.body.empty()) return NetworkReachability::kOpen;
  return NetworkReachability::kCaptured;
}

std::string urlEncodeForm(const std::string& value) {
  static const char* const kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

std::string resolveUrl(const std::string& pageUrl, const std::string& reference) {
  if (reference.empty()) return pageUrl;
  if (reference.find("://") != std::string::npos) return reference;

  std::string origin;
  std::string path;
  splitUrl(pageUrl, origin, path);

  if (reference.rfind("//", 0) == 0) {
    const size_t schemeEnd = pageUrl.find("://");
    const std::string scheme = schemeEnd == std::string::npos ? std::string("http") : pageUrl.substr(0, schemeEnd);
    return scheme + ":" + reference;
  }
  if (reference[0] == '/') return origin + reference;
  if (reference[0] == '?') {
    const size_t queryStart = path.find('?');
    const std::string basePath = queryStart == std::string::npos ? path : path.substr(0, queryStart);
    return origin + basePath + reference;
  }

  // Plain relative: replace the last path segment.
  const size_t queryStart = path.find('?');
  std::string basePath = queryStart == std::string::npos ? path : path.substr(0, queryStart);
  const size_t lastSlash = basePath.rfind('/');
  basePath = lastSlash == std::string::npos ? std::string("/") : basePath.substr(0, lastSlash + 1);
  return origin + basePath + reference;
}

bool discoverLoginForm(const std::string& html, const std::string& pageUrl, LoginForm& out) {
  const size_t formStart = findCaseInsensitive(html, "<form", 0);
  if (formStart == std::string::npos) return false;
  const size_t formTagEnd = html.find('>', formStart);
  if (formTagEnd == std::string::npos) return false;

  const std::string formTag = html.substr(formStart + 1, formTagEnd - formStart - 1);

  LoginForm form;
  std::string action;
  if (attributeValue(formTag, "action", action) && !action.empty()) {
    form.action = resolveUrl(pageUrl, action);
  } else {
    // An action-less form posts back to the page it came from -- the HTML
    // spec's own default, and a very common shape for these splash pages.
    form.action = pageUrl;
  }

  std::string method;
  form.method = attributeValue(formTag, "method", method) && !method.empty() ? toUpper(method) : std::string("POST");

  // Only the first form is considered (see the header's known limits), so
  // stop at its closing tag rather than sweeping up inputs from a search
  // box further down the page.
  size_t formEnd = findCaseInsensitive(html, "</form", formTagEnd);
  if (formEnd == std::string::npos) formEnd = html.size();

  size_t cursor = formTagEnd;
  while (cursor < formEnd) {
    const size_t inputStart = findCaseInsensitive(html, "<input", cursor);
    if (inputStart == std::string::npos || inputStart >= formEnd) break;
    size_t inputEnd = html.find('>', inputStart);
    if (inputEnd == std::string::npos) break;
    const std::string inputTag = html.substr(inputStart + 1, inputEnd - inputStart - 1);
    cursor = inputEnd + 1;

    std::string name;
    if (!attributeValue(inputTag, "name", name) || name.empty()) continue;  // unnamed inputs aren't submitted

    std::string type;
    attributeValue(inputTag, "type", type);
    if (toLower(type) == "reset") continue;  // a browser never submits these

    std::string value;
    const bool hasValue = attributeValue(inputTag, "value", value);
    if (!hasValue) value.clear();

    if (form.identityField.empty() && looksLikeIdentityField(type, name)) {
      form.identityField = name;
      value.clear();  // whatever placeholder was there is about to be replaced
    } else if (!hasValue && toLower(type) == "checkbox") {
      // A checked checkbox with no value attribute submits "on" -- which is
      // what a terms-acceptance box wants, and accepting the terms is the
      // whole point of pressing Connect.
      value = "on";
    }

    form.fields.emplace_back(name, value);
  }

  out = form;
  return true;
}

std::string buildRequestBody(const LoginForm& form, const std::string& identity) {
  std::string body;
  bool identityWritten = false;
  for (const auto& field : form.fields) {
    if (!body.empty()) body += '&';
    body += urlEncodeForm(field.first);
    body += '=';
    if (!form.identityField.empty() && field.first == form.identityField) {
      body += urlEncodeForm(identity);
      identityWritten = true;
    } else {
      body += urlEncodeForm(field.second);
    }
  }
  // A manually-configured form (no discovered inputs) still has to carry the
  // identity somewhere -- append it rather than POSTing an empty body.
  if (!identityWritten && !form.identityField.empty()) {
    if (!body.empty()) body += '&';
    body += urlEncodeForm(form.identityField);
    body += '=';
    body += urlEncodeForm(identity);
  }
  return body;
}

CaptivePortalClient::CaptivePortalClient(HttpTransport& transport) : transport_(transport) {}

ProbeOutcome CaptivePortalClient::probe() {
  ProbeOutcome outcome;
  for (int i = 0; i < kDefaultProbeUrlCount; ++i) {
    const std::string url = kDefaultProbeUrls[i];
    const HttpResponse response = transport_.get(url, {});
    const NetworkReachability verdict = classifyProbeResponse(response);
    if (verdict == NetworkReachability::kOpen) {
      outcome.reachability = verdict;
      outcome.probeUrl = url;
      return outcome;
    }
    if (verdict == NetworkReachability::kCaptured) {
      outcome.reachability = verdict;
      outcome.probeUrl = url;
      // The splash page lives wherever the redirect pointed, not at the
      // canary. Everything downstream resolves relative URLs against
      // pageUrl for exactly that reason.
      outcome.pageUrl =
          response.redirectLocation.empty() ? url : resolveUrl(url, response.redirectLocation);
      outcome.body = response.body;
      return outcome;
    }
    // kUnreachable: one host being unreachable isn't proof the network is
    // -- keep trying the others before concluding anything.
  }
  return outcome;
}

CaptivePortalResult CaptivePortalClient::connect(const CaptivePortalConfig& config) {
  CaptivePortalResult result;

  const ProbeOutcome initial = probe();
  if (initial.reachability == NetworkReachability::kOpen) {
    result.online = true;
    result.alreadyOpen = true;
    result.message = "Online";
    return result;
  }
  if (initial.reachability == NetworkReachability::kUnreachable) {
    result.message = "No internet on this network";
    return result;
  }

  if (config.identity.empty()) {
    result.message = "Portal sign-in needed (no email/phone saved)";
    return result;
  }

  LoginForm form;
  bool haveForm = false;

  if (!config.overrideSubmitUrl.empty()) {
    // A URL read off a real capture beats anything inferred from the page,
    // so it's checked first and discovery is never even attempted.
    form.action = config.overrideSubmitUrl;
    form.method = "POST";
    form.identityField = config.overrideFieldName.empty() ? std::string("email") : config.overrideFieldName;
    haveForm = true;
  } else {
    // The probe that came back captured usually handed us the splash page
    // body already. Re-fetch only when it didn't -- an empty-bodied
    // redirect the transport couldn't follow, which is what an http canary
    // pointed at an https splash page looks like. Reissuing pageUrl as its
    // own request gets a TLS client for it.
    std::string pageUrl = initial.pageUrl.empty() ? initial.probeUrl : initial.pageUrl;
    std::string html = initial.body;
    if (html.empty() && !pageUrl.empty()) {
      const HttpResponse page = transport_.get(pageUrl, {});
      html = page.body;
      if (!page.redirectLocation.empty()) pageUrl = resolveUrl(pageUrl, page.redirectLocation);
    }
    // Resolved against the page the HTML actually came from. Using the
    // canary URL here would send a relative "/login" action to
    // connectivitycheck.gstatic.com instead of to the portal.
    haveForm = discoverLoginForm(html, pageUrl, form);
    if (haveForm && form.identityField.empty()) {
      if (!config.overrideFieldName.empty()) {
        form.identityField = config.overrideFieldName;
      } else {
        // The page has a form but nothing that reads as an email/phone
        // input. Submitting it without the identity would just bounce, and
        // guessing a field name is exactly what this module refuses to do.
        result.message = "Portal form not recognized - set the field name in settings";
        return result;
      }
    }
  }

  if (!haveForm) {
    result.message = "Portal page has no sign-in form to submit";
    return result;
  }

  const std::string body = buildRequestBody(form, config.identity);
  const std::vector<std::pair<std::string, std::string>> headers = {
      {"Content-Type", "application/x-www-form-urlencoded"}};

  if (form.method == "GET") {
    const std::string separator = form.action.find('?') == std::string::npos ? "?" : "&";
    transport_.get(form.action + separator + body, {});
  } else {
    transport_.post(form.action, headers, body);
  }

  // The submission's own status code is not evidence: portals return 200
  // for a rejected login as readily as an accepted one. Only a clean canary
  // afterward proves the network actually opened.
  result.online = probe().reachability == NetworkReachability::kOpen;
  result.message = result.online ? "Signed in to bus Wi-Fi" : "Bus Wi-Fi sign-in did not go through";
  return result;
}

}  // namespace transit
