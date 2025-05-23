/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmTryRunCommand.h"

#include <stdexcept>

#include <cm/optional>
#include <cmext/string_view>

#include "cmsys/FStream.hxx"

#include "cmArgumentParserTypes.h"
#include "cmConfigureLog.h"
#include "cmCoreTryCompile.h"
#include "cmDuration.h"
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
#include "cmake.h"

namespace {
struct cmTryRunResult
{
  bool VariableCached = true;
  std::string Variable;
  cm::optional<std::string> Stdout;
  cm::optional<std::string> Stderr;
  cm::optional<std::string> ExitCode;
};

#ifndef CMAKE_BOOTSTRAP
void WriteTryRunEvent(cmConfigureLog& log, cmMakefile const& mf,
                      cmTryCompileResult const& compileResult,
                      cmTryRunResult const& runResult)
{
  // Keep in sync with cmFileAPIConfigureLog's DumpEventKindNames.
  static std::vector<unsigned long> const LogVersionsWithTryRunV1{ 1 };

  if (log.IsAnyLogVersionEnabled(LogVersionsWithTryRunV1)) {
    log.BeginEvent("try_run-v1", mf);
    cmCoreTryCompile::WriteTryCompileEventFields(log, compileResult);

    log.BeginObject("runResult"_s);
    log.WriteValue("variable"_s, runResult.Variable);
    log.WriteValue("cached"_s, runResult.VariableCached);
    if (runResult.Stdout) {
      log.WriteLiteralTextBlock("stdout"_s, *runResult.Stdout);
    }
    if (runResult.Stderr) {
      log.WriteLiteralTextBlock("stderr"_s, *runResult.Stderr);
    }
    if (runResult.ExitCode) {
      try {
        log.WriteValue("exitCode"_s, std::stoi(*runResult.ExitCode));
      } catch (std::invalid_argument const&) {
        log.WriteValue("exitCode"_s, *runResult.ExitCode);
      }
    }
    log.EndObject();
    log.EndEvent();
  }
}
#endif

class TryRunCommandImpl : public cmCoreTryCompile
{
public:
  TryRunCommandImpl(cmMakefile* mf)
    : cmCoreTryCompile(mf)
  {
  }

  bool TryRunCode(std::vector<std::string> const& args);

  void RunExecutable(std::string const& runArgs,
                     cm::optional<std::string> const& workDir,
                     std::string* runOutputContents,
                     std::string* runOutputStdOutContents,
                     std::string* runOutputStdErrContents);
  void DoNotRunExecutable(std::string const& runArgs,
                          cm::optional<std::string> const& srcFile,
                          std::string const& compileResultVariable,
                          std::string* runOutputContents,
                          std::string* runOutputStdOutContents,
                          std::string* runOutputStdErrContents,
                          bool stdOutErrRequired);

  bool NoCache;
  std::string RunResultVariable;
};

bool TryRunCommandImpl::TryRunCode(std::vector<std::string> const& argv)
{
  this->RunResultVariable = argv[0];
  cmCoreTryCompile::Arguments arguments =
    this->ParseArgs(cmMakeRange(argv).advance(1), true);
  if (!arguments) {
    return true;
  }
  this->NoCache = arguments.NoCache;

  // although they could be used together, don't allow it, because
  // using OUTPUT_VARIABLE makes crosscompiling harder
  if (arguments.OutputVariable &&
      (arguments.CompileOutputVariable || arguments.RunOutputVariable ||
       arguments.RunOutputStdOutVariable ||
       arguments.RunOutputStdErrVariable)) {
    cmSystemTools::Error(
      "You cannot use OUTPUT_VARIABLE together with COMPILE_OUTPUT_VARIABLE "
      ", RUN_OUTPUT_VARIABLE, RUN_OUTPUT_STDOUT_VARIABLE or "
      "RUN_OUTPUT_STDERR_VARIABLE. "
      "Please use only COMPILE_OUTPUT_VARIABLE, RUN_OUTPUT_VARIABLE, "
      "RUN_OUTPUT_STDOUT_VARIABLE "
      "and/or RUN_OUTPUT_STDERR_VARIABLE.");
    return false;
  }

  if ((arguments.RunOutputStdOutVariable ||
       arguments.RunOutputStdErrVariable) &&
      arguments.RunOutputVariable) {
    cmSystemTools::Error(
      "You cannot use RUN_OUTPUT_STDOUT_VARIABLE or "
      "RUN_OUTPUT_STDERR_VARIABLE together "
      "with RUN_OUTPUT_VARIABLE. Please use only COMPILE_OUTPUT_VARIABLE or "
      "RUN_OUTPUT_STDOUT_VARIABLE and/or RUN_OUTPUT_STDERR_VARIABLE.");
    return false;
  }

  if (arguments.RunWorkingDirectory) {
    cmSystemTools::MakeDirectory(*arguments.RunWorkingDirectory);
    //   cmSystemTools::Error(cmStrCat("Error creating working directory \"",
    //                                *arguments.RunWorkingDirectory, "\"."));
    //  return false;
    //}
  }

  bool captureRunOutput = false;
  if (arguments.OutputVariable) {
    captureRunOutput = true;
  } else if (arguments.CompileOutputVariable) {
    arguments.OutputVariable = arguments.CompileOutputVariable;
  }

  // Capture the split output for the configure log unless the caller
  // requests combined output to be captured by a variable.
  bool captureRunOutputStdOutErr = true;
  if (!arguments.RunOutputStdOutVariable &&
      !arguments.RunOutputStdErrVariable) {
    if (arguments.RunOutputVariable) {
      captureRunOutput = true;
      captureRunOutputStdOutErr = false;
    } else if (arguments.OutputVariable) {
      captureRunOutputStdOutErr = false;
    }
  }

  // do the try compile
  cm::optional<cmTryCompileResult> compileResult =
    this->TryCompileCode(arguments, cmStateEnums::EXECUTABLE);

  cmTryRunResult runResult;
  runResult.Variable = this->RunResultVariable;
  runResult.VariableCached = !arguments.NoCache;

  // now try running the command if it compiled
  if (compileResult && compileResult->ExitCode == 0) {
    if (this->OutputFile.empty()) {
      cmSystemTools::Error(this->FindErrorMessage);
    } else {
      std::string runArgs;
      if (arguments.RunArgs) {
        runArgs = cmStrCat(" ", cmJoin(*arguments.RunArgs, " "));
      }

      // "run" it and capture the output
      std::string runOutputContents;
      std::string runOutputStdOutContents;
      std::string runOutputStdErrContents;
      if (this->m_pMakefile->IsOn("CMAKE_CROSSCOMPILING") &&
          !this->m_pMakefile->IsDefinitionSet("CMAKE_CROSSCOMPILING_EMULATOR")) {
        // We only require the stdout/stderr cache entries if the project
        // actually asked for the values, not just for logging.
        bool const stdOutErrRequired = (arguments.RunOutputStdOutVariable ||
                                        arguments.RunOutputStdErrVariable);
        this->DoNotRunExecutable(
          runArgs, arguments.SourceDirectoryOrFile,
          *arguments.CompileResultVariable,
          captureRunOutput ? &runOutputContents : nullptr,
          captureRunOutputStdOutErr ? &runOutputStdOutContents : nullptr,
          captureRunOutputStdOutErr ? &runOutputStdErrContents : nullptr,
          stdOutErrRequired);
      } else {
        this->RunExecutable(
          runArgs, arguments.RunWorkingDirectory,
          captureRunOutput ? &runOutputContents : nullptr,
          captureRunOutputStdOutErr ? &runOutputStdOutContents : nullptr,
          captureRunOutputStdOutErr ? &runOutputStdErrContents : nullptr);
      }

      if (captureRunOutputStdOutErr) {
        runResult.Stdout = runOutputStdOutContents;
        runResult.Stderr = runOutputStdErrContents;
      } else {
        runResult.Stdout = runOutputContents;
      }

      if (cmValue ec =
            this->m_pMakefile->GetDefinition(this->RunResultVariable)) {
        runResult.ExitCode = *ec;
      }

      // now put the output into the variables
      if (arguments.RunOutputVariable) {
        this->m_pMakefile->AddDefinition(*arguments.RunOutputVariable,
                                      runOutputContents);
      }
      if (arguments.RunOutputStdOutVariable) {
        this->m_pMakefile->AddDefinition(*arguments.RunOutputStdOutVariable,
                                      runOutputStdOutContents);
      }
      if (arguments.RunOutputStdErrVariable) {
        this->m_pMakefile->AddDefinition(*arguments.RunOutputStdErrVariable,
                                      runOutputStdErrContents);
      }

      if (arguments.OutputVariable && !arguments.CompileOutputVariable) {
        // if the TryCompileCore saved output in this outputVariable then
        // prepend that output to this output
        cmValue compileOutput =
          this->m_pMakefile->GetDefinition(*arguments.OutputVariable);
        if (compileOutput) {
          runOutputContents = *compileOutput + runOutputContents;
        }
        this->m_pMakefile->AddDefinition(*arguments.OutputVariable,
                                      runOutputContents);
      }
    }
  }

#ifndef CMAKE_BOOTSTRAP
  if (compileResult && !arguments.NoLog) {
    cmMakefile const& mf = *(this->m_pMakefile);
    if (cmConfigureLog* log = mf.GetCMakeInstance()->GetConfigureLog()) {
      WriteTryRunEvent(*log, mf, *compileResult, runResult);
    }
  }
#endif

  // if we created a directory etc, then cleanup after ourselves
  if (!this->m_pMakefile->GetCMakeInstance()->GetDebugTryCompile()) {
    this->CleanupFiles(this->m_binaryDirectory);
  }
  return true;
}

void TryRunCommandImpl::RunExecutable(std::string const& runArgs,
                                      cm::optional<std::string> const& workDir,
                                      std::string* out, std::string* stdOut,
                                      std::string* stdErr)
{
  int retVal = -1;

  std::string finalCommand;
  std::string const& emulator =
    this->m_pMakefile->GetSafeDefinition("CMAKE_CROSSCOMPILING_EMULATOR");
  if (!emulator.empty()) {
    cmList emulatorWithArgs{ emulator };
    finalCommand += cmStrCat(
      cmSystemTools::ConvertToRunCommandPath(emulatorWithArgs[0]), ' ',
      cmWrap("\"", cmMakeRange(emulatorWithArgs).advance(1), "\"", " "), ' ');
  }
  finalCommand += cmSystemTools::ConvertToRunCommandPath(this->OutputFile);
  if (!runArgs.empty()) {
    finalCommand += runArgs;
  }
  bool worked = cmSystemTools::RunSingleCommand(
    finalCommand, stdOut || stdErr ? stdOut : out,
    stdOut || stdErr ? stdErr : out, &retVal,
    workDir ? workDir->c_str() : nullptr, cmSystemTools::OUTPUT_NONE,
    cmDuration::zero());
  // set the run var
  std::string retStr = worked ? std::to_string(retVal) : "FAILED_TO_RUN";
  if (this->NoCache) {
    this->m_pMakefile->AddDefinition(this->RunResultVariable, retStr);
  } else {
    this->m_pMakefile->AddCacheDefinition(this->RunResultVariable, retStr,
                                       "Result of try_run()",
                                       cmStateEnums::INTERNAL);
  }
}

/* This is only used when cross compiling. Instead of running the
 executable, two cache variables are created which will hold the results
 the executable would have produced.
*/
void TryRunCommandImpl::DoNotRunExecutable(
  std::string const& runArgs, cm::optional<std::string> const& srcFile,
  std::string const& compileResultVariable, std::string* out,
  std::string* stdOut, std::string* stdErr, bool stdOutErrRequired)
{
  // copy the executable out of the CMakeFiles/ directory, so it is not
  // removed at the end of try_run() and the user can run it manually
  // on the target platform.
  std::string copyDest =
    cmStrCat(this->m_pMakefile->GetHomeOutputDirectory(), "/CMakeFiles/",
             cmSystemTools::GetFilenameWithoutExtension(this->OutputFile), '-',
             this->RunResultVariable,
             cmSystemTools::GetFilenameExtension(this->OutputFile));
  cmSystemTools::CopyFileAlways(this->OutputFile, copyDest);

  std::string resultFileName =
    cmStrCat(this->m_pMakefile->GetHomeOutputDirectory(), "/TryRunResults.cmake");

  std::string detailsString = cmStrCat("For details see ", resultFileName);

  std::string internalRunOutputName =
    this->RunResultVariable + "__TRYRUN_OUTPUT";
  std::string internalRunOutputStdOutName =
    this->RunResultVariable + "__TRYRUN_OUTPUT_STDOUT";
  std::string internalRunOutputStdErrName =
    this->RunResultVariable + "__TRYRUN_OUTPUT_STDERR";
  bool error = false;

  if (!this->m_pMakefile->GetDefinition(this->RunResultVariable)) {
    // if the variables doesn't exist, create it with a helpful error text
    // and mark it as advanced
    std::string comment =
      cmStrCat("Run result of try_run(), indicates whether the executable "
               "would have been able to run on its target platform.\n",
               detailsString);
    this->m_pMakefile->AddCacheDefinition(this->RunResultVariable,
                                       "PLEASE_FILL_OUT-FAILED_TO_RUN",
                                       comment, cmStateEnums::STRING);

    cmState* state = this->m_pMakefile->GetState();
    cmValue existingValue = state->GetCacheEntryValue(this->RunResultVariable);
    if (existingValue) {
      state->SetCacheEntryProperty(this->RunResultVariable, "ADVANCED", "1");
    }

    error = true;
  }

  // is the output from the executable used ?
  if (stdOutErrRequired) {
    if (!this->m_pMakefile->GetDefinition(internalRunOutputStdOutName)) {
      // if the variables doesn't exist, create it with a helpful error text
      // and mark it as advanced
      std::string comment = cmStrCat(
        "Output of try_run(), contains the text, which the executable "
        "would have printed on stdout on its target platform.\n",
        detailsString);

      this->m_pMakefile->AddCacheDefinition(internalRunOutputStdOutName,
                                         "PLEASE_FILL_OUT-NOTFOUND", comment,
                                         cmStateEnums::STRING);
      cmState* state = this->m_pMakefile->GetState();
      cmValue existing =
        state->GetCacheEntryValue(internalRunOutputStdOutName);
      if (existing) {
        state->SetCacheEntryProperty(internalRunOutputStdOutName, "ADVANCED",
                                     "1");
      }

      error = true;
    }

    if (!this->m_pMakefile->GetDefinition(internalRunOutputStdErrName)) {
      // if the variables doesn't exist, create it with a helpful error text
      // and mark it as advanced
      std::string comment = cmStrCat(
        "Output of try_run(), contains the text, which the executable "
        "would have printed on stderr on its target platform.\n",
        detailsString);

      this->m_pMakefile->AddCacheDefinition(internalRunOutputStdErrName,
                                         "PLEASE_FILL_OUT-NOTFOUND", comment,
                                         cmStateEnums::STRING);
      cmState* state = this->m_pMakefile->GetState();
      cmValue existing =
        state->GetCacheEntryValue(internalRunOutputStdErrName);
      if (existing) {
        state->SetCacheEntryProperty(internalRunOutputStdErrName, "ADVANCED",
                                     "1");
      }

      error = true;
    }
  } else if (out) {
    if (!this->m_pMakefile->GetDefinition(internalRunOutputName)) {
      // if the variables doesn't exist, create it with a helpful error text
      // and mark it as advanced
      std::string comment = cmStrCat(
        "Output of try_run(), contains the text, which the executable "
        "would have printed on stdout and stderr on its target platform.\n",
        detailsString);

      this->m_pMakefile->AddCacheDefinition(internalRunOutputName,
                                         "PLEASE_FILL_OUT-NOTFOUND", comment,
                                         cmStateEnums::STRING);
      cmState* state = this->m_pMakefile->GetState();
      cmValue existing = state->GetCacheEntryValue(internalRunOutputName);
      if (existing) {
        state->SetCacheEntryProperty(internalRunOutputName, "ADVANCED", "1");
      }

      error = true;
    }
  }

  if (error) {
    static bool firstTryRun = true;
    cmsys::ofstream file(resultFileName.c_str(),
                         firstTryRun ? std::ios::out : std::ios::app);
    if (file) {
      if (firstTryRun) {
        /* clang-format off */
        file << "# This file was generated by CMake because it detected "
                "try_run() commands\n"
                "# in crosscompiling mode. It will be overwritten by the next "
                "CMake run.\n"
                "# Copy it to a safe location, set the variables to "
                "appropriate values\n"
                "# and use it then to preset the CMake cache (using -C).\n\n";
        /* clang-format on */
      }

      std::string comment =
        cmStrCat('\n', this->RunResultVariable,
                 "\n   indicates whether the executable would have been able "
                 "to run on its\n"
                 "   target platform. If so, set ",
                 this->RunResultVariable,
                 " to\n"
                 "   the exit code (in many cases 0 for success), otherwise "
                 "enter \"FAILED_TO_RUN\".\n");
      if (stdOut || stdErr) {
        if (stdOut) {
          comment += cmStrCat(
            internalRunOutputStdOutName,
            "\n   contains the text the executable would have printed on "
            "stdout.\n"
            "   If the executable would not have been able to run, set ",
            internalRunOutputStdOutName,
            " empty.\n"
            "   Otherwise check if the output is evaluated by the "
            "calling CMake code. If so,\n"
            "   check what the source file would have printed when "
            "called with the given arguments.\n");
        }
        if (stdErr) {
          comment += cmStrCat(
            internalRunOutputStdErrName,
            "\n   contains the text the executable would have printed on "
            "stderr.\n"
            "   If the executable would not have been able to run, set ",
            internalRunOutputStdErrName,
            " empty.\n"
            "   Otherwise check if the output is evaluated by the "
            "calling CMake code. If so,\n"
            "   check what the source file would have printed when "
            "called with the given arguments.\n");
        }
      } else if (out) {
        comment += cmStrCat(
          internalRunOutputName,
          "\n   contains the text the executable would have printed on stdout "
          "and stderr.\n"
          "   If the executable would not have been able to run, set ",
          internalRunOutputName,
          " empty.\n"
          "   Otherwise check if the output is evaluated by the "
          "calling CMake code. If so,\n"
          "   check what the source file would have printed when "
          "called with the given arguments.\n");
      }

      comment +=
        cmStrCat("The ", compileResultVariable,
                 " variable holds the build result for this try_run().\n\n");
      if (srcFile) {
        comment += cmStrCat("Source file   : ", *srcFile, '\n');
      }
      comment += cmStrCat("Executable    : ", copyDest,
                          "\n"
                          "Run arguments : ",
                          runArgs,
                          "\n"
                          "   Called from: ",
                          this->m_pMakefile->FormatListFileStack());
      cmsys::SystemTools::ReplaceString(comment, "\n", "\n# ");
      file << comment << "\n\n";

      file << "set( " << this->RunResultVariable << " \n     \""
           << this->m_pMakefile->GetSafeDefinition(this->RunResultVariable)
           << "\"\n     CACHE STRING \"Result from try_run\" FORCE)\n\n";

      if (out) {
        file << "set( " << internalRunOutputName << " \n     \""
             << this->m_pMakefile->GetSafeDefinition(internalRunOutputName)
             << "\"\n     CACHE STRING \"Output from try_run\" FORCE)\n\n";
      }
      file.close();
    }
    firstTryRun = false;

    std::string errorMessage =
      cmStrCat("try_run() invoked in cross-compiling mode, "
               "please set the following cache variables "
               "appropriately:\n   ",
               this->RunResultVariable, " (advanced)\n");
    if (out) {
      errorMessage += "   " + internalRunOutputName + " (advanced)\n";
    }
    errorMessage += detailsString;
    cmSystemTools::Error(errorMessage);
    return;
  }

  if (stdOut || stdErr) {
    if (stdOut) {
      (*stdOut) = *this->m_pMakefile->GetDefinition(internalRunOutputStdOutName);
    }
    if (stdErr) {
      (*stdErr) = *this->m_pMakefile->GetDefinition(internalRunOutputStdErrName);
    }
  } else if (out) {
    (*out) = *this->m_pMakefile->GetDefinition(internalRunOutputName);
  }
}
}

bool cmTryRunCommand(std::vector<std::string> const& args,
                     cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();

  if (args.size() < 4) {
    mf.IssueMessage(MessageType::FATAL_ERROR,
                    "The try_run() command requires at least 4 arguments.");
    return false;
  }

  if (mf.GetCMakeInstance()->GetWorkingMode() == CMake::FIND_PACKAGE_MODE) {
    mf.IssueMessage(
      MessageType::FATAL_ERROR,
      "The try_run() command is not supported in --find-package mode.");
    return false;
  }

  TryRunCommandImpl tr(&mf);
  return tr.TryRunCode(args);
}
