#pragma once

#include <string>
#include <vector>

class cmExecutionStatus;

bool cmAddCompileDefinitionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddCompileOptionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddCustomCommandCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddCustomTargetCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddDefinitionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddDependenciesCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddExecutableCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddLibraryCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddLinkOptionsCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddSubDirectoryCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);

bool cmAddTestCommand(
  std::vector<std::string> const& args,
  cmExecutionStatus& status);
