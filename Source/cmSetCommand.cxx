/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmSetCommand.h"

#include <iostream>

#include "CmakeBetter.h"
#include "cmExecutionStatus.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmRange.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"

// cmSetCommand
bool cmSetCommand(std::vector<std::string> const& args,
                  cmExecutionStatus& status)
{
  SafeArgs safeArgs(args);

  if (safeArgs.isEmpty()) {
    status.SetError("called with incorrect number of arguments.");
    return false;
  }

  // watch for ENV signatures
  if (safeArgs[0].starts_with("ENV{")) {
    if (!safeArgs[0].ends_with("}")) {
      status.SetError("ENV{ is missing closing } character.");
      return false;
    }
    std::string const envVarName =
      StringUtil::substrStartEnd(safeArgs[0], 4, -1);
    if (envVarName.empty()) {
      status.SetError("ENV{} is missing environment variable name.");
      return false;
    }

    if (safeArgs.argCount() > 2) {
      status.SetError("ENV{} has too many args.");
      return false;
    }

    if (safeArgs.argCount() == 1 || safeArgs[1].empty()) {
      SysEnv::unsetEnv(envVarName);
      return true;
    }

    SysEnv::setEnv(envVarName, safeArgs[1]);
    return true;
  }

  std::string const& variable = safeArgs[0]; // VAR is always first

  // SET (VAR) // Removes the definition of VAR.
  if (safeArgs.argCount() == 1) {
    status.GetMakefile().RemoveDefinition(variable);
    return true;
  }
  // SET (VAR PARENT_SCOPE) // Removes the definition of VAR
  // in the parent scope.
  if (safeArgs.argCount() == 2 && safeArgs[1] == "PARENT_SCOPE") {
    status.GetMakefile().RaiseScope(variable, nullptr);
    return true;
  }

  // here are the remaining options
  //  SET (VAR value )
  //  SET (VAR value PARENT_SCOPE)
  //  SET (VAR CACHE TYPE "doc String" [FORCE])
  //  SET (VAR value CACHE TYPE "doc string" [FORCE])
  bool isParentScope = false;
  bool isForce = false;
  bool isCacheCmd = false;
  std::string docstring;
  std::string typeArg;
  std::string cacheArg;
  int valueStartIndex = 1;         //  default for regular set variable command
  int valueEndIndexInclusive = -1; //  default for regular set variable command
  if (safeArgs[-1] == "PARENT_SCOPE") {
    isParentScope = true;
    valueEndIndexInclusive = -2;
  } else if (safeArgs[-1] == "FORCE") {
    isCacheCmd = true;
    isForce = true;
    docstring = safeArgs[-2];
    typeArg = safeArgs[-3];
    cacheArg = safeArgs[-4];
    valueEndIndexInclusive = -5;
  } else if (safeArgs[-3] == "CACHE") {
    isCacheCmd = true;
    docstring = safeArgs[-1];
    typeArg = safeArgs[-2];
    cacheArg = safeArgs[-3];
    valueEndIndexInclusive = -4;
  } else {
    //    regular var
  }

  if (isForce) {
    if (cacheArg != "CACHE") {
      status.SetError("FORCE argument given but no CACHE argument found.");
      return false;
    }
  }
  if (isCacheCmd) {
    //    TODO: do some error checking here?
  }

  std::optional<std::string> value =
    safeArgs.collectArgs(valueStartIndex, valueEndIndexInclusive);
  if (!value) {
    if (!isCacheCmd) {
      //    Missing values and not a set cache command
      status.SetError("Missing value(s) for set command.");
      return false;
    } else {
      //  TODO: clunky.  Need to give some value so that we don't
      //  barf when actually using it for the cache command.
      value = "";
    }
  }

  if (isParentScope) {
    status.GetMakefile().RaiseScope(variable, value.value().c_str());
    return true;
  }

  cmStateEnums::CacheEntryType type =
    cmStateEnums::STRING; // required if cache
  cmValue docstringValue; // required if cache

  if (isCacheCmd) {
    //    TODO: what is this?
    if (!cmState::StringToCacheEntryType(typeArg, type)) {
      std::string m =
        "implicitly converting '" + typeArg + "' to 'STRING' type.";
      status.GetMakefile().IssueMessage(MessageType::AUTHOR_WARNING, m);
      // Setting this may not be required, since it's
      // initialized as a string. Keeping this here to
      // ensure that the type is actually converting to a string.
      type = cmStateEnums::STRING;
    }
    docstringValue = cmValue{ docstring };
  }

  // see if this is already in the cache
  cmState* state = status.GetMakefile().GetState();
  cmValue existingValue = state->GetCacheEntryValue(variable);
  if (existingValue &&
      (state->GetCacheEntryType(variable) != cmStateEnums::UNINITIALIZED)) {
    // if the set is trying to CACHE the value but the value
    // is already in the cache and the type is not internal
    // then leave now without setting any definitions in the cache
    // or the makefile
    //    TODO: what does this even mean?
    if (isCacheCmd && type != cmStateEnums::INTERNAL && !isForce) {
      return true;
    }
  }

  // if it is meant to be in the cache then define it in the cache
  if (isCacheCmd) {
    status.GetMakefile().AddCacheDefinition(
      variable, cmValue{ value.value() }, docstringValue, type, isForce);
    return true;
  }

  status.GetMakefile().AddDefinition(variable, value.value());
  return true;
}
