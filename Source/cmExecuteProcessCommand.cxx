/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmExecuteProcessCommand.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <cm/optional>
#include <cm/string_view>
#include <cmext/algorithm>
#include <cmext/string_view>

#include <cm3p/uv.h>

#ifndef _WIN32
#  include <fcntl.h>

#  include "cm_fileno.hxx"
#endif

#include "cmArgumentParser.h"
#include "cmExecutionStatus.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmProcessOutput.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmUVHandlePtr.h"
#include "cmUVProcessChain.h"
#include "cmUVStream.h"

namespace {
bool cmExecuteProcessCommandIsWhitespace(char c)
{
  return (cmIsSpace(c) || c == '\n' || c == '\r');
}

FILE* FopenCLOEXEC(std::string const& path, char const* mode)
{
  FILE* f = cmsys::SystemTools::Fopen(path, mode);
#ifndef _WIN32
  if (f) {
    if (fcntl(cm_fileno(f), F_SETFD, FD_CLOEXEC) < 0) {
      fclose(f);
      f = nullptr;
    }
  }
#endif
  return f;
}

void cmExecuteProcessCommandFixText(std::vector<char>& output,
                                    bool strip_trailing_whitespace);
void cmExecuteProcessCommandAppend(std::vector<char>& output, char const* data,
                                   std::size_t length);
}

// cmExecuteProcessCommand
bool cmExecuteProcessCommand(std::vector<std::string> const& args,
                             cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("called with incorrect number of arguments");
    return false;
  }

  struct Arguments : public ArgumentParser::ParseResult
  {
    std::vector<std::vector<std::string>> m_commands;
    std::string OutputVariable;
    std::string ErrorVariable;
    std::string ResultVariable;
    std::string ResultsVariable;
    std::string WorkingDirectory;
    std::string InputFile;
    std::string OutputFile;
    std::string ErrorFile;
    std::string Timeout;
    std::string CommandEcho;
    bool OutputQuiet = false;
    bool ErrorQuiet = false;
    bool OutputStripTrailingWhitespace = false;
    bool ErrorStripTrailingWhitespace = false;
    bool EchoOutputVariable = false;
    bool EchoErrorVariable = false;
    cm::optional<std::string> Encoding;
    std::string CommandErrorIsFatal;
  };

  static auto const parser =
    cmArgumentParser<Arguments>{}
      .Bind("COMMAND"_s, &Arguments::m_commands)
      .Bind("COMMAND_ECHO"_s, &Arguments::CommandEcho)
      .Bind("OUTPUT_VARIABLE"_s, &Arguments::OutputVariable)
      .Bind("ERROR_VARIABLE"_s, &Arguments::ErrorVariable)
      .Bind("RESULT_VARIABLE"_s, &Arguments::ResultVariable)
      .Bind("RESULTS_VARIABLE"_s, &Arguments::ResultsVariable)
      .Bind("WORKING_DIRECTORY"_s, &Arguments::WorkingDirectory)
      .Bind("INPUT_FILE"_s, &Arguments::InputFile)
      .Bind("OUTPUT_FILE"_s, &Arguments::OutputFile)
      .Bind("ERROR_FILE"_s, &Arguments::ErrorFile)
      .Bind("TIMEOUT"_s, &Arguments::Timeout)
      .Bind("OUTPUT_QUIET"_s, &Arguments::OutputQuiet)
      .Bind("ERROR_QUIET"_s, &Arguments::ErrorQuiet)
      .Bind("OUTPUT_STRIP_TRAILING_WHITESPACE"_s,
            &Arguments::OutputStripTrailingWhitespace)
      .Bind("ERROR_STRIP_TRAILING_WHITESPACE"_s,
            &Arguments::ErrorStripTrailingWhitespace)
      .Bind("ENCODING"_s, &Arguments::Encoding)
      .Bind("ECHO_OUTPUT_VARIABLE"_s, &Arguments::EchoOutputVariable)
      .Bind("ECHO_ERROR_VARIABLE"_s, &Arguments::EchoErrorVariable)
      .Bind("COMMAND_ERROR_IS_FATAL"_s, &Arguments::CommandErrorIsFatal);

  std::vector<std::string> unparsedArguments;
  Arguments arguments = parser.Parse(args, &unparsedArguments);

  if (arguments.MaybeReportError(status.GetMakefile())) {
    return true;
  }
  if (!unparsedArguments.empty()) {
    status.SetError(" given unknown argument \"" + unparsedArguments.front() +
                    "\".");
    return false;
  }

  std::string inputFilename = arguments.InputFile;
  std::string outputFilename = arguments.OutputFile;
  std::string errorFilename = arguments.ErrorFile;
  if (!arguments.WorkingDirectory.empty()) {
    if (!inputFilename.empty()) {
      inputFilename = cmSystemTools::CollapseFullPath(
        inputFilename, arguments.WorkingDirectory);
    }
    if (!outputFilename.empty()) {
      outputFilename = cmSystemTools::CollapseFullPath(
        outputFilename, arguments.WorkingDirectory);
    }
    if (!errorFilename.empty()) {
      errorFilename = cmSystemTools::CollapseFullPath(
        errorFilename, arguments.WorkingDirectory);
    }
  }

  if (!status.GetMakefile().CanIWriteThisFile(outputFilename)) {
    status.SetError("attempted to output into a file: " + outputFilename +
                    " into a source directory.");
    cmSystemTools::SetFatalErrorOccurred();
    return false;
  }

  // Check for commands given.
  if (arguments.m_commands.empty()) {
    status.SetError(" called with no COMMAND argument.");
    return false;
  }
  for (std::vector<std::string>& cmd : arguments.m_commands) {
    if (cmd.empty()) {
      status.SetError(" given COMMAND argument with no value.");
      return false;
    }
    cmSystemTools::MaybePrependCmdExe(cmd);
  }

  // Parse the timeout string.
  double timeout = -1;
  if (!arguments.Timeout.empty()) {
    if (sscanf(arguments.Timeout.c_str(), "%lg", &timeout) != 1) {
      status.SetError(" called with TIMEOUT value that could not be parsed.");
      return false;
    }
  }

  std::string commandErrorIsFatal = arguments.CommandErrorIsFatal;
  if (commandErrorIsFatal.empty() && arguments.ResultVariable.empty() &&
      arguments.ResultsVariable.empty()) {
    commandErrorIsFatal = status.GetMakefile().GetSafeDefinition(
      "CMAKE_EXECUTE_PROCESS_COMMAND_ERROR_IS_FATAL");
  }

  if (!commandErrorIsFatal.empty() && commandErrorIsFatal != "ANY"_s &&
      commandErrorIsFatal != "LAST"_s && commandErrorIsFatal != "NONE"_s) {
    if (!arguments.CommandErrorIsFatal.empty()) {
      status.SetError(
        "COMMAND_ERROR_IS_FATAL option can be ANY, LAST or NONE");
      return false;
    }
    status.SetError(cmStrCat(
      "Using CMAKE_EXECUTE_PROCESS_COMMAND_ERROR_IS_FATAL with invalid value "
      "\"",
      commandErrorIsFatal, "\". This variable can be ANY, LAST or NONE"));
    return false;
  }
  // Create a process instance.
  cmUVProcessChainBuilder builder;

  // Set the command sequence.
  for (std::vector<std::string> const& cmd : arguments.m_commands) {
    builder.AddCommand(cmd);
  }

  // Set the process working directory.
  if (!arguments.WorkingDirectory.empty()) {
    builder.SetWorkingDirectory(arguments.WorkingDirectory);
  }

  // Check the output variables.
  std::unique_ptr<FILE, int (*)(FILE*)> inputFile(nullptr, fclose);
  if (!inputFilename.empty()) {
    inputFile.reset(FopenCLOEXEC(inputFilename, "rb"));
    if (inputFile) {
      builder.SetExternalStream(cmUVProcessChainBuilder::Stream_INPUT,
                                inputFile.get());
    }
  } else {
    builder.SetExternalStream(cmUVProcessChainBuilder::Stream_INPUT, stdin);
  }

  std::unique_ptr<FILE, int (*)(FILE*)> outputFile(nullptr, fclose);
  if (!outputFilename.empty()) {
    outputFile.reset(FopenCLOEXEC(outputFilename, "wb"));
    if (outputFile) {
      builder.SetExternalStream(cmUVProcessChainBuilder::Stream_OUTPUT,
                                outputFile.get());
    }
  } else {
    if (arguments.OutputVariable == arguments.ErrorVariable &&
        !arguments.ErrorVariable.empty()) {
      builder.SetMergedBuiltinStreams();
    } else {
      builder.SetBuiltinStream(cmUVProcessChainBuilder::Stream_OUTPUT);
    }
  }

  std::unique_ptr<FILE, int (*)(FILE*)> errorFile(nullptr, fclose);
  if (!errorFilename.empty()) {
    if (errorFilename == outputFilename) {
      if (outputFile) {
        builder.SetExternalStream(cmUVProcessChainBuilder::Stream_ERROR,
                                  outputFile.get());
      }
    } else {
      errorFile.reset(FopenCLOEXEC(errorFilename, "wb"));
      if (errorFile) {
        builder.SetExternalStream(cmUVProcessChainBuilder::Stream_ERROR,
                                  errorFile.get());
      }
    }
  } else if (arguments.ErrorVariable.empty() ||
             (!arguments.ErrorVariable.empty() &&
              arguments.OutputVariable != arguments.ErrorVariable)) {
    builder.SetBuiltinStream(cmUVProcessChainBuilder::Stream_ERROR);
  }

  // Set the timeout if any.
  int64_t timeoutMillis = static_cast<int64_t>(timeout * 1000.0);

  bool echo_stdout = false;
  bool echo_stderr = false;
  bool echo_output_from_variable = true;
  std::string echo_output = status.GetMakefile().GetSafeDefinition(
    "CMAKE_EXECUTE_PROCESS_COMMAND_ECHO");
  if (!arguments.CommandEcho.empty()) {
    echo_output_from_variable = false;
    echo_output = arguments.CommandEcho;
  }

  if (!echo_output.empty()) {
    if (echo_output == "STDERR") {
      echo_stderr = true;
    } else if (echo_output == "STDOUT") {
      echo_stdout = true;
    } else if (echo_output != "NONE") {
      std::string error;
      if (echo_output_from_variable) {
        error = "CMAKE_EXECUTE_PROCESS_COMMAND_ECHO set to '";
      } else {
        error = " called with '";
      }
      error += echo_output;
      error += "' expected STDERR|STDOUT|NONE";
      if (!echo_output_from_variable) {
        error += " for COMMAND_ECHO.";
      }
      status.GetMakefile().IssueMessage(MessageType::FATAL_ERROR, error);
      return true;
    }
  }
  if (echo_stdout || echo_stderr) {
    std::string command;
    for (auto const& cmd : arguments.m_commands) {
      command += "'";
      command += cmJoin(cmd, "' '");
      command += "'";
      command += "\n";
    }
    if (echo_stdout) {
      std::cout << command;
    } else if (echo_stderr) {
      std::cerr << command;
    }
  }
  // Start the process.
  auto chain = builder.Start();

  bool timedOut = false;
  cm::uv_timer_ptr timer;

  if (timeoutMillis >= 0) {
    timer.init(chain.GetLoop(), &timedOut);
    timer.start(
      [](uv_timer_t* handle) {
        auto* timeoutPtr = static_cast<bool*>(handle->data);
        *timeoutPtr = true;
      },
      timeoutMillis, 0);
  }

  // Read the process output.
  struct ReadData
  {
    bool Finished = false;
    std::vector<char> Output;
    cm::uv_pipe_ptr Stream;
  };
  ReadData outputData;
  ReadData errorData;
  cmPolicies::PolicyStatus const cmp0176 =
    status.GetMakefile().GetPolicyStatus(cmPolicies::CMP0176);
  cmProcessOutput::Encoding encoding =
    cmp0176 == cmPolicies::OLD || cmp0176 == cmPolicies::WARN
    ? cmProcessOutput::Auto
    : cmProcessOutput::UTF8;
  if (arguments.Encoding) {
    if (cm::optional<cmProcessOutput::Encoding> maybeEncoding =
          cmProcessOutput::FindEncoding(*arguments.Encoding)) {
      encoding = *maybeEncoding;
    } else {
      status.GetMakefile().IssueMessage(
        MessageType::AUTHOR_WARNING,
        cmStrCat("ENCODING option given unknown value \"", *arguments.Encoding,
                 "\".  Ignoring."));
    }
  }
  cmProcessOutput processOutput(encoding);
  std::string strdata;

  std::unique_ptr<cmUVStreamReadHandle> outputHandle;
  if (chain.OutputStream() >= 0) {
    outputData.Stream.init(chain.GetLoop(), 0);
    uv_pipe_open(outputData.Stream, chain.OutputStream());
    outputHandle = cmUVStreamRead(
      outputData.Stream,
      [&arguments, &processOutput, &outputData,
       &strdata](std::vector<char> data) {
        if (!arguments.OutputQuiet) {
          if (arguments.OutputVariable.empty() ||
              arguments.EchoOutputVariable) {
            processOutput.DecodeText(data.data(), data.size(), strdata, 1);
            cmSystemTools::Stdout(strdata);
          }
          if (!arguments.OutputVariable.empty()) {
            cmExecuteProcessCommandAppend(outputData.Output, data.data(),
                                          data.size());
          }
        }
      },
      [&outputData]() { outputData.Finished = true; });
  } else {
    outputData.Finished = true;
  }
  std::unique_ptr<cmUVStreamReadHandle> errorHandle;
  if (chain.ErrorStream() >= 0 &&
      chain.ErrorStream() != chain.OutputStream()) {
    errorData.Stream.init(chain.GetLoop(), 0);
    uv_pipe_open(errorData.Stream, chain.ErrorStream());
    errorHandle = cmUVStreamRead(
      errorData.Stream,
      [&arguments, &processOutput, &errorData,
       &strdata](std::vector<char> data) {
        if (!arguments.ErrorQuiet) {
          if (arguments.ErrorVariable.empty() || arguments.EchoErrorVariable) {
            processOutput.DecodeText(data.data(), data.size(), strdata, 2);
            cmSystemTools::Stderr(strdata);
          }
          if (!arguments.ErrorVariable.empty()) {
            cmExecuteProcessCommandAppend(errorData.Output, data.data(),
                                          data.size());
          }
        }
      },
      [&errorData]() { errorData.Finished = true; });
  } else {
    errorData.Finished = true;
  }

  while (chain.Valid() && !timedOut &&
         !(chain.Finished() && outputData.Finished && errorData.Finished)) {
    uv_run(&chain.GetLoop(), UV_RUN_ONCE);
  }
  if (!arguments.OutputQuiet &&
      (arguments.OutputVariable.empty() || arguments.EchoOutputVariable)) {
    processOutput.DecodeText(std::string(), strdata, 1);
    if (!strdata.empty()) {
      cmSystemTools::Stdout(strdata);
    }
  }
  if (!arguments.ErrorQuiet &&
      (arguments.ErrorVariable.empty() || arguments.EchoErrorVariable)) {
    processOutput.DecodeText(std::string(), strdata, 2);
    if (!strdata.empty()) {
      cmSystemTools::Stderr(strdata);
    }
  }

  // All output has been read.
  processOutput.DecodeText(outputData.Output, outputData.Output);
  processOutput.DecodeText(errorData.Output, errorData.Output);

  // Fix the text in the output strings.
  cmExecuteProcessCommandFixText(outputData.Output,
                                 arguments.OutputStripTrailingWhitespace);
  cmExecuteProcessCommandFixText(errorData.Output,
                                 arguments.ErrorStripTrailingWhitespace);

  // Store the output obtained.
  if (!arguments.OutputVariable.empty() && !outputData.Output.empty()) {
    status.GetMakefile().AddDefinition(arguments.OutputVariable,
                                       outputData.Output.data());
  }
  if (arguments.ErrorVariable != arguments.OutputVariable &&
      !arguments.ErrorVariable.empty() && !errorData.Output.empty()) {
    status.GetMakefile().AddDefinition(arguments.ErrorVariable,
                                       errorData.Output.data());
  }

  // Store the result of running the process.
  if (!arguments.ResultVariable.empty()) {
    if (timedOut) {
      status.GetMakefile().AddDefinition(arguments.ResultVariable,
                                         "Process terminated due to timeout");
    } else {
      auto const* lastStatus = chain.GetStatus().back();
      auto exception = lastStatus->GetException();
      if (exception.first == cmUVProcessChain::ExceptionCode::None) {
        status.GetMakefile().AddDefinition(
          arguments.ResultVariable,
          std::to_string(static_cast<int>(lastStatus->ExitStatus)));
      } else {
        status.GetMakefile().AddDefinition(arguments.ResultVariable,
                                           exception.second);
      }
    }
  }
  // Store the result of running the processes.
  if (!arguments.ResultsVariable.empty()) {
    if (timedOut) {
      status.GetMakefile().AddDefinition(arguments.ResultsVariable,
                                         "Process terminated due to timeout");
    } else {
      std::vector<std::string> res;
      for (auto const* processStatus : chain.GetStatus()) {
        auto exception = processStatus->GetException();
        if (exception.first == cmUVProcessChain::ExceptionCode::None) {
          res.emplace_back(
            std::to_string(static_cast<int>(processStatus->ExitStatus)));
        } else {
          res.emplace_back(exception.second);
        }
      }
      status.GetMakefile().AddDefinition(arguments.ResultsVariable,
                                         cmList::to_string(res));
    }
  }

  auto queryProcessStatusByIndex = [&chain](std::size_t index) -> std::string {
    auto const& processStatus = chain.GetStatus(index);
    auto exception = processStatus.GetException();
    if (exception.first == cmUVProcessChain::ExceptionCode::None) {
      if (processStatus.ExitStatus) {
        return cmStrCat("Child return code: ", processStatus.ExitStatus);
      }
      return "";
    }
    return cmStrCat("Abnormal exit with child return code: ",
                    exception.second);
  };

  if (commandErrorIsFatal == "ANY"_s) {
    bool ret = true;
    if (timedOut) {
      status.SetError("Process terminated due to timeout");
      ret = false;
    } else {
      std::map<std::size_t, std::string> failureIndices;
      auto statuses = chain.GetStatus();
      for (std::size_t i = 0; i < statuses.size(); ++i) {
        std::string processStatus = queryProcessStatusByIndex(i);
        if (!processStatus.empty()) {
          failureIndices[i] = processStatus;
        }
      }
      if (!failureIndices.empty()) {
        std::ostringstream oss;
        oss << "failed command indexes:\n";
        for (auto const& e : failureIndices) {
          oss << "  " << e.first + 1 << ": \"" << e.second << "\"\n";
        }
        status.SetError(oss.str());
        ret = false;
      }
    }

    if (!ret) {
      cmSystemTools::SetFatalErrorOccurred();
      return false;
    }
  }

  if (commandErrorIsFatal == "LAST"_s) {
    bool ret = true;
    if (timedOut) {
      status.SetError("Process terminated due to timeout");
      ret = false;
    } else {
      auto const& lastStatus = chain.GetStatus(arguments.m_commands.size() - 1);
      auto exception = lastStatus.GetException();
      if (exception.first != cmUVProcessChain::ExceptionCode::None) {
        status.SetError(cmStrCat("Abnormal exit: ", exception.second));
        ret = false;
      } else {
        int lastIndex = static_cast<int>(arguments.m_commands.size() - 1);
        std::string const processStatus = queryProcessStatusByIndex(lastIndex);
        if (!processStatus.empty()) {
          status.SetError("last command failed");
          ret = false;
        }
      }
    }
    if (!ret) {
      cmSystemTools::SetFatalErrorOccurred();
      return false;
    }
  }

  return true;
}

namespace {
void cmExecuteProcessCommandFixText(std::vector<char>& output,
                                    bool strip_trailing_whitespace)
{
  // Remove \0 characters and the \r part of \r\n pairs.
  unsigned int in_index = 0;
  unsigned int out_index = 0;
  while (in_index < output.size()) {
    char c = output[in_index++];
    if ((c != '\r' ||
         !(in_index < output.size() && output[in_index] == '\n')) &&
        c != '\0') {
      output[out_index++] = c;
    }
  }

  // Remove trailing whitespace if requested.
  if (strip_trailing_whitespace) {
    while (out_index > 0 &&
           cmExecuteProcessCommandIsWhitespace(output[out_index - 1])) {
      --out_index;
    }
  }

  // Shrink the vector to the size needed.
  output.resize(out_index);

  // Put a terminator on the text string.
  output.push_back('\0');
}

void cmExecuteProcessCommandAppend(std::vector<char>& output, char const* data,
                                   std::size_t length)
{
#if defined(__APPLE__)
  // HACK on Apple to work around bug with inserting at the
  // end of an empty vector.  This resulted in random failures
  // that were hard to reproduce.
  if (output.empty() && length > 0) {
    output.push_back(data[0]);
    ++data;
    --length;
  }
#endif
  cm::append(output, data, data + length);
}
}
