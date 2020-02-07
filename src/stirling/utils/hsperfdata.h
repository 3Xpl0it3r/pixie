#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/common/base/base.h"
#include "src/stirling/common/parse_state.h"

namespace pl {
namespace stirling {
namespace hsperf {

// src/java.management/share/classes/sun/management/counter/perf/Prologue.java
// This prologue is at the start of the hsperfdata file. Followed by data entries, whose number is
// specified in num_entries.
struct Prologue {
  const uint8_t magic[4];
  uint8_t byte_order;
  uint8_t major_version;
  uint8_t minor_version;
  uint8_t accessible;
  uint32_t used;
  uint32_t overflow;
  uint64_t mod_timestamp;
  uint32_t entry_offset;
  uint32_t num_entries;
};

// src/java.management/share/classes/sun/management/counter/perf/PerfDataEntry.java
// This header is at the start of each data entry. It specifies the offset of the name and data.
struct DataEntryHeader {
  uint32_t entry_length;
  uint32_t name_offset;
  uint32_t vector_length;
  uint8_t data_type;
  uint8_t flags;
  uint8_t data_units;
  uint8_t data_variability;
  uint32_t data_offset;
};

struct DataEntry {
  const DataEntryHeader* header;
  std::string_view name;
  std::string_view data;
  ParseState parse_state = ParseState::kSuccess;
};

enum class DataType : uint8_t {
  kUnknown = 0x0,
  kByte = 'B',
  kChar = 'C',
  kDouble = 'D',
  kFloat = 'F',
  kInt = 'I',
  kLong = 'J',
  kShort = 'S',
  kBool = 'Z',
  kVoid = 'V',
  kObject = 'L',
  kArray = '[',
};

enum class DataUnits : uint8_t {
  kString = 5,
};

enum class DataVariability : uint8_t {
  kConstant = 1,
  kVariable = 3,
};

struct HsperfData {
  // Holds the actual data. The rest fields point to its segments.
  std::string buf;

  const Prologue* prologue;
  std::vector<DataEntry> data_entries;
};

/**
 * Parses the input buffer, and writes the data fields. Returns OK if succeeded.
 */
Status ParseHsperfData(std::string buf, HsperfData* data);

}  // namespace hsperf
}  // namespace stirling
}  // namespace pl
