/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmTestGenerator.h"

#include <algorithm>
#include <cstddef> // IWYU pragma: keep
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "cmGeneratorExpression.h"
#include "cmGeneratorTarget.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmOutputConverter.h"
#include "cmPolicies.h"
#include "cmPropertyMap.h"
#include "cmRange.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTest.h"
#include "cmValue.h"

namespace /* anonymous */
{

bool needToQuoteTestName(cmMakefile const& mf, std::string const& name)
{
  // Determine if policy CMP0110 is set to NEW.
  switch (mf.GetPolicyStatus(cmPolicies::CMP0110)) {
    case cmPolicies::WARN:
      // Only warn if a forbidden character is used in the name.
      if (name.find_first_of("$[] #;\t\n\"\\") != std::string::npos) {
        mf.IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat(cmPolicies::GetPolicyWarning(cmPolicies::CMP0110),
                   "\nThe following name given to add_test() is invalid if "
                   "CMP0110 is not set or set to OLD:\n  `",
                   name, "´\n"));
      }
      CM_FALLTHROUGH;
    case cmPolicies::OLD:
      // OLD behavior is to not quote the test's name.
      return false;
    case cmPolicies::NEW:
    default:
      // NEW behavior is to quote the test's name.
      return true;
  }
}

std::size_t countMaxConsecutiveEqualSigns(std::string const& name)
{
  std::size_t max = 0;
  auto startIt = find(name.begin(), name.end(), '=');
  auto endIt = startIt;
  for (; startIt != name.end(); startIt = find(endIt, name.end(), '=')) {
    endIt =
      find_if_not(startIt + 1, name.end(), [](char c) { return c == '='; });
    max =
      std::max(max, static_cast<std::size_t>(std::distance(startIt, endIt)));
  }
  return max;
}

} // End: anonymous namespace

cmTestGenerator::cmTestGenerator(
  cmTest* test, std::vector<std::string> const& configurations)
  : cmScriptGenerator("CTEST_CONFIGURATION_TYPE", configurations)
  , Test(test)
{
  this->ActionsPerConfig = !test->GetOldStyle();
  this->TestGenerated = false;
  this->LG = nullptr;
}

cmTestGenerator::~cmTestGenerator() = default;

void cmTestGenerator::Compute(cmLocalGenerator* lg)
{
  this->LG = lg;
}

bool cmTestGenerator::TestsForConfig(std::string const& config)
{
  return this->GeneratesForConfig(config);
}

cmTest* cmTestGenerator::GetTest() const
{
  return this->Test;
}

void cmTestGenerator::GenerateScriptConfigs(std::ostream& os, Indent indent)
{
  // Create the tests.
  this->cmScriptGenerator::GenerateScriptConfigs(os, indent);
}

void cmTestGenerator::GenerateScriptActions(std::ostream& os, Indent indent)
{
  if (this->ActionsPerConfig) {
    // This is the per-config generation in a single-configuration
    // build generator case.  The superclass will call our per-config
    // method.
    this->cmScriptGenerator::GenerateScriptActions(os, indent);
  } else {
    // This is an old-style test, so there is only one config.
    // assert(this->Test->GetOldStyle());
    this->GenerateOldStyle(os, indent);
  }
}

void cmTestGenerator::GenerateScriptForConfig(std::ostream& os,
                                              std::string const& config,
                                              Indent indent)
{
  this->TestGenerated = true;

  // Set up generator expression evaluation context.
  cmGeneratorExpression ge(*this->Test->GetMakefile()->GetCMakeInstance(),
                           this->Test->GetBacktrace());

  // Determine if policy CMP0110 is set to NEW.
  bool const quote_test_name =
    needToQuoteTestName(*this->Test->GetMakefile(), this->Test->GetName());
  // Determine the number of equal-signs needed for quoting test name with
  // [==[...]==] syntax.
  std::string const equalSigns(
    1 + countMaxConsecutiveEqualSigns(this->Test->GetName()), '=');

  // Start the test command.
  if (quote_test_name) {
    os << indent << "add_test([" << equalSigns << "[" << this->Test->GetName()
       << "]" << equalSigns << "] ";
  } else {
    os << indent << "add_test(" << this->Test->GetName() << " ";
  }

  // Evaluate command line arguments
  cmList argv{
    this->EvaluateCommandLineArguments(this->Test->GetCommand(), ge, config),
    // Expand arguments if COMMAND_EXPAND_LISTS is set
    this->Test->GetCommandExpandLists() ? cmList::ExpandElements::Yes
                                        : cmList::ExpandElements::No,
    cmList::EmptyElements::Yes
  };
  // Expanding lists on an empty command may have left it empty
  if (argv.empty()) {
    argv.emplace_back();
  }

  // Check whether the command executable is a target whose name is to
  // be translated.
  std::string exe = argv[0];
  cmGeneratorTarget* target = this->LG->FindGeneratorTargetToUse(exe);
  if (target && target->GetType() == cmStateEnums::EXECUTABLE) {
    // Use the target file on disk.
    exe = target->GetFullPath(config);

    auto addLauncher = [this, &config, &ge, &os,
                        target](std::string const& propertyName) {
      cmValue launcher = target->GetProperty(propertyName);
      if (!cmNonempty(launcher)) {
        return;
      }
      auto const propVal = ge.Parse(*launcher)->Evaluate(this->LG, config);
      cmList launcherWithArgs(propVal, cmList::ExpandElements::Yes,
                              this->Test->GetCMP0178() == cmPolicies::NEW
                                ? cmList::EmptyElements::Yes
                                : cmList::EmptyElements::No);
      if (!launcherWithArgs.empty() && !launcherWithArgs[0].empty()) {
        if (this->Test->GetCMP0178() == cmPolicies::WARN) {
          cmList argsWithEmptyValuesPreserved(
            propVal, cmList::ExpandElements::Yes, cmList::EmptyElements::Yes);
          if (launcherWithArgs != argsWithEmptyValuesPreserved) {
            this->Test->GetMakefile()->IssueMessage(
              MessageType::AUTHOR_WARNING,
              cmStrCat("The ", propertyName, " property of target '",
                       target->GetName(),
                       "' contains empty list items. Those empty items are "
                       "being silently discarded to preserve backward "
                       "compatibility.\n",
                       cmPolicies::GetPolicyWarning(cmPolicies::CMP0178)));
          }
        }
        std::string launcherExe(launcherWithArgs[0]);
        cmSystemTools::ConvertToUnixSlashes(launcherExe);
        os << cmOutputConverter::EscapeForCMake(launcherExe) << " ";
        for (std::string const& arg :
             cmMakeRange(launcherWithArgs).advance(1)) {
          if (arg.empty()) {
            os << "\"\" ";
          } else {
            os << cmOutputConverter::EscapeForCMake(arg) << " ";
          }
        }
      }
    };

    // Prepend with the test launcher if specified.
    addLauncher("TEST_LAUNCHER");

    // Prepend with the emulator when cross compiling if required.
    if (!this->GetTest()->GetCMP0158IsNew() ||
        this->LG->GetMakefile()->IsOn("CMAKE_CROSSCOMPILING")) {
      addLauncher("CROSSCOMPILING_EMULATOR");
    }
  } else {
    // Use the command name given.
    cmSystemTools::ConvertToUnixSlashes(exe);
  }

  // Generate the command line with full escapes.
  os << cmOutputConverter::EscapeForCMake(exe);

  for (auto const& arg : cmMakeRange(argv).advance(1)) {
    os << " " << cmOutputConverter::EscapeForCMake(arg);
  }

  // Finish the test command.
  os << ")\n";

  // Output properties for the test.
  if (quote_test_name) {
    os << indent << "set_tests_properties([" << equalSigns << "["
       << this->Test->GetName() << "]" << equalSigns << "] PROPERTIES ";
  } else {
    os << indent << "set_tests_properties(" << this->Test->GetName()
       << " PROPERTIES ";
  }
  for (auto const& i : this->Test->GetProperties().GetList()) {
    os << " " << i.first << " "
       << cmOutputConverter::EscapeForCMake(
            ge.Parse(i.second)->Evaluate(this->LG, config));
  }
  this->GenerateInternalProperties(os);
  os << ")\n";
}

void cmTestGenerator::GenerateScriptNoConfig(std::ostream& os, Indent indent)
{
  // Determine if policy CMP0110 is set to NEW.
  bool const quote_test_name =
    needToQuoteTestName(*this->Test->GetMakefile(), this->Test->GetName());
  // Determine the number of equal-signs needed for quoting test name with
  // [==[...]==] syntax.
  std::string const equalSigns(
    1 + countMaxConsecutiveEqualSigns(this->Test->GetName()), '=');

  if (quote_test_name) {
    os << indent << "add_test([" << equalSigns << "[" << this->Test->GetName()
       << "]" << equalSigns << "] NOT_AVAILABLE)\n";
  } else {
    os << indent << "add_test(" << this->Test->GetName()
       << " NOT_AVAILABLE)\n";
  }
}

bool cmTestGenerator::NeedsScriptNoConfig() const
{
  return (this->TestGenerated &&    // test generated for at least one config
          this->ActionsPerConfig && // test is config-aware
          this->Configurations.empty() &&      // test runs in all configs
          !this->ConfigurationTypes->empty()); // config-dependent command
}

void cmTestGenerator::GenerateOldStyle(std::ostream& fout, Indent indent)
{
  this->TestGenerated = true;

  // Determine if policy CMP0110 is set to NEW.
  bool const quote_test_name =
    needToQuoteTestName(*this->Test->GetMakefile(), this->Test->GetName());
  // Determine the number of equal-signs needed for quoting test name with
  // [==[...]==] syntax.
  std::string const equalSigns(
    1 + countMaxConsecutiveEqualSigns(this->Test->GetName()), '=');

  // Get the test command line to be executed.
  std::vector<std::string> const& command = this->Test->GetCommand();

  std::string exe = command[0];
  cmSystemTools::ConvertToUnixSlashes(exe);
  if (quote_test_name) {
    fout << indent << "add_test([" << equalSigns << "["
         << this->Test->GetName() << "]" << equalSigns << "] \"" << exe
         << "\"";
  } else {
    fout << indent << "add_test(" << this->Test->GetName() << " \"" << exe
         << "\"";
  }

  for (std::string const& arg : cmMakeRange(command).advance(1)) {
    // Just double-quote all arguments so they are re-parsed
    // correctly by the test system.
    fout << " \"";
    for (char c : arg) {
      // Escape quotes within arguments.  We should escape
      // backslashes too but we cannot because it makes the result
      // inconsistent with previous behavior of this command.
      if (c == '"') {
        fout << '\\';
      }
      fout << c;
    }
    fout << '"';
  }
  fout << ")\n";

  // Output properties for the test.
  if (quote_test_name) {
    fout << indent << "set_tests_properties([" << equalSigns << "["
         << this->Test->GetName() << "]" << equalSigns << "] PROPERTIES ";
  } else {
    fout << indent << "set_tests_properties(" << this->Test->GetName()
         << " PROPERTIES ";
  }
  for (auto const& i : this->Test->GetProperties().GetList()) {
    fout << " " << i.first << " "
         << cmOutputConverter::EscapeForCMake(i.second);
  }
  this->GenerateInternalProperties(fout);
  fout << ")\n";
}

void cmTestGenerator::GenerateInternalProperties(std::ostream& os)
{
  cmListFileBacktrace bt = this->Test->GetBacktrace();
  if (bt.Empty()) {
    return;
  }

  os << " "
     << "_BACKTRACE_TRIPLES"
     << " \"";

  bool prependTripleSeparator = false;
  while (!bt.Empty()) {
    auto const& entry = bt.Top();
    if (prependTripleSeparator) {
      os << ";";
    }
    os << entry.m_filePath << ";" << entry.Line << ";" << entry.m_name;
    bt = bt.Pop();
    prependTripleSeparator = true;
  }

  os << '"';
}

std::vector<std::string> cmTestGenerator::EvaluateCommandLineArguments(
  std::vector<std::string> const& argv, cmGeneratorExpression& ge,
  std::string const& config) const
{
  // Evaluate executable name and arguments
  auto evaluatedRange =
    cmMakeRange(argv).transform([&](std::string const& arg) {
      return ge.Parse(arg)->Evaluate(this->LG, config);
    });

  return { evaluatedRange.begin(), evaluatedRange.end() };
}
