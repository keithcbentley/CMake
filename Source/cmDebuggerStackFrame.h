/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class cmListFileFunction;
struct cmListFileArgument;
class cmMakefile;

namespace cmDebugger {

class cmDebuggerStackFrame
{
  static std::atomic<std::int64_t> NextId;
  std::int64_t m_id;
  std::string FileName;
  cmListFileFunction const& Function;
  cmMakefile* m_pMakefile;

public:
  cmDebuggerStackFrame(cmMakefile* mf, std::string sourcePath,
                       cmListFileFunction const& lff);
  int64_t GetId() const noexcept { return this->m_id; }
  std::string const& GetFileName() const noexcept { return this->FileName; }
  int64_t GetLine() const noexcept;
  cmMakefile* GetMakefile() const noexcept { return this->m_pMakefile; }
  cmListFileFunction const& GetFunction() const noexcept
  {
    return this->Function;
  }
  std::vector<cmListFileArgument> const& GetArguments() const noexcept;
};

} // namespace cmDebugger
