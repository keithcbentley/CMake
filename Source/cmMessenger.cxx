/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmMessenger.h"

#include "cmDocumentationFormatter.h"
#include "cmMessageMetadata.h"
#include "cmMessageType.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"

#if !defined(CMAKE_BOOTSTRAP)
#  include "cmsys/SystemInformation.hxx"

#  include "cmSarifLog.h"
#endif

#include <sstream>
#include <utility>

#include "cmsys/Terminal.h"

#ifdef CMake_ENABLE_DEBUGGER
#  include "cmDebuggerAdapter.h"
#endif

namespace {
char const* getMessageTypeStr(MessageType t)
{
  switch (t) {
    case MessageType::FATAL_ERROR:
      return "Error";
    case MessageType::INTERNAL_ERROR:
      return "Internal Error (please report a bug)";
    case MessageType::LOG:
      return "Debug Log";
    case MessageType::DEPRECATION_ERROR:
      return "Deprecation Error";
    case MessageType::DEPRECATION_WARNING:
      return "Deprecation Warning";
    case MessageType::AUTHOR_WARNING:
      return "Warning (dev)";
    case MessageType::AUTHOR_ERROR:
      return "Error (dev)";
    default:
      break;
  }
  return "Warning";
}

int getMessageColor(MessageType t)
{
  switch (t) {
    case MessageType::INTERNAL_ERROR:
    case MessageType::FATAL_ERROR:
    case MessageType::AUTHOR_ERROR:
      return cmsysTerminal_Color_ForegroundRed;
    case MessageType::AUTHOR_WARNING:
    case MessageType::WARNING:
      return cmsysTerminal_Color_ForegroundYellow;
    default:
      return cmsysTerminal_Color_Normal;
  }
}

//void printMessageText(std::ostream& msg, std::string const& text)
//{
//  msg << ":\n";
//  cmDocumentationFormatter formatter;
//  formatter.SetIndent(2u);
//  formatter.PrintFormatted(msg, text);
//}

void displayMessage(MessageType t, std::ostringstream& msg)
{
  // Add a note about warning suppression.
  if (t == MessageType::AUTHOR_WARNING) {
    msg << "This warning is for project developers.  Use -Wno-dev to suppress "
           "it.";
  } else if (t == MessageType::AUTHOR_ERROR) {
    msg << "This error is for project developers. Use -Wno-error=dev to "
           "suppress it.";
  }

  // Add a terminating blank line.
  msg << '\n';

#if !defined(CMAKE_BOOTSTRAP)
  // Add a C++ stack trace to internal errors.
  if (t == MessageType::INTERNAL_ERROR) {
    std::string stack = cmsys::SystemInformation::GetProgramStack(0, 0);
    if (!stack.empty()) {
      if (cmHasLiteralPrefix(stack, "WARNING:")) {
        stack = "Note:" + stack.substr(8);
      }
      msg << stack << '\n';
    }
  }
#endif

  // Output the message.
  cmMessageMetadata md;
  md.desiredColor = getMessageColor(t);
  if (t == MessageType::FATAL_ERROR || t == MessageType::INTERNAL_ERROR ||
      t == MessageType::DEPRECATION_ERROR || t == MessageType::AUTHOR_ERROR) {
    cmSystemTools::SetErrorOccurred();
    md.title = "Error";
  } else {
    md.title = "Warning";
  }
  cmSystemTools::Message(msg.str(), md);
}

void PrintCallStack(std::ostream& out, cmListFileBacktrace bt,
                    cm::optional<std::string> const& topSource)
{
  // The call stack exists only if we have at least two calls on top
  // of the bottom.
  if (bt.Empty()) {
    return;
  }
  std::string lastFilePath = bt.Top().m_filePath;
  bt = bt.Pop();
  if (bt.Empty()) {
    return;
  }

  bool first = true;
  for (; !bt.Empty(); bt = bt.Pop()) {
    cmListFileContext lfc = bt.Top();
    if (lfc.m_name.empty() &&
        lfc.Line != cmListFileContext::DeferPlaceholderLine &&
        lfc.m_filePath == lastFilePath) {
      // An entry with no function name is frequently preceded (in the stack)
      // by a more specific entry.  When this happens (as verified by the
      // preceding entry referencing the same file path), skip the less
      // specific entry, as we have already printed the more specific one.
      continue;
    }
    if (first) {
      first = false;
      out << "Call Stack (most recent call first):\n";
    }
    lastFilePath = lfc.m_filePath;
    if (topSource) {
      lfc.m_filePath = cmSystemTools::RelativeIfUnder(*topSource, lfc.m_filePath);
    }
    out << "  " << lfc << '\n';
  }
}

} // anonymous namespace

MessageType cmMessenger::ConvertMessageType(MessageType t) const
{
  if (t == MessageType::AUTHOR_WARNING || t == MessageType::AUTHOR_ERROR) {
    if (this->GetDevWarningsAsErrors()) {
      return MessageType::AUTHOR_ERROR;
    }
    return MessageType::AUTHOR_WARNING;
  }
  if (t == MessageType::DEPRECATION_WARNING ||
      t == MessageType::DEPRECATION_ERROR) {
    if (this->GetDeprecatedWarningsAsErrors()) {
      return MessageType::DEPRECATION_ERROR;
    }
    return MessageType::DEPRECATION_WARNING;
  }
  return t;
}

bool cmMessenger::IsMessageTypeVisible(MessageType t) const
{
  if (t == MessageType::DEPRECATION_ERROR) {
    return this->GetDeprecatedWarningsAsErrors();
  }
  if (t == MessageType::DEPRECATION_WARNING) {
    return !this->GetSuppressDeprecatedWarnings();
  }
  if (t == MessageType::AUTHOR_ERROR) {
    return this->GetDevWarningsAsErrors();
  }
  if (t == MessageType::AUTHOR_WARNING) {
    return !this->GetSuppressDevWarnings();
  }

  return true;
}

void cmMessenger::IssueMessage(MessageType t, std::string const& text,
                               cmListFileBacktrace const& backtrace) const
{
  bool force = false;
  // override the message type, if needed, for warnings and errors
  MessageType override = this->ConvertMessageType(t);
  if (override != t) {
    t = override;
    force = true;
  }

  if (force || this->IsMessageTypeVisible(t)) {
    this->DisplayMessage(t, text, backtrace);
  }
}

void cmMessenger::DisplayMessage(MessageType t, std::string const& text,
                                 cmListFileBacktrace const& backtrace) const
{
  std::ostringstream msg;

  // Print the message preamble.
  msg << "CMake " << getMessageTypeStr(t);

  // Add the immediate context.
  this->PrintBacktraceTitle(msg, backtrace);

//  printMessageText(msg, text);

  // Add the rest of the context.
  PrintCallStack(msg, backtrace, this->TopSource);

  displayMessage(t, msg);

#ifndef CMAKE_BOOTSTRAP
  // Add message to SARIF logs
  this->SarifLog.LogMessage(t, text, backtrace);
#endif

#ifdef CMake_ENABLE_DEBUGGER
  if (DebuggerAdapter) {
    DebuggerAdapter->OnMessageOutput(t, msg.str());
  }
#endif
}

void cmMessenger::PrintBacktraceTitle(std::ostream& out,
                                      cmListFileBacktrace const& bt) const
{
  // The title exists only if we have a call on top of the bottom.
  if (bt.Empty()) {
    return;
  }
  cmListFileContext lfc = bt.Top();
  if (this->TopSource) {
    lfc.m_filePath =
      cmSystemTools::RelativeIfUnder(*this->TopSource, lfc.m_filePath);
  }
  out << (lfc.Line ? " at " : " in ") << lfc;
}

void cmMessenger::SetTopSource(cm::optional<std::string> topSource)
{
  this->TopSource = std::move(topSource);
}
