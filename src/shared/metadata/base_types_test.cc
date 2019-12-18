#include <gtest/gtest.h>

#include <absl/hash/hash_testing.h>
#include "src/common/base/test_utils.h"
#include "src/shared/metadata/base_types.h"

namespace pl {
namespace md {
using ::testing::ContainsRegex;

TEST(UPID, check_upid_components) {
  auto upid = UPID(123, 456, 3420030816657ULL);
  EXPECT_EQ(123, upid.asid());
  EXPECT_EQ(456, upid.pid());
  EXPECT_EQ(3420030816657ULL, upid.start_ts());
}

TEST(UPID, check_upid_eq) {
  EXPECT_NE(UPID(12, 456, 3420030816657ULL), UPID(123, 456, 3420030816657ULL));
  EXPECT_NE(UPID(123, 456, 3420030816657ULL), UPID(123, 456, 3000000000000ULL));
  EXPECT_NE(UPID(123, 45, 3420030816657ULL), UPID(123, 456, 3420030816657ULL));

  EXPECT_EQ(UPID(123, 456, 3420030816657ULL), UPID(123, 456, 3420030816657ULL));
}

TEST(UPID, hash_func) {
  EXPECT_TRUE(absl::VerifyTypeImplementsAbslHashCorrectly({
      UPID(123, 456, 789),
      UPID(13, 46, 3420030816657ULL),
      UPID(12, 456, 3420030816657ULL),
  }));
}

TEST(UPID, string) {
  EXPECT_EQ("123:456:3420030816657", UPID(123, 456, 3420030816657ULL).String());
  EXPECT_EQ("12:456:3420030816657", UPID(12, 456, 3420030816657ULL).String());
  EXPECT_EQ("12:46:3420030816657", UPID(12, 46, 3420030816657ULL).String());
}

TEST(UPID, parse_from_uuid_string) {
  auto upid_or_s = UPID::ParseFromUUIDString("0000007b-0000-01c8-0000-031c49b8d191");
  ASSERT_OK(upid_or_s);
  EXPECT_EQ(upid_or_s.ConsumeValueOrDie(), UPID(123, 456, 3420030816657ULL));
}

// Test to see whether we can convert upid to uuid to upid and its consistent
TEST(UPID, uuid_conversion_consistency) {
  UPID upid(123, 456, 3420030816657ULL);
  absl::uint128 val = upid.value();
  std::string value = sole::rebuild(absl::Uint128High64(val), absl::Uint128Low64(val)).str();
  auto upid_or_s = UPID::ParseFromUUIDString(value);
  ASSERT_OK(upid_or_s);
  EXPECT_EQ(upid_or_s.ConsumeValueOrDie(), upid);
}

TEST(UPID, parse_from_uuid_fails_on_bad_uuid) {
  auto upid_or_s = UPID::ParseFromUUIDString("9999");
  ASSERT_NOT_OK(upid_or_s);
  EXPECT_THAT(upid_or_s.status().msg(), ContainsRegex("'9999' is not a valid UUID"));
}

}  // namespace md
}  // namespace pl
