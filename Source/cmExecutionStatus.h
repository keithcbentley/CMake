/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include <cmConfigure.h> // IWYU pragma: keep

#include <string>
#include <vector>

#include <cm/optional>

class cmMakefile;

/** \class cmExecutionStatus
 * \brief Superclass for all command status classes
 *
 * when a command is involked it may set values on a command status instance
 */
class cmExecutionStatus
{
public:
  cmExecutionStatus(cmMakefile& makefile)
    : m_pMakefile(makefile)
    , Error("unknown error.")
  {
  }

  cmMakefile& GetMakefile() { return this->m_pMakefile; }

  void SetError(std::string const& e) { this->Error = e; }
  std::string const& GetError() const { return this->Error; }

  void SetReturnInvoked()
  {
    this->Variables.clear();
    this->ReturnInvoked = true;
  }
  void SetReturnInvoked(std::vector<std::string> variables)
  {
    this->Variables = std::move(variables);
    this->ReturnInvoked = true;
  }
  bool GetReturnInvoked() const { return this->ReturnInvoked; }
  std::vector<std::string> const& GetReturnVariables() const
  {
    return this->Variables;
  }

  void SetBreakInvoked() { this->BreakInvoked = true; }
  bool GetBreakInvoked() const { return this->BreakInvoked; }

  void SetContinueInvoked() { this->ContinueInvoked = true; }
  bool GetContinueInvoked() const { return this->ContinueInvoked; }

  void SetNestedError() { this->NestedError = true; }
  bool GetNestedError() const { return this->NestedError; }

  void SetExitCode(int code) noexcept { this->ExitCode = code; }
  bool HasExitCode() const noexcept { return this->ExitCode.has_value(); }
  void CleanExitCode() noexcept { this->ExitCode.reset(); }
  int GetExitCode() const noexcept { return this->ExitCode.value_or(-1); }

private:
  cmMakefile& m_pMakefile;
  std::string Error;
  bool ReturnInvoked = false;
  bool BreakInvoked = false;
  bool ContinueInvoked = false;
  bool NestedError = false;
  cm::optional<int> ExitCode;
  std::vector<std::string> Variables;
};
