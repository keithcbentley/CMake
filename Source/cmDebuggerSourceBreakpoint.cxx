/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmDebuggerSourceBreakpoint.h"

namespace cmDebugger {

cmDebuggerSourceBreakpoint::cmDebuggerSourceBreakpoint(int64_t id,
                                                       int64_t line)
  : m_id(id)
  , Line(line)
{
}

} // namespace cmDebugger
