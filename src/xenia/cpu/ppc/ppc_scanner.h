/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_SCANNER_H_
#define XENIA_CPU_PPC_PPC_SCANNER_H_

#include <vector>

#include "xenia/cpu/function.h"
#include "xenia/cpu/function_debug_info.h"

namespace xe {
namespace cpu {
namespace ppc {

class GuestCodeReader;
class PPCFrontend;

struct BlockInfo {
  uint32_t start_address;
  uint32_t end_address;
};

class PPCScanner {
 public:
  explicit PPCScanner(PPCFrontend* frontend);
  ~PPCScanner();

  bool Scan(GuestFunction* function, const GuestCodeReader& guest_code_reader,
            FunctionDebugInfo* debug_info);

  bool FindBlocks(GuestFunction* function,
                  const GuestCodeReader& guest_code_reader,
                  std::vector<BlockInfo>* blocks_out);

 private:
  bool IsRestGprLr(TranslationDomainKey translation_domain_key,
                   uint32_t address);

  PPCFrontend* frontend_ = nullptr;
};

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_SCANNER_H_
