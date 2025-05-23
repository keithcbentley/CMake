/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmGlobalMinGWMakefileGenerator.h"

#include "cmMakefile.h"
#include "cmState.h"
#include "cmSystemTools.h"
#include "cmake.h"

cmGlobalMinGWMakefileGenerator::cmGlobalMinGWMakefileGenerator(CMake* cm)
  : cmGlobalUnixMakefileGenerator3(cm)
{
  this->FindMakeProgramFile = "CMakeMinGWFindMake.cmake";
  this->ForceUnixPaths = true;
  this->ToolSupportsColor = true;
  this->UseLinkScript = true;
  cm->GetState()->SetWindowsShell(true);
  cm->GetState()->SetMinGWMake(true);
}

//cmDocumentationEntry cmGlobalMinGWMakefileGenerator::GetDocumentation()
//{
//  return { cmGlobalMinGWMakefileGenerator::GetActualName(),
//           "Generates a make file for use with mingw32-make." };
//}
