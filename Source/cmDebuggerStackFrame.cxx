/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmDebuggerStackFrame.h"

#include <utility>

#include "cmListFileCache.h"

namespace cmDebugger {

std::atomic<int64_t> cmDebuggerStackFrame::NextId(1);

cmDebuggerStackFrame::cmDebuggerStackFrame(cmMakefile* mf,
                                           std::string sourcePath,
                                           cmListFileFunction const& lff)
  : m_id(NextId.fetch_add(1))
  , FileName(std::move(sourcePath))
  , Function(lff)
  , m_pMakefile(mf)
{
}

int64_t cmDebuggerStackFrame::GetLine() const noexcept
{
  return this->Function.Line();
}

std::vector<cmListFileArgument> const& cmDebuggerStackFrame::GetArguments()
  const noexcept
{
  return this->Function.Arguments();
}

} // namespace cmDebugger
