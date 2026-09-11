// Host-side tests for captive_portal.h: canary classification, URL
// resolution, form scraping, body building, and the whole connect()
// sequence driven by a scripted transport.
//
// The scripted transport matters more than usual here. The single most
// important behavior in this module is that connect() only reports success
// when a probe AFTER the login attempt comes back clean -- a portal returns
// 200 for a rejected sign-in just as readily as an accepted one, so
// trusting the POST's status code would make the feature silently claim to
// work on every network it fails on.

#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "transit/captive_portal.h"

using transit::CaptivePortalClient;
using transit::CaptivePortalConfig;
using transit::CaptivePortalResult;
using transit::HttpResponse;
using transit::HttpTransport;
using transit::LoginForm;
using transit::NetworkReachability;
using transit::ProbeOutcome;
using transit::buildRequestBody;
using transit::classifyProbeResponse;
using transit::discoverLoginForm;
using transit::kDefaultProbeUrls;
using transit::resolveUrl;
using transit::urlEncodeForm;

namespace {

HttpResponse ok204() {
  HttpResponse r;
  r.transportOk = true;
  r.statusCode = 204;
  return r;
}

HttpResponse splash(const std::string& html) {
  HttpResponse r;
  r.transportOk = true;
  r.statusCode = 200;
  r.body = html;
  return r;
}

HttpResponse dead() { return HttpResponse{}; }

// A transport that answers probes according to a flag the test flips, and
// records every request it saw.
//
// Probes answer the way a real portal does: a redirect to the portal's own
// splash page, surfaced through HttpResponse::redirectLocation. That detail
// is the whole reason the login POST has to be aimed at the portal's host
// rather than the canary's, so the fake has to reproduce it.
class ScriptedTransport : public HttpTransport {
 public:
  HttpResponse get(const std::string& url,
                   const std::vector<std::pair<std::string, std::string>>& /*headers*/) override {
    getUrls.push_back(url);
    if (isProbe(url)) {
      if (probeOpen) return ok204();
      HttpResponse r = splash(followRedirect ? splashHtml : "");
      r.statusCode = followRedirect ? 200 : 302;
      r.redirectLocation = portalPageUrl;
      return r;
    }
    if (url == portalPageUrl) return splash(splashHtml);
    auto it = pages.find(url);
    return it == pages.end() ? dead() : splash(it->second);
  }

  HttpResponse post(const std::string& url,
                    const std::vector<std::pair<std::string, std::string>>& /*headers*/,
                    const std::string& body) override {
    postUrls.push_back(url);
    postBodies.push_back(body);
    if (openOnLogin) probeOpen = true;
    HttpResponse r;
    r.transportOk = true;
    r.statusCode = 200;
    return r;
  }

  static bool isProbe(const std::string& url) {
    for (int i = 0; i < transit::kDefaultProbeUrlCount; ++i) {
      if (url == kDefaultProbeUrls[i]) return true;
    }
    return false;
  }

  bool probeOpen = false;
  bool openOnLogin = false;
  // false models the case the transport couldn't follow the redirect at
  // all -- an http canary pointed at an https splash page. The client must
  // then fetch the page itself rather than giving up with an empty body.
  bool followRedirect = true;
  std::string portalPageUrl = "http://portal.bus.example/welcome/index.html";
  std::string splashHtml;
  std::map<std::string, std::string> pages;
  std::vector<std::string> getUrls;
  std::vector<std::string> postUrls;
  std::vector<std::string> postBodies;
};

const char kSimpleSplash[] =
    "<html><body><h1>Welcome aboard</h1>"
    "<form action=\"/login\" method=\"POST\">"
    "<input type=\"hidden\" name=\"token\" value=\"abc123\">"
    "<input type=\"email\" name=\"user_email\" placeholder=\"you@example.com\">"
    "<input type=\"checkbox\" name=\"accept_terms\">"
    "<input type=\"submit\" name=\"go\" value=\"Connect\">"
    "</form></body></html>";

// --- classifyProbeResponse -------------------------------------------------

void test_classify_probe_response() {
  TEST_ASSERT_TRUE(classifyProbeResponse(ok204()) == NetworkReachability::kOpen);
  TEST_ASSERT_TRUE(classifyProbeResponse(splash("<html>portal</html>")) == NetworkReachability::kCaptured);
  TEST_ASSERT_TRUE(classifyProbeResponse(dead()) == NetworkReachability::kUnreachable);

  // A 204 that somehow carries a body isn't the canary's real answer.
  HttpResponse chatty = ok204();
  chatty.body = "hi";
  TEST_ASSERT_TRUE(classifyProbeResponse(chatty) == NetworkReachability::kCaptured);

  // An unfollowed redirect is interception too.
  HttpResponse redirect;
  redirect.transportOk = true;
  redirect.statusCode = 302;
  TEST_ASSERT_TRUE(classifyProbeResponse(redirect) == NetworkReachability::kCaptured);
}

// --- urlEncodeForm / resolveUrl -------------------------------------------

void test_url_encode_form() {
  TEST_ASSERT_EQUAL_STRING("rider%40example.com", urlEncodeForm("rider@example.com").c_str());
  TEST_ASSERT_EQUAL_STRING("a+b", urlEncodeForm("a b").c_str());
  TEST_ASSERT_EQUAL_STRING("safe-_.~", urlEncodeForm("safe-_.~").c_str());
  TEST_ASSERT_EQUAL_STRING("%2B1509%25", urlEncodeForm("+1509%").c_str());
}

void test_resolve_url() {
  const std::string page = "http://portal.example.com/welcome/index.html?x=1";
  TEST_ASSERT_EQUAL_STRING("https://other.example/a", resolveUrl(page, "https://other.example/a").c_str());
  TEST_ASSERT_EQUAL_STRING("http://other.example/a", resolveUrl(page, "//other.example/a").c_str());
  TEST_ASSERT_EQUAL_STRING("http://portal.example.com/login", resolveUrl(page, "/login").c_str());
  TEST_ASSERT_EQUAL_STRING("http://portal.example.com/welcome/login", resolveUrl(page, "login").c_str());
  TEST_ASSERT_EQUAL_STRING("http://portal.example.com/welcome/index.html?y=2",
                           resolveUrl(page, "?y=2").c_str());
  // An empty action means "post back to this page" (the HTML default).
  TEST_ASSERT_EQUAL_STRING(page.c_str(), resolveUrl(page, "").c_str());
}

// --- discoverLoginForm ----------------------------------------------------

void test_discover_form_reads_action_method_and_fields() {
  LoginForm form;
  TEST_ASSERT_TRUE(discoverLoginForm(kSimpleSplash, "http://portal.example.com/", form));

  TEST_ASSERT_EQUAL_STRING("http://portal.example.com/login", form.action.c_str());
  TEST_ASSERT_EQUAL_STRING("POST", form.method.c_str());
  TEST_ASSERT_EQUAL_STRING("user_email", form.identityField.c_str());
  TEST_ASSERT_EQUAL_size_t(4, form.fields.size());
  TEST_ASSERT_EQUAL_STRING("token", form.fields[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("abc123", form.fields[0].second.c_str());
  // A valueless checkbox submits "on", the way a checked box would.
  TEST_ASSERT_EQUAL_STRING("accept_terms", form.fields[2].first.c_str());
  TEST_ASSERT_EQUAL_STRING("on", form.fields[2].second.c_str());
  TEST_ASSERT_EQUAL_STRING("Connect", form.fields[3].second.c_str());
}

void test_discover_form_handles_messy_html() {
  // Uppercase tags, single quotes, a bare attribute value, an unnamed
  // input, a reset button, and no method attribute at all.
  const char* html =
      "<HTML><BODY><FORM ACTION='signin.php'>"
      "<INPUT TYPE=hidden NAME=sess VALUE='z9'>"
      "<input name='emailAddress'>"
      "<input type=\"reset\" name=\"clear\" value=\"Clear\">"
      "<input value=\"orphan\">"
      "</FORM></BODY></HTML>";
  LoginForm form;
  TEST_ASSERT_TRUE(discoverLoginForm(html, "http://1.1.1.1/portal/start", form));

  TEST_ASSERT_EQUAL_STRING("http://1.1.1.1/portal/signin.php", form.action.c_str());
  TEST_ASSERT_EQUAL_STRING("POST", form.method.c_str());  // default when unspecified
  TEST_ASSERT_EQUAL_STRING("emailAddress", form.identityField.c_str());
  // sess and emailAddress only: the reset button and the unnamed input are
  // both things a browser would never submit.
  TEST_ASSERT_EQUAL_size_t(2, form.fields.size());
  TEST_ASSERT_EQUAL_STRING("z9", form.fields[0].second.c_str());
}

void test_discover_form_does_not_reach_past_the_first_form() {
  const char* html =
      "<form action=\"/a\"><input type=\"email\" name=\"mail\"></form>"
      "<form action=\"/b\"><input name=\"search_query\"></form>";
  LoginForm form;
  TEST_ASSERT_TRUE(discoverLoginForm(html, "http://p/", form));
  TEST_ASSERT_EQUAL_STRING("http://p/a", form.action.c_str());
  TEST_ASSERT_EQUAL_size_t(1, form.fields.size());
}

void test_discover_form_reports_no_identity_field_rather_than_guessing() {
  // A password-only form: nothing here is an email or phone box, and
  // picking one at random would just post junk at the portal.
  const char* html =
      "<form action=\"/x\"><input type=\"password\" name=\"pw\">"
      "<input type=\"hidden\" name=\"t\" value=\"1\"></form>";
  LoginForm form;
  TEST_ASSERT_TRUE(discoverLoginForm(html, "http://p/", form));
  TEST_ASSERT_TRUE(form.identityField.empty());
}

void test_discover_form_returns_false_when_there_is_no_form() {
  LoginForm form;
  TEST_ASSERT_FALSE(discoverLoginForm("<html><body>Redirecting...</body></html>", "http://p/", form));
}

// --- buildRequestBody -----------------------------------------------------

void test_build_request_body_substitutes_the_identity() {
  LoginForm form;
  discoverLoginForm(kSimpleSplash, "http://portal.example.com/", form);
  const std::string body = buildRequestBody(form, "rider@example.com");
  TEST_ASSERT_EQUAL_STRING(
      "token=abc123&user_email=rider%40example.com&accept_terms=on&go=Connect", body.c_str());
}

void test_build_request_body_for_a_manually_configured_field() {
  // No discovered inputs at all -- the override path. The identity still
  // has to end up in the body rather than POSTing nothing.
  LoginForm form;
  form.action = "http://portal/login";
  form.identityField = "email";
  TEST_ASSERT_EQUAL_STRING("email=rider%40example.com", buildRequestBody(form, "rider@example.com").c_str());
}

// --- connect() ------------------------------------------------------------

void test_connect_reports_already_open_without_touching_the_portal() {
  ScriptedTransport transport;
  transport.probeOpen = true;
  CaptivePortalClient client(transport);

  const CaptivePortalResult result = client.connect(CaptivePortalConfig{});
  TEST_ASSERT_TRUE(result.online);
  TEST_ASSERT_TRUE(result.alreadyOpen);
  TEST_ASSERT_EQUAL_size_t(0, transport.postUrls.size());
  TEST_ASSERT_EQUAL_size_t(1, transport.getUrls.size());  // one probe, no more
}

// The single most important URL in this module. A portal answers the canary
// with a redirect to its own page, so the form's relative action has to be
// resolved against THAT page. Resolving it against the canary would send
// the sign-in POST to connectivitycheck.gstatic.com, and the feature would
// fail silently on every real network.
void test_form_action_resolves_against_the_portal_page_not_the_canary() {
  ScriptedTransport transport;
  transport.splashHtml = kSimpleSplash;  // action="/login"
  transport.openOnLogin = true;

  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  CaptivePortalClient client(transport);
  const CaptivePortalResult result = client.connect(config);

  TEST_ASSERT_TRUE(result.online);
  TEST_ASSERT_EQUAL_size_t(1, transport.postUrls.size());
  TEST_ASSERT_EQUAL_STRING("http://portal.bus.example/login", transport.postUrls[0].c_str());
}

// An action-less form posts back to the page it came from -- which is the
// portal's page, never the canary.
void test_actionless_form_posts_back_to_the_portal_page() {
  ScriptedTransport transport;
  transport.splashHtml = "<form><input type=\"email\" name=\"email\"></form>";
  transport.openOnLogin = true;

  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  CaptivePortalClient client(transport);
  client.connect(config);

  TEST_ASSERT_EQUAL_size_t(1, transport.postUrls.size());
  TEST_ASSERT_EQUAL_STRING("http://portal.bus.example/welcome/index.html",
                           transport.postUrls[0].c_str());
}

// The redirect couldn't be followed in one request (http canary -> https
// splash page needs a TLS client the plain-http request never made), so the
// probe comes back with a location and no body. The client must reissue it
// rather than reporting "no sign-in form".
void test_unfollowed_redirect_is_fetched_as_its_own_request() {
  ScriptedTransport transport;
  transport.followRedirect = false;
  transport.portalPageUrl = "https://portal.bus.example/welcome/index.html";
  transport.splashHtml = kSimpleSplash;
  transport.openOnLogin = true;

  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  CaptivePortalClient client(transport);
  const CaptivePortalResult result = client.connect(config);

  TEST_ASSERT_TRUE(result.online);
  // The splash page was fetched separately...
  bool fetchedPage = false;
  for (const std::string& url : transport.getUrls) {
    if (url == transport.portalPageUrl) fetchedPage = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(fetchedPage, "an unfollowable redirect must be fetched as its own request");
  // ...and the action resolved against it, keeping the https scheme.
  TEST_ASSERT_EQUAL_STRING("https://portal.bus.example/login", transport.postUrls[0].c_str());
}

void test_connect_signs_in_through_a_discovered_form() {
  ScriptedTransport transport;
  transport.splashHtml = kSimpleSplash;
  transport.openOnLogin = true;

  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  CaptivePortalClient client(transport);
  const CaptivePortalResult result = client.connect(config);

  TEST_ASSERT_TRUE(result.online);
  TEST_ASSERT_FALSE(result.alreadyOpen);
  TEST_ASSERT_EQUAL_size_t(1, transport.postUrls.size());
  TEST_ASSERT_TRUE(transport.postBodies[0].find("user_email=rider%40example.com") != std::string::npos);
}

void test_connect_does_not_claim_success_when_the_portal_still_blocks() {
  // The portal happily returns 200 for the POST but never actually lets us
  // out. This is the failure mode that makes re-probing non-negotiable.
  ScriptedTransport transport;
  transport.splashHtml = kSimpleSplash;
  transport.openOnLogin = false;

  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  CaptivePortalClient client(transport);
  const CaptivePortalResult result = client.connect(config);

  TEST_ASSERT_EQUAL_size_t(1, transport.postUrls.size());  // it did try
  TEST_ASSERT_FALSE(result.online);
}

void test_connect_uses_the_manual_override_instead_of_discovery() {
  ScriptedTransport transport;
  transport.splashHtml = kSimpleSplash;  // would discover /login
  transport.openOnLogin = true;

  CaptivePortalConfig config;
  config.identity = "5095551234";
  config.overrideSubmitUrl = "http://1.2.3.4:8080/auth";
  config.overrideFieldName = "phone";

  CaptivePortalClient client(transport);
  const CaptivePortalResult result = client.connect(config);

  TEST_ASSERT_TRUE(result.online);
  TEST_ASSERT_EQUAL_STRING("http://1.2.3.4:8080/auth", transport.postUrls[0].c_str());
  TEST_ASSERT_EQUAL_STRING("phone=5095551234", transport.postBodies[0].c_str());
}

void test_connect_gives_up_cleanly_with_no_identity_saved() {
  ScriptedTransport transport;
  transport.splashHtml = kSimpleSplash;
  CaptivePortalClient client(transport);

  const CaptivePortalResult result = client.connect(CaptivePortalConfig{});
  TEST_ASSERT_FALSE(result.online);
  TEST_ASSERT_EQUAL_size_t(0, transport.postUrls.size());
  TEST_ASSERT_FALSE(result.message.empty());
}

void test_connect_refuses_to_guess_an_unrecognized_form() {
  ScriptedTransport transport;
  transport.splashHtml = "<form action=\"/x\"><input type=\"password\" name=\"pw\"></form>";

  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  CaptivePortalClient client(transport);
  const CaptivePortalResult result = client.connect(config);

  TEST_ASSERT_FALSE(result.online);
  TEST_ASSERT_EQUAL_size_t(0, transport.postUrls.size());
}

void test_connect_reports_an_unreachable_network_as_such() {
  // Every probe fails outright: no portal, just no network. Trying all of
  // them before concluding that is the point -- one blocked host is not a
  // dead network.
  class DeadTransport : public HttpTransport {
   public:
    HttpResponse get(const std::string& /*url*/,
                     const std::vector<std::pair<std::string, std::string>>& /*h*/) override {
      ++calls;
      return dead();
    }
    int calls = 0;
  };

  DeadTransport transport;
  CaptivePortalClient client(transport);
  CaptivePortalConfig config;
  config.identity = "rider@example.com";

  const CaptivePortalResult result = client.connect(config);
  TEST_ASSERT_FALSE(result.online);
  TEST_ASSERT_EQUAL_INT(transit::kDefaultProbeUrlCount, transport.calls);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_classify_probe_response);
  RUN_TEST(test_url_encode_form);
  RUN_TEST(test_resolve_url);
  RUN_TEST(test_discover_form_reads_action_method_and_fields);
  RUN_TEST(test_discover_form_handles_messy_html);
  RUN_TEST(test_discover_form_does_not_reach_past_the_first_form);
  RUN_TEST(test_discover_form_reports_no_identity_field_rather_than_guessing);
  RUN_TEST(test_discover_form_returns_false_when_there_is_no_form);
  RUN_TEST(test_build_request_body_substitutes_the_identity);
  RUN_TEST(test_build_request_body_for_a_manually_configured_field);
  RUN_TEST(test_connect_reports_already_open_without_touching_the_portal);
  RUN_TEST(test_form_action_resolves_against_the_portal_page_not_the_canary);
  RUN_TEST(test_actionless_form_posts_back_to_the_portal_page);
  RUN_TEST(test_unfollowed_redirect_is_fetched_as_its_own_request);
  RUN_TEST(test_connect_signs_in_through_a_discovered_form);
  RUN_TEST(test_connect_does_not_claim_success_when_the_portal_still_blocks);
  RUN_TEST(test_connect_uses_the_manual_override_instead_of_discovery);
  RUN_TEST(test_connect_gives_up_cleanly_with_no_identity_saved);
  RUN_TEST(test_connect_refuses_to_guess_an_unrecognized_form);
  RUN_TEST(test_connect_reports_an_unreachable_network_as_such);
  return UNITY_END();
}
