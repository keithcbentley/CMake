/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmNinjaLinkLineDeviceComputer.h"

#include "cmGlobalNinjaGenerator.h"

cmNinjaLinkLineDeviceComputer::cmNinjaLinkLineDeviceComputer(
  cmOutputConverter* outputConverter, cmStateDirectory const& stateDir,
  cmGlobalNinjaGenerator const* gg)
  : cmLinkLineDeviceComputer(outputConverter, stateDir)
  , m_globalGenerator(gg)
{
}

std::string cmNinjaLinkLineDeviceComputer::ConvertToLinkReference(
  std::string const& lib) const
{
  return this->m_globalGenerator->ConvertToNinjaPath(lib);
}
