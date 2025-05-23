/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "cmGeneratorTarget.h"
#include "cmLocalGenerator.h"

class cmGlobalGenerator;
class cmSourceFile;
class cmCustomCommand;
class cmMakefile;

class cmTargetTraceDependencies
{
public:
  cmTargetTraceDependencies(cmGeneratorTarget* target);
  void m_trace();

private:
  cmGeneratorTarget* GeneratorTarget;
  cmMakefile* m_pMakefile;
  cmLocalGenerator* LocalGenerator;
  cmGlobalGenerator const* m_pGlobalGenerator;
  using SourceEntry = cmGeneratorTarget::SourceEntry;
  SourceEntry* CurrentEntry;
  std::queue<cmSourceFile*> SourceQueue;
  std::set<cmSourceFile*> SourcesQueued;
  using NameMapType = std::map<std::string, cmSourcesWithOutput>;
  NameMapType NameMap;
  std::vector<std::string> NewSources;

  void QueueSource(cmSourceFile* sf);
  void FollowName(std::string const& name);
  void FollowNames(std::vector<std::string> const& names);
  bool IsUtility(std::string const& dep);
  void CheckCustomCommand(cmCustomCommand const& m_pCustomCommand);
  void CheckCustomCommands(std::vector<cmCustomCommand> const& commands);
};
