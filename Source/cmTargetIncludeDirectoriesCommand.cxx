/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmTargetIncludeDirectoriesCommand.h"

#include <set>

#include "cmGeneratorExpression.h"
#include "cmListFileCache.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTargetPropCommandBase.h"

namespace {

class TargetIncludeDirectoriesImpl : public cmTargetPropCommandBase
{
public:
  using cmTargetPropCommandBase::cmTargetPropCommandBase;

private:
  void HandleMissingTarget(std::string const& name) override
  {
    this->m_pMakefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("Cannot specify include directories for target \"", name,
               "\" which is not built by this project."));
  }

  bool HandleDirectContent(cmTarget* tgt,
                           std::vector<std::string> const& content,
                           bool prepend, bool system) override;

  void HandleInterfaceContent(cmTarget* tgt,
                              std::vector<std::string> const& content,
                              bool prepend, bool system) override;

  std::string Join(std::vector<std::string> const& content) override;
};

std::string TargetIncludeDirectoriesImpl::Join(
  std::vector<std::string> const& content)
{
  std::string dirs;
  std::string sep;
  std::string prefix = this->m_pMakefile->GetCurrentSourceDirectory() + "/";
  for (std::string const& it : content) {
    if (cmSystemTools::FileIsFullPath(it) ||
        cmGeneratorExpression::Find(it) == 0) {
      dirs += cmStrCat(sep, it);
    } else {
      dirs += cmStrCat(sep, prefix, it);
    }
    sep = ";";
  }
  return dirs;
}

bool TargetIncludeDirectoriesImpl::HandleDirectContent(
  cmTarget* tgt, std::vector<std::string> const& content, bool prepend,
  bool system)
{
  cmListFileBacktrace lfbt = this->m_pMakefile->GetBacktrace();
  tgt->InsertInclude(BT<std::string>(this->Join(content), lfbt), prepend);
  if (system) {
    std::string prefix = this->m_pMakefile->GetCurrentSourceDirectory() + "/";
    std::set<std::string> sdirs;
    for (std::string const& it : content) {
      if (cmSystemTools::FileIsFullPath(it) ||
          cmGeneratorExpression::Find(it) == 0) {
        sdirs.insert(it);
      } else {
        sdirs.insert(prefix + it);
      }
    }
    tgt->AddSystemIncludeDirectories(sdirs);
  }
  return true; // Successfully handled.
}

void TargetIncludeDirectoriesImpl::HandleInterfaceContent(
  cmTarget* tgt, std::vector<std::string> const& content, bool prepend,
  bool system)
{
  this->cmTargetPropCommandBase::HandleInterfaceContent(tgt, content, prepend,
                                                        system);
  if (system) {
    std::string joined = this->Join(content);
    tgt->AppendProperty("INTERFACE_SYSTEM_INCLUDE_DIRECTORIES", joined,
                        this->m_pMakefile->GetBacktrace());
  }
}

} // namespace

bool cmTargetIncludeDirectoriesCommand(std::vector<std::string> const& args,
                                       cmExecutionStatus& status)
{
  return TargetIncludeDirectoriesImpl(status).HandleArguments(
    args, "INCLUDE_DIRECTORIES",
    TargetIncludeDirectoriesImpl::PROCESS_BEFORE |
      TargetIncludeDirectoriesImpl::PROCESS_AFTER |
      TargetIncludeDirectoriesImpl::PROCESS_SYSTEM);
}
