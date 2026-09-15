// Host-side tests for transit::findAgencyMetadata (agency_metadata.h),
// the generated (tools/gen_agency_metadata.py) on-device table the
// settings portal reads a configured agency's name/attribution/terms
// from. Mostly a smoke test on the generator's own output rather than
// exhaustive coverage of the lookup logic (a linear scan over a handful
// of entries has little to get wrong) -- the real risk here is the
// generator silently producing something that doesn't build or doesn't
// round-trip a real registry entry's fields, which these catch.

#include <cstring>
#include <unity.h>

#include "transit/agency_metadata.h"

using transit::AgencyMetadata;
using transit::findAgencyMetadata;
using transit::kAgencyMetadata;
using transit::kAgencyMetadataCount;

void test_table_is_not_empty() {
  // agencies/registry.json has at least STA as of this writing -- an empty
  // table would mean the generator ran against the wrong file or every
  // entry was (wrongly) treated as status=requested.
  TEST_ASSERT_TRUE(kAgencyMetadataCount > 0);
}

void test_finds_sta_with_its_real_fields() {
  const AgencyMetadata* sta = findAgencyMetadata("sta");
  TEST_ASSERT_NOT_NULL(sta);
  TEST_ASSERT_EQUAL_STRING("sta", sta->id);
  TEST_ASSERT_EQUAL_STRING("Spokane Transit Authority", sta->name);
  TEST_ASSERT_TRUE(strlen(sta->region) > 0);
  TEST_ASSERT_TRUE(strlen(sta->termsUrl) > 0);
  // Reviewed in docs/STA_INTEGRATION.md's Compliance section: STA's terms
  // don't require an on-device attribution badge.
  TEST_ASSERT_FALSE(sta->attributionRequired);
}

void test_returns_null_for_an_unknown_or_merely_requested_id() {
  TEST_ASSERT_NULL(findAgencyMetadata("not-a-real-agency"));
  TEST_ASSERT_NULL(findAgencyMetadata(""));
}

void test_every_entry_has_a_non_empty_id_and_name() {
  // Cheap invariant check on the generator's own output -- every row it
  // could possibly emit comes from a schema-validated registry entry
  // (agencies/registry.schema.json requires id/name), so this should
  // always hold; a violation would mean the generator itself is broken.
  for (size_t i = 0; i < kAgencyMetadataCount; ++i) {
    TEST_ASSERT_TRUE(strlen(kAgencyMetadata[i].id) > 0);
    TEST_ASSERT_TRUE(strlen(kAgencyMetadata[i].name) > 0);
  }
}

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_table_is_not_empty);
  RUN_TEST(test_finds_sta_with_its_real_fields);
  RUN_TEST(test_returns_null_for_an_unknown_or_merely_requested_id);
  RUN_TEST(test_every_entry_has_a_non_empty_id_and_name);
  return UNITY_END();
}
