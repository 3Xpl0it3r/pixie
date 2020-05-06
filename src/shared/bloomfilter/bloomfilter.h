#pragma once

#include <math.h>
#include <memory>
#include <string>
#include <vector>

#include "src/common/base/base.h"

namespace pl {
namespace bloomfilter {

class XXHash64BloomFilter {
 public:
  /**
   * Create creates a bloom filter which is sized to meet the criteria for maximum number of
   * entries and the false positive error rate. The false negative error rate is always 0.
   */
  static StatusOr<std::unique_ptr<XXHash64BloomFilter>> Create(int64_t max_entries,
                                                               double error_rate);
  // TODO(nserrino): Add these in.
  // static StatusOr<std::unique_ptr<BloomFilter>> FromProto(const bloomfilterpb::BloomFilter& pb);
  // bloomfilterpb::BloomFilter ToProto();

  /**
   * Insert inserts an item into the bloom filter.
   */
  void Insert(std::string_view item);
  void Insert(const std::string& item) { return Insert(std::string_view(item)); }

  /**
   * Contains checks for the presence of an item in the bloom filter. May return a false positive,
   * but will not return a false negative.
   */
  bool Contains(std::string_view item) const;
  bool Contains(const std::string& item) const { return Contains(std::string_view(item)); }

  /**
   * Get the buffer size in bytes of the bloom filter.
   */
  size_t buffer_size_bytes() const { return buffer_.size(); }

  /**
   * Get the number of hashes used in the bloom filter.
   */
  int num_hashes() const { return num_hashes_; }

 protected:
  XXHash64BloomFilter(int64_t num_bytes, int num_hashes)
      : num_hashes_(num_hashes), buffer_(std::vector<uint8_t>(num_bytes, 0)) {}

 private:
  void SetBit(int bit_number);
  bool HasBitSet(int bit_number) const;

  const int num_hashes_;
  std::vector<uint8_t> buffer_;
  const uint64_t seed_ = 3091990;
};

}  // namespace bloomfilter
}  // namespace pl
