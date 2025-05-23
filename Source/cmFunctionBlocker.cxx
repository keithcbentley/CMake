/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFunctionBlocker.h"

#include <cassert>
#include <memory> // IWYU pragma: keep
#include <sstream>
#include <string> // IWYU pragma: keep
#include <utility>

#include "cmExecutionStatus.h"
#include "cmMakefile.h"
#include "cmMessageType.h"

bool cmFunctionBlocker::IsFunctionBlocked(cmListFileFunction const& lff,
                                          cmExecutionStatus& status)
{
  if (lff.LowerCaseName() == this->StartCommandName()) {
    this->ScopeDepth++;
  } else if (lff.LowerCaseName() == this->EndCommandName()) {
    this->ScopeDepth--;
    if (this->ScopeDepth == 0U) {
      cmMakefile& mf = status.GetMakefile();
      auto self = mf.RemoveFunctionBlocker();
      assert(self.get() == this);

      cmListFileContext const& lfc = this->GetStartingContext();
      cmListFileContext closingContext =
        cmListFileContext::FromListFileFunction(lff, lfc.m_filePath);
      if (this->EndCommandSupportsArguments() &&
          !this->ArgumentsMatch(lff, mf)) {
        std::ostringstream e;
        /* clang-format off */
        e << "A logical block opening on the line\n"
          << "  " << lfc << "\n"
          << "closes on the line\n"
          << "  " << closingContext << "\n"
          << "with mis-matching arguments.";  // noqa: spellcheck disable-line
        /* clang-format on */
        mf.IssueMessage(MessageType::AUTHOR_WARNING, e.str());
      } else if (!this->EndCommandSupportsArguments() &&
                 !lff.Arguments().empty()) {
        std::ostringstream e;
        /* clang-format off */
        e << "A logical block closing on the line\n"
          "  " << closingContext << "\n"
          "has unexpected arguments.";
        /* clang-format on */
        mf.IssueMessage(MessageType::AUTHOR_WARNING, e.str());
      }

      return this->Replay(std::move(this->Functions), status);
    }
  }

  this->Functions.push_back(lff);
  return true;
}
