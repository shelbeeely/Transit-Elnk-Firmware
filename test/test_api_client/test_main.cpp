// Host-side tests for work unit 2 (Transit API client) — verifies request
// building (URL/query string/apiKey header) against a fake HttpTransport,
// per docs/API_CONTRACT.md's param tables. Response *parsing* is covered by
// test_models instead (parseNearbyRoutes et al. are stubs here, always
// returning false, which is fine: these tests only inspect what request was
// sent).

#include <unity.h>

#include <string>
#include <vector>

#include "transit/api_client.h"

namespace {

// Captures the last GET call's URL/headers and returns a canned response.
class FakeHttpTransport : public transit::HttpTransport {
 public:
  transit::HttpResponse get(
      const std::string& url,
      const std::vector<std::pair<std::string, std::string>>& headers) override {
    lastUrl = url;
    lastHeaders = headers;
    ++callCount;
    return nextResponse;
  }

  std::string lastUrl;
  std::vector<std::pair<std::string, std::string>> lastHeaders;
  int callCount = 0;
  transit::HttpResponse nextResponse;
};

bool headerHas(const std::vector<std::pair<std::string, std::string>>& headers,
               const std::string& key, const std::string& value) {
  for (const auto& h : headers) {
    if (h.first == key && h.second == value) return true;
  }
  return false;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

void test_nearby_routes_builds_url_and_apikey_header() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "test-key-123");

  transit::NearbyRoutesParams params;
  params.maxDistanceMeters = 500;
  params.maxNumDepartures = 5;
  params.shouldUpdateRealtime = false;
  params.mergePlatformStops = true;
  params.includeStopsAndShapes = false;
  params.stopDetailed = false;
  params.locale = "fr,en";

  transit::NearbyRoutesResponse out;
  client.nearbyRoutes(45.5017, -73.5673, params, out);

  TEST_ASSERT_EQUAL_INT(1, fake.callCount);
  TEST_ASSERT_TRUE(contains(fake.lastUrl,
                             "https://external.transitapp.com/v4/public/nearby_routes?"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "lat=45.501700"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "lon=-73.567300"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "max_distance=500"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "max_num_departures=5"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "should_update_realtime=false"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "merge_platform_stops=true"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "locale=fr%2Cen"));
  TEST_ASSERT_TRUE(headerHas(fake.lastHeaders, "apiKey", "test-key-123"));
}

void test_nearby_routes_clamps_out_of_range_values() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::NearbyRoutesParams params;
  params.maxDistanceMeters = 999999;  // over the documented max of 1500
  params.maxNumDepartures = 0;        // under the documented min of 1

  transit::NearbyRoutesResponse out;
  client.nearbyRoutes(0, 0, params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "max_distance=1500"));
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "max_num_departures=1"));
}

void test_nearby_routes_omits_time_when_zero() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::NearbyRoutesParams params;  // timeEpoch defaults to 0 ("now")
  transit::NearbyRoutesResponse out;
  client.nearbyRoutes(1.0, 2.0, params, out);

  // NB: "&time=" (not bare "time=") since "should_update_realtime=" also
  // contains the substring "time=".
  TEST_ASSERT_FALSE(contains(fake.lastUrl, "&time="));
}

void test_nearby_routes_includes_time_when_set() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::NearbyRoutesParams params;
  params.timeEpoch = 1700000000;
  transit::NearbyRoutesResponse out;
  client.nearbyRoutes(1.0, 2.0, params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "&time=1700000000"));
}

void test_stop_departures_joins_ids_with_commas() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::StopDeparturesParams params;
  transit::StopDeparturesResponse out;
  client.stopDepartures({"1:82774", "2:54321"}, params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "global_stop_ids=1%3A82774,2%3A54321"));
}

void test_stop_departures_caps_ids_at_100() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  std::vector<std::string> ids;
  for (int i = 0; i < 120; ++i) ids.push_back(std::to_string(i));

  transit::StopDeparturesParams params;
  transit::StopDeparturesResponse out;
  client.stopDepartures(ids, params, out);

  // Only the first 100 should appear; the 101st (index 100, value "100")
  // must be dropped, not silently sent and rejected server-side.
  TEST_ASSERT_TRUE(contains(fake.lastUrl, "99"));
  TEST_ASSERT_FALSE(contains(fake.lastUrl, "global_stop_ids=100,"));
  size_t pos = fake.lastUrl.find("global_stop_ids=");
  TEST_ASSERT_NOT_EQUAL(std::string::npos, pos);
  std::string idsParam = fake.lastUrl.substr(pos);
  size_t commaCount = 0;
  for (char c : idsParam) {
    if (c == ',') ++commaCount;
    else if (c == '&') break;
  }
  TEST_ASSERT_EQUAL_INT(99, commaCount);  // 100 ids -> 99 commas
}

void test_nearby_stops_omits_empty_pickup_dropoff_filter() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::NearbyStopsParams params;  // pickupDropoffFilter default-empty
  transit::NearbyStopsResponse out;
  client.nearbyStops(1.0, 2.0, params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "stop_filter=Routable"));
  TEST_ASSERT_FALSE(contains(fake.lastUrl, "pickup_dropoff_filter="));
}

void test_nearby_stops_includes_pickup_dropoff_filter_when_set() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::NearbyStopsParams params;
  params.pickupDropoffFilter = "PickupAllowedOnly";
  transit::NearbyStopsResponse out;
  client.nearbyStops(1.0, 2.0, params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "pickup_dropoff_filter=PickupAllowedOnly"));
}

void test_search_stops_percent_encodes_query_text() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::SearchStopsParams params;
  transit::SearchStopsResponse out;
  client.searchStops(45.5, -73.5, "Main St & 5th/Ave", params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "query=Main%20St%20%26%205th%2FAve"));
}

void test_search_stops_omits_max_distance_when_unset() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::SearchStopsParams params;  // maxDistanceMeters defaults to -1 (unset)
  transit::SearchStopsResponse out;
  client.searchStops(1.0, 2.0, "bus", params, out);

  TEST_ASSERT_FALSE(contains(fake.lastUrl, "max_distance="));
}

void test_search_stops_includes_max_distance_when_set() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "k");

  transit::SearchStopsParams params;
  params.maxDistanceMeters = 800;
  transit::SearchStopsResponse out;
  client.searchStops(1.0, 2.0, "bus", params, out);

  TEST_ASSERT_TRUE(contains(fake.lastUrl, "max_distance=800"));
}

void test_transport_failure_returns_false_without_treating_body_as_success() {
  FakeHttpTransport fake;
  fake.nextResponse.transportOk = false;
  fake.nextResponse.statusCode = 0;
  transit::TransitApiClient client(fake, "k");

  transit::NearbyRoutesParams params;
  transit::NearbyRoutesResponse out;
  bool ok = client.nearbyRoutes(1.0, 2.0, params, out);

  TEST_ASSERT_FALSE(ok);
}

void test_non_200_status_returns_false() {
  FakeHttpTransport fake;
  fake.nextResponse.transportOk = true;
  fake.nextResponse.statusCode = 401;
  fake.nextResponse.body = "{\"error\":\"unauthorized\"}";
  transit::TransitApiClient client(fake, "bad-key");

  transit::NearbyRoutesParams params;
  transit::NearbyRoutesResponse out;
  bool ok = client.nearbyRoutes(1.0, 2.0, params, out);

  TEST_ASSERT_FALSE(ok);
}

void test_set_api_key_updates_subsequent_requests() {
  FakeHttpTransport fake;
  transit::TransitApiClient client(fake, "old-key");
  client.setApiKey("new-key");

  transit::NearbyRoutesParams params;
  transit::NearbyRoutesResponse out;
  client.nearbyRoutes(1.0, 2.0, params, out);

  TEST_ASSERT_TRUE(headerHas(fake.lastHeaders, "apiKey", "new-key"));
  TEST_ASSERT_FALSE(headerHas(fake.lastHeaders, "apiKey", "old-key"));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_nearby_routes_builds_url_and_apikey_header);
  RUN_TEST(test_nearby_routes_clamps_out_of_range_values);
  RUN_TEST(test_nearby_routes_omits_time_when_zero);
  RUN_TEST(test_nearby_routes_includes_time_when_set);
  RUN_TEST(test_stop_departures_joins_ids_with_commas);
  RUN_TEST(test_stop_departures_caps_ids_at_100);
  RUN_TEST(test_nearby_stops_omits_empty_pickup_dropoff_filter);
  RUN_TEST(test_nearby_stops_includes_pickup_dropoff_filter_when_set);
  RUN_TEST(test_search_stops_percent_encodes_query_text);
  RUN_TEST(test_search_stops_omits_max_distance_when_unset);
  RUN_TEST(test_search_stops_includes_max_distance_when_set);
  RUN_TEST(test_transport_failure_returns_false_without_treating_body_as_success);
  RUN_TEST(test_non_200_status_returns_false);
  RUN_TEST(test_set_api_key_updates_subsequent_requests);
  return UNITY_END();
}
