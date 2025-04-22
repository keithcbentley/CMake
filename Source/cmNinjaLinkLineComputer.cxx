/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmNinjaLinkLineComputer.h"

#include "cmGlobalNinjaGenerator.h"

class cmOutputConverter;

cmNinjaLinkLineComputer::cmNinjaLinkLineComputer(
  cmOutputConverter* outputConverter, cmStateDirectory const& stateDir,
  cmGlobalNinjaGenerator const* gg)
  : cmLinkLineComputer(outputConverter, stateDir)
  , m_globalGenerator(gg)
{
}

std::string cmNinjaLinkLineComputer::ConvertToLinkReference(
  std::string const& lib) const
{
  return this->m_globalGenerator->ConvertToNinjaPath(lib);
}
