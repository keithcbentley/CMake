/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmIncludeGuardCommand.h"

#include "cmCryptoHash.h"
#include "cmExecutionStatus.h"
#include "cmMakefile.h"
#include "cmStateDirectory.h"
#include "cmStateSnapshot.h"
#include "cmStringAlgorithms.h"
#include "cmValue.h"
#include "cmake.h"

namespace {

enum IncludeGuardScope
{
  VARIABLE,
  DIRECTORY,
  GLOBAL
};

std::string GetIncludeGuardVariableName(std::string const& filePath)
{
  cmCryptoHash hasher(cmCryptoHash::AlgoMD5);
  return cmStrCat("__INCGUARD_", hasher.HashString(filePath), "__");
}

bool CheckIncludeGuardIsSet(cmMakefile* mf, std::string const& includeGuardVar)
{
  if (mf->GetProperty(includeGuardVar)) {
    return true;
  }
  cmStateSnapshot dirSnapshot =
    mf->GetStateSnapshot().GetBuildsystemDirectoryParent();
  while (dirSnapshot.GetState()) {
    cmStateDirectory stateDir = dirSnapshot.GetDirectory();
    if (stateDir.GetProperty(includeGuardVar)) {
      return true;
    }
    dirSnapshot = dirSnapshot.GetBuildsystemDirectoryParent();
  }
  return false;
}

} // anonymous namespace

// cmIncludeGuardCommand
bool cmIncludeGuardCommand(std::vector<std::string> const& args,
                           cmExecutionStatus& status)
{
  if (args.size() > 1) {
    status.SetError(
      "given an invalid number of arguments. The command takes at "
      "most 1 argument.");
    return false;
  }

  IncludeGuardScope scope = VARIABLE;

  if (!args.empty()) {
    std::string const& arg = args[0];
    if (arg == "DIRECTORY") {
      scope = DIRECTORY;
    } else if (arg == "GLOBAL") {
      scope = GLOBAL;
    } else {
      status.SetError("given an invalid scope: " + arg);
      return false;
    }
  }

  std::string includeGuardVar = GetIncludeGuardVariableName(
    *status.GetMakefile().GetDefinition("CMAKE_CURRENT_LIST_FILE"));

  cmMakefile* const mf = &status.GetMakefile();

  switch (scope) {
    case VARIABLE:
      if (mf->IsDefinitionSet(includeGuardVar)) {
        status.SetReturnInvoked();
        return true;
      }
      mf->AddDefinitionBool(includeGuardVar, true);
      break;
    case DIRECTORY:
      if (CheckIncludeGuardIsSet(mf, includeGuardVar)) {
        status.SetReturnInvoked();
        return true;
      }
      mf->SetProperty(includeGuardVar, "TRUE");
      break;
    case GLOBAL:
      CMake* const cm = mf->GetCMakeInstance();
      if (cm->GetProperty(includeGuardVar)) {
        status.SetReturnInvoked();
        return true;
      }
      cm->SetProperty(includeGuardVar, "TRUE");
      break;
  }

  return true;
}
