/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>

//  TODO: Why is this in a separate file from documentation.

/** Standard documentation entry for cmDocumentation's formatting.  */
struct cmDocumentationEntry
{
#if __cplusplus <= 201103L
  cmDocumentationEntry(
    std::string const& name,
    std::string const& brief)
    : m_name{ name }
    , m_brief{ brief }
  {
  }
#endif

  std::string m_name;
  std::string m_brief;
  char m_customNamePrefix = ' ';
};
