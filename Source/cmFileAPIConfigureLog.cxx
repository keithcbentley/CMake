/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFileAPIConfigureLog.h"

#include <cm3p/json/value.h>

#include "cmFileAPI.h"
#include "cmStringAlgorithms.h"
#include "cmake.h"

namespace {

class m_configureLog
{
  cmFileAPI& m_fileAPI;
  unsigned long Version;

  Json::Value DumpPath();
  Json::Value DumpEventKindNames();

public:
  m_configureLog(cmFileAPI& fileAPI, unsigned long version);
  Json::Value Dump();
};

m_configureLog::m_configureLog(cmFileAPI& fileAPI, unsigned long version)
  : m_fileAPI(fileAPI)
  , Version(version)
{
  static_cast<void>(this->Version);
}

Json::Value m_configureLog::Dump()
{
  Json::Value configureLog = Json::objectValue;
  configureLog["path"] = this->DumpPath();
  configureLog["eventKindNames"] = this->DumpEventKindNames();
  return configureLog;
}

Json::Value m_configureLog::DumpPath()
{
  return cmStrCat(this->m_fileAPI.GetCMakeInstance()->GetHomeOutputDirectory(),
                  "/CMakeFiles/CMakeConfigureLog.yaml");
}

Json::Value m_configureLog::DumpEventKindNames()
{
  // Report at most one version of each event kind.
  // If a new event kind is added, increment ConfigureLogV1Minor.
  // If a new version of an existing event kind is added, a new
  // major version of the configureLog object kind is needed.
  Json::Value eventKindNames = Json::arrayValue;
  if (this->Version == 1) {
    eventKindNames.append("message-v1");     // WriteMessageEvent
    eventKindNames.append("try_compile-v1"); // WriteTryCompileEvent
    eventKindNames.append("try_run-v1");     // WriteTryRunEvent
  }
  return eventKindNames;
}
}

Json::Value cmFileAPIConfigureLogDump(cmFileAPI& fileAPI,
                                      unsigned long version)
{
  m_configureLog configureLog(fileAPI, version);
  return configureLog.Dump();
}
