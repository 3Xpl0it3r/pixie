#include <gtest/gtest.h>

#include "src/shared/bloomfilter/bloomfilter.h"

namespace pl {
namespace bloomfilter {

TEST(XXHash64BloomFilter, test_create) {
  auto bf1 = XXHash64BloomFilter::Create(10, 0.1).ConsumeValueOrDie();
  EXPECT_EQ(bf1->num_hashes(), 4);
  EXPECT_EQ(bf1->buffer_size_bytes(), 6);

  auto bf2 = XXHash64BloomFilter::Create(100000, 0.01).ConsumeValueOrDie();
  EXPECT_EQ(bf2->num_hashes(), 7);
  EXPECT_EQ(bf2->buffer_size_bytes(), 119814);

  auto bf3 = XXHash64BloomFilter::Create(1, 0.999999999999).ConsumeValueOrDie();
  EXPECT_EQ(bf3->num_hashes(), 1);
  EXPECT_EQ(bf3->buffer_size_bytes(), 1);
}

TEST(XXHash64BloomFilter, test_string_view) {
  auto bf1 = XXHash64BloomFilter::Create(10, 0.1).ConsumeValueOrDie();
  EXPECT_FALSE(bf1->Contains(std::string_view("foo")));
  EXPECT_FALSE(bf1->Contains(std::string_view("bar")));
  bf1->Insert(std::string_view("foo"));
  bf1->Insert(std::string_view("bar"));
  EXPECT_TRUE(bf1->Contains(std::string_view("foo")));
  EXPECT_TRUE(bf1->Contains(std::string_view("bar")));
  EXPECT_FALSE(bf1->Contains(std::string_view("not_present")));
  EXPECT_FALSE(bf1->Contains(std::string_view("")));
}

TEST(XXHash64BloomFilter, test_string) {
  auto bf1 = XXHash64BloomFilter::Create(10, 0.1).ConsumeValueOrDie();
  EXPECT_FALSE(bf1->Contains(std::string("foo")));
  EXPECT_FALSE(bf1->Contains(std::string("bar")));
  bf1->Insert(std::string("foo"));
  bf1->Insert(std::string("bar"));
  EXPECT_TRUE(bf1->Contains(std::string("foo")));
  EXPECT_TRUE(bf1->Contains(std::string("bar")));
  EXPECT_FALSE(bf1->Contains(std::string("not_present")));
  EXPECT_FALSE(bf1->Contains(std::string("")));
}

TEST(XXHash64BloomFilter, test_error_rate) {
  auto high_fp_bf = XXHash64BloomFilter::Create(10, 0.5).ConsumeValueOrDie();
  auto low_fp_bf = XXHash64BloomFilter::Create(1000, 0.0001).ConsumeValueOrDie();

  std::vector<std::string> actual{"foo", "bar"};
  std::vector<std::string> false_positives{"7"};
  std::vector<std::string> true_negatives{"1", "2", "3", "4"};

  for (const auto& str : actual) {
    high_fp_bf->Insert(str);
    low_fp_bf->Insert(str);
  }
  for (const auto& str : actual) {
    EXPECT_TRUE(high_fp_bf->Contains(str));
    EXPECT_TRUE(low_fp_bf->Contains(str));
  }
  for (const auto& str : false_positives) {
    EXPECT_TRUE(high_fp_bf->Contains(str));
    EXPECT_FALSE(low_fp_bf->Contains(str));
  }
  for (const auto& str : true_negatives) {
    EXPECT_FALSE(high_fp_bf->Contains(str));
    EXPECT_FALSE(low_fp_bf->Contains(str));
  }
}

}  // namespace bloomfilter
}  // namespace pl
