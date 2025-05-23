/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cm/optional>

#include "cmCustomCommandLines.h"
#include "cmListFileCache.h"

class cmCustomCommand;
class cmLocalGenerator;

class cmCustomCommandGenerator
{
  std::string GetInternalDepfileName(std::string const&,
                                     std::string const&) const;

  cmCustomCommand const* CC;
  std::string OutputConfig;
  std::string CommandConfig;
  std::string Target;
  cmLocalGenerator* LG;
  bool OldStyle;
  bool MakeVars;
  cmCustomCommandLines CommandLines;
  std::vector<std::vector<std::string>> EmulatorsWithArguments;
  std::vector<std::string> Outputs;
  std::vector<std::string> Byproducts;
  std::vector<std::string> Depends;
  std::string WorkingDirectory;
  std::set<BT<std::pair<std::string, bool>>> Utilities;
  std::function<std::string(std::string const&, std::string const&)>
    ComputeInternalDepfile;

  void FillEmulatorsWithArguments();
  std::vector<std::string> GetCrossCompilingEmulator(unsigned int c) const;
  char const* GetArgv0Location(unsigned int c) const;

public:
  cmCustomCommandGenerator(
    cmCustomCommand const& m_pCustomCommand, std::string config, cmLocalGenerator* lg,
    bool transformDepfile = true, cm::optional<std::string> crossConfig = {},
    std::function<std::string(std::string const&, std::string const&)>
      computeInternalDepfile = {});
  cmCustomCommandGenerator(cmCustomCommandGenerator const&) = delete;
  cmCustomCommandGenerator(cmCustomCommandGenerator&&) = default;
  cmCustomCommandGenerator& operator=(cmCustomCommandGenerator const&) =
    delete;
  cmCustomCommandGenerator& operator=(cmCustomCommandGenerator&&) = default;
  cmCustomCommand const& GetCC() const { return *(this->CC); }
  unsigned int GetNumberOfCommands() const;
  std::string GetCommand(unsigned int c) const;
  void AppendArguments(unsigned int c, std::string& cmd) const;
  cm::optional<std::string> GetComment() const;
  std::string GetWorkingDirectory() const;
  std::vector<std::string> const& GetOutputs() const;
  std::vector<std::string> const& GetByproducts() const;
  std::vector<std::string> const& GetDepends() const;
  std::set<BT<std::pair<std::string, bool>>> const& GetUtilities() const;
  bool HasOnlyEmptyCommandLines() const;
  std::string GetDepfile() const;
  std::string GetFullDepfile() const;
  std::string GetInternalDepfile() const;

  std::string const& GetOutputConfig() const { return this->OutputConfig; }
  std::string const& GetCommandConfig() const { return this->CommandConfig; }
};
