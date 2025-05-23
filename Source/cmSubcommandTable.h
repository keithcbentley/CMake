/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <cm/string_view>
#include <cmext/string_view>

class cmExecutionStatus;

class cmSubcommandTable
{
public:
  using m_command = bool (*)(std::vector<std::string> const&,
                           cmExecutionStatus&);

  using Elem = std::pair<cm::string_view, m_command>;
  using InitElem = std::pair<cm::static_string_view, m_command>;

  cmSubcommandTable(std::initializer_list<InitElem> init);

  bool operator()(cm::string_view key, std::vector<std::string> const& args,
                  cmExecutionStatus& status) const;

private:
  std::vector<Elem> Impl;
};
