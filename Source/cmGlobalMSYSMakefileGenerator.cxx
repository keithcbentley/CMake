/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmGlobalMSYSMakefileGenerator.h"

#include <cmext/string_view>

#include "cmsys/FStream.hxx"

#include "cmMakefile.h"
#include "cmState.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmake.h"

cmGlobalMSYSMakefileGenerator::cmGlobalMSYSMakefileGenerator(CMake* cm)
  : cmGlobalUnixMakefileGenerator3(cm)
{
  this->FindMakeProgramFile = "CMakeMSYSFindMake.cmake";
  this->ForceUnixPaths = true;
  this->ToolSupportsColor = true;
  this->UseLinkScript = false;
  cm->GetState()->SetMSYSShell(true);
}

std::string cmGlobalMSYSMakefileGenerator::FindMinGW(
  std::string const& makeloc)
{
  std::string fstab = cmStrCat(makeloc, "/../etc/fstab");
  cmsys::ifstream fin(fstab.c_str());
  std::string path;
  std::string mount;
  std::string mingwBin;
  while (fin) {
    fin >> path;
    fin >> mount;
    if (mount == "/mingw"_s) {
      mingwBin = cmStrCat(path, "/bin");
    }
  }
  return mingwBin;
}

void cmGlobalMSYSMakefileGenerator::EnableLanguage(
  std::vector<std::string> const& l, cmMakefile* mf, bool optional)
{
  mf->AddDefinition("MSYS", "1");
  this->cmGlobalUnixMakefileGenerator3::EnableLanguage(l, mf, optional);

  if (!mf->IsSet("CMAKE_AR") && !this->CMakeInstance->GetIsInTryCompile() &&
      !(1 == l.size() && l[0] == "NONE"_s)) {
    cmSystemTools::Error(
      "CMAKE_AR was not found, please set to archive program. " +
      mf->GetSafeDefinition("CMAKE_AR"));
  }
}

//cmDocumentationEntry cmGlobalMSYSMakefileGenerator::GetDocumentation()
//{
//  return { cmGlobalMSYSMakefileGenerator::GetActualName(),
//           "Generates MSYS makefiles." };
//}
