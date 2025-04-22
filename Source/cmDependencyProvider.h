/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

class cmDependencyProvider
{
public:
  enum class Method
  {
    FindPackage,
    FetchContentMakeAvailableSerial,
  };

  cmDependencyProvider(std::string command, std::vector<Method> methods)
    : m_command(std::move(command))
    , Methods(std::move(methods))
  {
  }

  std::string const& GetCommand() const { return this->m_command; }
  std::vector<Method> const& GetMethods() const { return this->Methods; }
  bool SupportsMethod(Method method) const
  {
    return std::find(this->Methods.begin(), this->Methods.end(), method) !=
      this->Methods.end();
  }

private:
  std::string m_command;
  std::vector<Method> Methods;
};
