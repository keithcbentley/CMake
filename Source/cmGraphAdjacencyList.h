/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <cstddef>
#include <utility>
#include <vector>

#include "cmListFileCache.h"

/**
 * Graph edge representation.  Most use cases just need the
 * destination vertex, so we support conversion to/from an int.  We
 * also store boolean to indicate whether an edge is "strong".
 */
class cmGraphEdge
{
public:
  cmGraphEdge(size_t n, bool s, bool c, cmListFileBacktrace bt)
    : Dest(n)
    , Strong(s)
    , Cross(c)
    , m_backtrace(std::move(bt))
  {
  }
  operator size_t() const { return this->Dest; }

  bool IsStrong() const { return this->Strong; }

  bool IsCross() const { return this->Cross; }

  cmListFileBacktrace const& GetBacktrace() const { return this->m_backtrace; }

private:
  size_t Dest;
  bool Strong;
  bool Cross;
  cmListFileBacktrace m_backtrace;
};
struct cmGraphEdgeList : public std::vector<cmGraphEdge>
{
};
struct cmGraphNodeList : public std::vector<size_t>
{
};
struct cmGraphAdjacencyList : public std::vector<cmGraphEdgeList>
{
};
