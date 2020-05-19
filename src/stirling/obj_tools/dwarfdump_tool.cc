#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

#include "src/common/base/base.h"

#include "src/stirling/obj_tools/dwarf_tools.h"

constexpr char kProgramDescription[] =
    "A simple tool that finds debug information in object files with DWARF info.\n"
    "Like dwarfdump, but simplified.";

DEFINE_string(filename, "", "Object file to search.");
DEFINE_string(die_name, "", "The Debugging Information Entry (DIE) to search for.");

void InitDumpOpts(llvm::DIDumpOptions* opts) {
  opts->DumpType = llvm::DIDT_DebugInfo;  // Other options: DIDT_UUID, DIDT_All, DIDT_Null
  opts->ChildRecurseDepth = -1;
  opts->ParentRecurseDepth = -1;
  opts->ShowAddresses = true;
  opts->ShowChildren = true;
  opts->ShowParents = false;
  opts->ShowForm = false;
  opts->SummarizeTypes = false;
  opts->Verbose = false;
}

int main(int argc, char** argv) {
  gflags::SetUsageMessage(kProgramDescription);
  pl::EnvironmentGuard env_guard(&argc, argv);

  std::error_code ec;
  llvm::ToolOutputFile OutputFile("-", ec, llvm::sys::fs::OF_Text);
  if (ec) {
    LOG(ERROR) << absl::Substitute("Unable to open file for writing. msg=$0", ec.message());
    exit(1);
  }

  PL_ASSIGN_OR_EXIT(auto dwarf_reader, pl::stirling::dwarf_tools::DwarfReader::Create(
                                           FLAGS_filename, /* index */ false));
  PL_ASSIGN_OR_EXIT(std::vector<llvm::DWARFDie> dies,
                    dwarf_reader->GetMatchingDIEs(FLAGS_die_name));

  llvm::DIDumpOptions dump_opts;
  InitDumpOpts(&dump_opts);
  for (const auto& d : dies) {
    d.dump(OutputFile.os(), 0, dump_opts);
  }

  return 0;
}
