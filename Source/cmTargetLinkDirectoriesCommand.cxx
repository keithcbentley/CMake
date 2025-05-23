/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmTargetLinkDirectoriesCommand.h"

#include "cmGeneratorExpression.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTargetPropCommandBase.h"

namespace {

class TargetLinkDirectoriesImpl : public cmTargetPropCommandBase
{
public:
  using cmTargetPropCommandBase::cmTargetPropCommandBase;

private:
  void HandleMissingTarget(std::string const& name) override
  {
    this->m_pMakefile->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat("Cannot specify link directories for target \"", name,
               "\" which is not built by this project."));
  }

  std::string Join(std::vector<std::string> const& content) override;

  bool HandleDirectContent(cmTarget* tgt,
                           std::vector<std::string> const& content,
                           bool prepend, bool /*system*/) override
  {
    cmListFileBacktrace lfbt = this->m_pMakefile->GetBacktrace();
    tgt->InsertLinkDirectory(BT<std::string>(this->Join(content), lfbt),
                             prepend);
    return true; // Successfully handled.
  }
};

std::string TargetLinkDirectoriesImpl::Join(
  std::vector<std::string> const& content)
{
  std::vector<std::string> directories;

  for (auto const& dir : content) {
    auto unixPath = dir;
    cmSystemTools::ConvertToUnixSlashes(unixPath);
    if (!cmSystemTools::FileIsFullPath(unixPath) &&
        !cmGeneratorExpression::StartsWithGeneratorExpression(unixPath)) {
      auto tmp = this->m_pMakefile->GetCurrentSourceDirectory();
      tmp += "/";
      tmp += unixPath;
      unixPath = tmp;
    }
    directories.push_back(unixPath);
  }

  return cmList::to_string(directories);
}

} // namespace

bool cmTargetLinkDirectoriesCommand(std::vector<std::string> const& args,
                                    cmExecutionStatus& status)
{
  return TargetLinkDirectoriesImpl(status).HandleArguments(
    args, "LINK_DIRECTORIES", TargetLinkDirectoriesImpl::PROCESS_BEFORE);
}
