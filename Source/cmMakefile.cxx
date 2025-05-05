/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmConfigure.h" // IWYU pragma: keep

#include "cmMakefile.h"
#include "cmakeMessage.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>

#include <cm/iterator>
#include <cm/memory>
#include <cm/optional>
#include <cm/type_traits> // IWYU pragma: keep
#include <cm/vector>
#include <cmext/algorithm>
#include <cmext/string_view>

#ifndef CMAKE_BOOTSTRAP
#  include <cm3p/json/value.h>
#  include <cm3p/json/writer.h>
#endif

#include "cmsys/FStream.hxx"
#include "cmsys/RegularExpression.hxx"

#include "cmCustomCommand.h"
#include "cmCustomCommandLines.h"
#include "cmCustomCommandTypes.h"
#include "cmExecutionStatus.h"
#include "cmExpandedCommandArgument.h" // IWYU pragma: keep
#include "cmExportBuildFileGenerator.h"
#include "cmFileLockPool.h"
#include "cmFunctionBlocker.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorExpressionEvaluationFile.h"
#include "cmGlobalGenerator.h"
#include "cmInstallGenerator.h" // IWYU pragma: keep
#include "cmInstallSubdirectoryGenerator.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmMessageType.h"
#include "cmRange.h"
#include "cmSourceFile.h"
#include "cmSourceFileLocation.h"
#include "cmState.h"
#include "cmStateDirectory.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTargetLinkLibraryType.h"
#include "cmTest.h"
#include "cmTestGenerator.h" // IWYU pragma: keep
#include "cmVersion.h"
#include "cmWorkingDirectory.h"
#include "cmake.h"

#ifndef CMAKE_BOOTSTRAP
#  include "cmMakefileProfilingData.h"
#  include "cmVariableWatch.h"
#endif

#ifdef CMake_ENABLE_DEBUGGER
#  include "cmDebuggerAdapter.h"
#endif

#ifndef __has_feature
#  define __has_feature(x) 0
#endif

// Select a recursion limit that fits within the stack size.
// See stack size flags in '../CompileFlags.cmake'.
#ifndef CMake_DEFAULT_RECURSION_LIMIT
#  if __has_feature(address_sanitizer)
#    define CMake_DEFAULT_RECURSION_LIMIT 400
#  elif defined(_MSC_VER) && defined(_DEBUG)
#    define CMake_DEFAULT_RECURSION_LIMIT 600
#  elif defined(__ibmxl__) && defined(__linux)
#    define CMake_DEFAULT_RECURSION_LIMIT 600
#  else
#    define CMake_DEFAULT_RECURSION_LIMIT 1000
#  endif
#endif

namespace {
std::string const kCMAKE_CURRENT_LIST_DIR = "CMAKE_CURRENT_LIST_DIR";
std::string const kCMAKE_CURRENT_LIST_FILE = "CMAKE_CURRENT_LIST_FILE";
std::string const kCMAKE_PARENT_LIST_FILE = "CMAKE_PARENT_LIST_FILE";

class FileScopeBase
{
protected:
  cmMakefile* m_pMakefile;

private:
  std::string m_oldCurrent;
  cm::optional<std::string> m_oldParent;

public:
  FileScopeBase(cmMakefile* mf)
    : m_pMakefile(mf)
  {
  }

  void PushListFileVars(std::string const& newCurrent)
  {
    if (cmValue p = m_pMakefile->GetDefinition(kCMAKE_PARENT_LIST_FILE)) {
      m_oldParent = *p;
    }
    if (cmValue c = m_pMakefile->GetDefinition(kCMAKE_CURRENT_LIST_FILE)) {
      m_oldCurrent = *c;
      m_pMakefile->AddDefinition(kCMAKE_PARENT_LIST_FILE, m_oldCurrent);
      m_pMakefile->MarkVariableAsUsed(kCMAKE_PARENT_LIST_FILE);
    }
    m_pMakefile->AddDefinition(kCMAKE_CURRENT_LIST_FILE, newCurrent);
    m_pMakefile->AddDefinition(kCMAKE_CURRENT_LIST_DIR, cmSystemTools::GetFilenamePath(newCurrent));
    m_pMakefile->MarkVariableAsUsed(kCMAKE_CURRENT_LIST_FILE);
    m_pMakefile->MarkVariableAsUsed(kCMAKE_CURRENT_LIST_DIR);
  }

  void PopListFileVars()
  {
    if (m_oldParent) {
      m_pMakefile->AddDefinition(kCMAKE_PARENT_LIST_FILE, *m_oldParent);
      m_pMakefile->MarkVariableAsUsed(kCMAKE_PARENT_LIST_FILE);
    } else {
      m_pMakefile->RemoveDefinition(kCMAKE_PARENT_LIST_FILE);
    }
    m_pMakefile->AddDefinition(kCMAKE_CURRENT_LIST_FILE, m_oldCurrent);
    m_pMakefile->AddDefinition(kCMAKE_CURRENT_LIST_DIR, cmSystemTools::GetFilenamePath(m_oldCurrent));
    m_pMakefile->MarkVariableAsUsed(kCMAKE_CURRENT_LIST_FILE);
    m_pMakefile->MarkVariableAsUsed(kCMAKE_CURRENT_LIST_DIR);
  }
};
}

class cmMessenger;

cmDirectoryId::cmDirectoryId(std::string s)
  : String(std::move(s))
{
}

cmMakefile::cmMakefile(
  cmGlobalGenerator* globalGenerator,
  cmStateSnapshot const& snapshot)
  : m_pGlobalGenerator(globalGenerator)
  , m_stateSnapshot(snapshot)
{
  m_isSourceFileTryCompile = false;

  m_checkSystemVars = GetCMakeInstance()->GetCheckSystemVars();

  // Setup the default include complaint regular expression (match nothing).
  m_complainFileRegularExpression = "^$";

  m_defineFlags = " ";

  //    TODO:  Why are these member instead of static?
  m_cmDefineRegex.compile("#([ \t]*)cmakedefine[ \t]+([A-Za-z_0-9]*)");
  m_cmDefine01Regex.compile("#([ \t]*)cmakedefine01[ \t]+([A-Za-z_0-9]*)");
  m_cmNamedCurly.compile("^[A-Za-z0-9/_.+-]+{");

  m_stateSnapshot = m_stateSnapshot.GetState()->CreatePolicyScopeSnapshot(m_stateSnapshot);

  PushPolicy();

  PushLoopBlockBarrier();

  // By default the check is not done.  It is enabled by
  // cmMakefile::Configure in the top level if necessary.
  m_checkCMP0000 = false;

#if !defined(CMAKE_BOOTSTRAP)
  AddSourceGroup("", "^.*$");
  AddSourceGroup("Source Files", CM_SOURCE_REGEX);
  AddSourceGroup("Header Files", CM_HEADER_REGEX);
  AddSourceGroup("Precompile Header File", CM_PCH_REGEX);
  AddSourceGroup("CMake Rules", "\\.rule$");
  AddSourceGroup("Resources", CM_RESOURCE_REGEX);
  AddSourceGroup("Object Files", "\\.(lo|o|obj)$");

  m_objectLibrariesSourceGroupIndex = m_sourceGroups.size();
  m_sourceGroups.emplace_back("Object Libraries", "^MATCH_NO_SOURCES$");
#endif
}

cmMakefile::~cmMakefile() = default;

//  TODO: This whold directory id stuff is screwy.  Does it even do anything?
cmDirectoryId cmMakefile::GetDirectoryId() const
{
  // Use the instance pointer value to uniquely identify this directory.
  // If we ever need to expose this to CMake language code we should
  // add a read-only property in cmMakefile::GetProperty.
  char buf[32];
  snprintf(buf, sizeof(buf), "(%p)",
           static_cast<void const*>(this)); // cast avoids format warning
  return std::string(buf);
}

void cmMakefile::IssueMessage(
  MessageType t,
  std::string const& text) const
{
  if (!m_executionStatusStack.empty()) {
    if ((t == MessageType::FATAL_ERROR) || (t == MessageType::INTERNAL_ERROR)) {
      m_executionStatusStack.back()->SetNestedError();
    }
  }
  GetCMakeInstance()->IssueMessage(t, text, m_backtrace);
}

Message::LogLevel cmMakefile::GetCurrentLogLevel() const
{
  CMake const* cmakeInstance = GetCMakeInstance();

  Message::LogLevel const logLevelCliOrDefault = cmakeInstance->GetLogLevel();
  assert("Expected a valid log level here" && logLevelCliOrDefault != Message::LogLevel::LOG_UNDEFINED);

  Message::LogLevel result = logLevelCliOrDefault;

  // If the log-level was set via the command line option, it takes precedence
  // over the CMAKE_MESSAGE_LOG_LEVEL variable.
  if (!cmakeInstance->WasLogLevelSetViaCLI()) {
    Message::LogLevel const logLevelFromVar = CMake::StringToLogLevel(GetSafeDefinition("CMAKE_MESSAGE_LOG_LEVEL"));
    if (logLevelFromVar != Message::LogLevel::LOG_UNDEFINED) {
      result = logLevelFromVar;
    }
  }

  return result;
}

void cmMakefile::IssueInvalidTargetNameError(std::string const& targetName) const
{
  IssueMessage(
    MessageType::FATAL_ERROR,
    cmStrCat(
      "The target name \"", targetName,
      "\" is reserved or not valid for certain "
      "CMake features, such as generator expressions, and may result "
      "in undefined behavior."));
}

void cmMakefile::MaybeWarnCMP0074(
  std::string const& rootVar,
  cmValue rootDef,
  cm::optional<std::string> const& rootEnv)
{
  // Warn if a <PackageName>_ROOT variable we may use is set.
  if ((rootDef || rootEnv) && m_warnedCMP0074.insert(rootVar).second) {
    auto e = cmStrCat(cmPolicies::GetPolicyWarning(cmPolicies::CMP0074), '\n');
    if (rootDef) {
      e += cmStrCat("CMake variable ", rootVar, " is set to:\n  ", *rootDef, '\n');
    }
    if (rootEnv) {
      e += cmStrCat("Environment variable ", rootVar, " is set to:\n  ", *rootEnv, '\n');
    }
    e += "For compatibility, CMake is ignoring the variable.";
    IssueMessage(MessageType::AUTHOR_WARNING, e);
  }
}

void cmMakefile::MaybeWarnCMP0144(
  std::string const& rootVar,
  cmValue rootDef,
  cm::optional<std::string> const& rootEnv)
{
  // Warn if a <PACKAGENAME>_ROOT variable we may use is set.
  if ((rootDef || rootEnv) && m_warnedCMP0144.insert(rootVar).second) {
    auto e = cmStrCat(cmPolicies::GetPolicyWarning(cmPolicies::CMP0144), '\n');
    if (rootDef) {
      e += cmStrCat("CMake variable ", rootVar, " is set to:\n  ", *rootDef, '\n');
    }
    if (rootEnv) {
      e += cmStrCat("Environment variable ", rootVar, " is set to:\n  ", *rootEnv, '\n');
    }
    e += "For compatibility, find_package is ignoring the variable, but "
         "code in a .cmake module might still use it.";
    IssueMessage(MessageType::AUTHOR_WARNING, e);
  }
}

cmBTStringRange cmMakefile::GetIncludeDirectoriesEntries() const
{
  return m_stateSnapshot.GetDirectory().GetIncludeDirectoriesEntries();
}

cmBTStringRange cmMakefile::GetCompileOptionsEntries() const
{
  return m_stateSnapshot.GetDirectory().GetCompileOptionsEntries();
}

cmBTStringRange cmMakefile::GetCompileDefinitionsEntries() const
{
  return m_stateSnapshot.GetDirectory().GetCompileDefinitionsEntries();
}

cmBTStringRange cmMakefile::GetLinkOptionsEntries() const
{
  return m_stateSnapshot.GetDirectory().GetLinkOptionsEntries();
}

cmBTStringRange cmMakefile::GetLinkDirectoriesEntries() const
{
  return m_stateSnapshot.GetDirectory().GetLinkDirectoriesEntries();
}

cmListFileBacktrace cmMakefile::GetBacktrace() const
{
  return m_backtrace;
}

cmFindPackageStack cmMakefile::GetFindPackageStack() const
{
  return m_findPackageStack;
}

void cmMakefile::PrintCommandTrace(
  cmListFileFunction const& lff,
  cmListFileBacktrace const& bt,
  CommandMissingFromStack missing) const
{
  // Check if current file in the list of requested to trace...
  std::vector<std::string> const& trace_only_this_files = GetCMakeInstance()->GetTraceSources();
  std::string const& full_path = bt.Top().m_filePath;
  std::string const& only_filename = cmSystemTools::GetFilenameName(full_path);
  bool trace = trace_only_this_files.empty();
  if (!trace) {
    for (std::string const& file : trace_only_this_files) {
      std::string::size_type const pos = full_path.rfind(file);
      trace = (pos != std::string::npos) && ((pos + file.size()) == full_path.size()) &&
        (only_filename == cmSystemTools::GetFilenameName(file));
      if (trace) {
        break;
      }
    }
    // Do nothing if current file wasn't requested for trace...
    if (!trace) {
      return;
    }
  }

  std::vector<std::string> args;
  std::string temp;
  bool expand = GetCMakeInstance()->GetTraceExpand();

  args.reserve(lff.Arguments().size());
  for (cmListFileArgument const& arg : lff.Arguments()) {
    if (expand && arg.Delim != cmListFileArgument::Bracket) {
      temp = arg.Value;
      ExpandVariablesInString(temp);
      args.push_back(temp);
    } else {
      args.push_back(arg.Value);
    }
  }
  cm::optional<std::string> const& deferId = bt.Top().DeferId;

  std::ostringstream msg;
  switch (GetCMakeInstance()->GetTraceFormat()) {
    case CMake::TraceFormat::JSONv1: {
#ifndef CMAKE_BOOTSTRAP
      Json::Value val;
      Json::StreamWriterBuilder builder;
      builder["indentation"] = "";
      val["file"] = full_path;
      val["line"] = static_cast<Json::Value::Int64>(lff.Line());
      if (lff.Line() != lff.LineEnd()) {
        val["line_end"] = static_cast<Json::Value::Int64>(lff.LineEnd());
      }
      if (deferId) {
        val["defer"] = *deferId;
      }
      val["cmd"] = lff.OriginalName();
      val["args"] = Json::Value(Json::arrayValue);
      for (std::string const& arg : args) {
        val["args"].append(arg);
      }
      val["time"] = cmSystemTools::GetTime();
      val["frame"] =
        int(missing == CommandMissingFromStack::Yes) + static_cast<Json::Value::UInt64>(m_executionStatusStack.size());
      val["global_frame"] =
        int(missing == CommandMissingFromStack::Yes) + static_cast<Json::Value::UInt64>(m_recursionDepth);
      msg << Json::writeString(builder, val);
#endif
      break;
    }
    case CMake::TraceFormat::Human:
      msg << full_path << '(' << lff.Line() << "):";
      if (deferId) {
        msg << "DEFERRED:" << *deferId << ':';
      }
      msg << "  " << lff.OriginalName() << '(';

      for (std::string const& arg : args) {
        msg << arg << ' ';
      }
      msg << ')';
      break;
    case CMake::TraceFormat::Undefined:
      msg << "INTERNAL ERROR: Trace format is Undefined";
      break;
  }

  auto& f = GetCMakeInstance()->GetTraceFile();
  if (f) {
    f << msg.str() << '\n';
  } else {
    cmSystemTools::Message(msg.str());
  }
}

cmMakefile::CallRAII::CallRAII(
  cmMakefile* mf,
  std::string const& file,
  cmExecutionStatus& status)
  : CallRAII(
      mf,
      cmListFileContext::FromListFilePath(file),
      status)
{
}

cmMakefile::CallRAII::CallRAII(
  cmMakefile* mf,
  cmListFileContext const& lfc,
  cmExecutionStatus& status)
  : m_pMakefile{ mf }
{
  m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Push(lfc);
  ++m_pMakefile->m_recursionDepth;
  m_pMakefile->m_executionStatusStack.push_back(&status);
}

cmMakefile::CallRAII::~CallRAII()
{
  if (m_pMakefile) {
    Detach();
  }
}

cmMakefile* cmMakefile::CallRAII::Detach()
{
  assert(m_pMakefile);

  m_pMakefile->m_executionStatusStack.pop_back();
  --m_pMakefile->m_recursionDepth;
  m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Pop();

  auto* const mf = m_pMakefile;
  m_pMakefile = nullptr;
  return mf;
}

// Helper class to make sure the call stack is valid.
class cmMakefile::CallScope : public CallRAII
{
public:
  CallScope(
    cmMakefile* mf,
    cmListFileFunction const& lff,
    cm::optional<std::string> deferId,
    cmExecutionStatus& status)
    : CallScope{ mf,
                 lff,
                 cmListFileContext::FromListFileFunction(
                   lff,
                   mf->m_stateSnapshot.GetExecutionListFile(),
                   std::move(deferId)),
                 status }
  {
  }

  CallScope(
    cmMakefile* mf,
    cmListFileFunction const& lff,
    cmListFileContext const& lfc,
    cmExecutionStatus& status)
    : CallRAII{ mf,
                lfc,
                status }
  {
#if !defined(CMAKE_BOOTSTRAP)
    ProfilingDataRAII = m_pMakefile->GetCMakeInstance()->CreateProfilingEntry(
      "script", lff.LowerCaseName(), [&lff, &lfc]() -> Json::Value {
        Json::Value argsValue = Json::objectValue;
        if (!lff.Arguments().empty()) {
          std::string args;
          for (auto const& a : lff.Arguments()) {
            args = cmStrCat(args, args.empty() ? "" : " ", a.Value);
          }
          argsValue["functionArgs"] = args;
        }
        argsValue["location"] = cmStrCat(lfc.m_filePath, ':', lfc.Line);
        return argsValue;
      });
#endif
#ifdef CMake_ENABLE_DEBUGGER
    if (m_pMakefile->GetCMakeInstance()->GetDebugAdapter()) {
      m_pMakefile->GetCMakeInstance()->GetDebugAdapter()->OnBeginFunctionCall(mf, lfc.m_filePath, lff);
    }
#endif
  }

  ~CallScope()
  {
#if !defined(CMAKE_BOOTSTRAP)
    ProfilingDataRAII.reset();
#endif
    auto* const mf = Detach();
#ifdef CMake_ENABLE_DEBUGGER
    if (mf->GetCMakeInstance()->GetDebugAdapter()) {
      mf->GetCMakeInstance()->GetDebugAdapter()->OnEndFunctionCall();
    }
#else
    static_cast<void>(mf);
#endif
  }

  CallScope(CallScope const&) = delete;
  CallScope& operator=(CallScope const&) = delete;

private:
#if !defined(CMAKE_BOOTSTRAP)
  cm::optional<cmMakefileProfilingData::RAII> ProfilingDataRAII;
#endif
};

void cmMakefile::OnExecuteCommand(std::function<void()> callback)
{
  m_executeCommandCallback = std::move(callback);
}

bool cmMakefile::ExecuteCommand(
  cmListFileFunction const& lff,
  cmExecutionStatus& status,
  cm::optional<std::string> deferId)
{
  bool result = true;

  // quick return if blocked
  if (IsFunctionBlocked(lff, status)) {
    // No error.
    return result;
  }

  // Place this call on the call stack.
  CallScope stack_manager(this, lff, std::move(deferId), status);
  static_cast<void>(stack_manager);

  // Check for maximum recursion depth.
  size_t depthLimit = GetRecursionDepthLimit();
  if (m_recursionDepth > depthLimit) {
    IssueMessage(MessageType::FATAL_ERROR, cmStrCat("Maximum recursion depth of ", depthLimit, " exceeded"));
    cmSystemTools::SetFatalErrorOccurred();
    return false;
  }

  // Lookup the command prototype.
  if (cmState::m_command command = GetState()->GetCommandByExactName(lff.LowerCaseName())) {
    // Decide whether to invoke the command.
    if (!cmSystemTools::GetFatalErrorOccurred()) {
      // if trace is enabled, print out invoke information
      if (GetCMakeInstance()->GetTrace()) {
        PrintCommandTrace(lff, m_backtrace);
      }
      // Try invoking the command.
      bool invokeSucceeded = command(lff.Arguments(), status);
      bool hadNestedError = status.GetNestedError();
      if (!invokeSucceeded || hadNestedError) {
        if (!hadNestedError) {
          // The command invocation requested that we report an error.
          std::string const error = cmStrCat(lff.OriginalName(), ' ', status.GetError());
          IssueMessage(MessageType::FATAL_ERROR, error);
        }
        result = false;
        if (GetCMakeInstance()->GetCommandFailureAction() == CMake::CommandFailureAction::FATAL_ERROR) {
          cmSystemTools::SetFatalErrorOccurred();
        }
      }
      if (GetCMakeInstance()->HasScriptModeExitCode() && GetCMakeInstance()->GetWorkingMode() == CMake::SCRIPT_MODE) {
        // pass-through the exit code from inner cmake_language(EXIT) ,
        // possibly from include() or similar command...
        status.SetExitCode(GetCMakeInstance()->GetScriptModeExitCode());
      }
    }
  } else {
    if (!cmSystemTools::GetFatalErrorOccurred()) {
      std::string error = cmStrCat("Unknown CMake command \"", lff.OriginalName(), "\".");
      IssueMessage(MessageType::FATAL_ERROR, error);
      result = false;
      cmSystemTools::SetFatalErrorOccurred();
    }
  }

  if (m_executeCommandCallback) {
    m_executeCommandCallback();
  }

  return result;
}

bool cmMakefile::IsImportedTargetGlobalScope() const
{
  return m_currentImportedTargetScope == ImportedTargetScope::Global;
}

class cmMakefile::IncludeScope : public FileScopeBase
{
public:
  IncludeScope(
    cmMakefile* mf,
    std::string const& filenametoread,
    bool noPolicyScope);
  ~IncludeScope();
  void Quiet() { m_reportError = false; }

  IncludeScope(IncludeScope const&) = delete;
  IncludeScope& operator=(IncludeScope const&) = delete;

private:
  bool m_noPolicyScope;
  bool m_reportError = true;
};

cmMakefile::IncludeScope::IncludeScope(
  cmMakefile* mf,
  std::string const& filenametoread,
  bool noPolicyScope)
  : FileScopeBase(mf)
  , m_noPolicyScope(noPolicyScope)
{
  m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Push(cmListFileContext::FromListFilePath(filenametoread));

  m_pMakefile->PushFunctionBlockerBarrier();

  m_pMakefile->m_stateSnapshot =
    m_pMakefile->GetState()->CreateIncludeFileSnapshot(m_pMakefile->m_stateSnapshot, filenametoread);
  if (!m_noPolicyScope) {
    m_pMakefile->PushPolicy();
  }
  PushListFileVars(filenametoread);
}

cmMakefile::IncludeScope::~IncludeScope()
{
  PopListFileVars();
  if (!m_noPolicyScope) {
    // Pop the scope we pushed for the script.
    m_pMakefile->PopPolicy();
  }
  m_pMakefile->PopSnapshot(m_reportError);

  m_pMakefile->PopFunctionBlockerBarrier(m_reportError);

  m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Pop();
}

bool cmMakefile::ReadDependentFile(
  std::string const& filename,
  bool noPolicyScope)
{
  std::string filenametoread = cmSystemTools::CollapseFullPath(filename, GetCurrentSourceDirectory());

  IncludeScope incScope(this, filenametoread, noPolicyScope);

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnBeginFileParse(this, filenametoread);
  }
#endif

  cmListFile listFile;
  if (!listFile.ParseFile(filenametoread.c_str(), GetMessenger(), m_backtrace)) {
#ifdef CMake_ENABLE_DEBUGGER
    if (GetCMakeInstance()->GetDebugAdapter()) {
      GetCMakeInstance()->GetDebugAdapter()->OnEndFileParse();
    }
#endif

    return false;
  }

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnEndFileParse();
    GetCMakeInstance()->GetDebugAdapter()->OnFileParsedSuccessfully(filenametoread, listFile.Functions);
  }
#endif

  RunListFile(listFile, filenametoread);
  if (cmSystemTools::GetFatalErrorOccurred()) {
    incScope.Quiet();
  }
  return true;
}

class cmMakefile::ListFileScope : public FileScopeBase
{
public:
  ListFileScope(
    cmMakefile* mf,
    std::string const& filenametoread)
    : FileScopeBase(mf)
  {
    m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Push(cmListFileContext::FromListFilePath(filenametoread));

    m_pMakefile->m_stateSnapshot =
      m_pMakefile->GetState()->CreateInlineListFileSnapshot(m_pMakefile->m_stateSnapshot, filenametoread);
    assert(m_pMakefile->m_stateSnapshot.IsValid());

    m_pMakefile->PushFunctionBlockerBarrier();
    PushListFileVars(filenametoread);
  }

  ~ListFileScope()
  {
    PopListFileVars();
    m_pMakefile->PopSnapshot(m_reportError);
    m_pMakefile->PopFunctionBlockerBarrier(m_reportError);
    m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Pop();
  }

  void Quiet() { m_reportError = false; }

  ListFileScope(ListFileScope const&) = delete;
  ListFileScope& operator=(ListFileScope const&) = delete;

private:
  bool m_reportError = true;
};

class cmMakefile::DeferScope
{
public:
  DeferScope(
    cmMakefile* mf,
    std::string const& deferredInFile)
    : m_pMakefile(mf)
  {
    cmListFileContext lfc;
    lfc.Line = cmListFileContext::DeferPlaceholderLine;
    lfc.m_filePath = deferredInFile;
    m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Push(lfc);
    m_pMakefile->m_deferRunning = true;
  }

  ~DeferScope()
  {
    m_pMakefile->m_deferRunning = false;
    m_pMakefile->m_backtrace = m_pMakefile->m_backtrace.Pop();
  }

  DeferScope(DeferScope const&) = delete;
  DeferScope& operator=(DeferScope const&) = delete;

private:
  cmMakefile* m_pMakefile;
};

class cmMakefile::DeferCallScope
{
public:
  DeferCallScope(
    cmMakefile* mf,
    std::string const& deferredFromFile)
    : m_pMakefile(mf)
  {
    m_pMakefile->m_stateSnapshot =
      m_pMakefile->GetState()->CreateDeferCallSnapshot(m_pMakefile->m_stateSnapshot, deferredFromFile);
    assert(m_pMakefile->m_stateSnapshot.IsValid());
  }

  ~DeferCallScope() { m_pMakefile->PopSnapshot(); }

  DeferCallScope(DeferCallScope const&) = delete;
  DeferCallScope& operator=(DeferCallScope const&) = delete;

private:
  cmMakefile* m_pMakefile;
};

bool cmMakefile::ReadListFile(std::string const& filename)
{
  FunctionTrace f(__func__);

  std::string filenametoread = cmSystemTools::CollapseFullPath(filename, GetCurrentSourceDirectory());
  f.more("filenametoread: ", filenametoread);

  ListFileScope scope(this, filenametoread);

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnBeginFileParse(this, filenametoread);
  }
#endif

  cmListFile listFile;
  if (!listFile.ParseFile(filenametoread.c_str(), GetMessenger(), m_backtrace)) {
#ifdef CMake_ENABLE_DEBUGGER
    if (GetCMakeInstance()->GetDebugAdapter()) {
      GetCMakeInstance()->GetDebugAdapter()->OnEndFileParse();
    }
#endif

    return false;
  }

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnEndFileParse();
    GetCMakeInstance()->GetDebugAdapter()->OnFileParsedSuccessfully(filenametoread, listFile.Functions);
  }
#endif

  RunListFile(listFile, filenametoread);
  if (cmSystemTools::GetFatalErrorOccurred()) {
    scope.Quiet();
  }
  return true;
}

bool cmMakefile::ReadListFileAsString(
  std::string const& content,
  std::string const& virtualFileName)
{
  std::string filenametoread = cmSystemTools::CollapseFullPath(virtualFileName, GetCurrentSourceDirectory());

  ListFileScope scope(this, filenametoread);

  cmListFile listFile;
  if (!listFile.ParseString(content.c_str(), virtualFileName.c_str(), GetMessenger(), m_backtrace)) {
    return false;
  }

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnFileParsedSuccessfully(filenametoread, listFile.Functions);
  }
#endif

  RunListFile(listFile, filenametoread);
  if (cmSystemTools::GetFatalErrorOccurred()) {
    scope.Quiet();
  }
  return true;
}

void cmMakefile::RunListFile(
  cmListFile const& listFile,
  std::string const& filenametoread,
  DeferCommands* defer)
{
  // add this list file to the list of dependencies
  m_listFiles.push_back(filenametoread);

  // Run the parsed commands.
  size_t const numberFunctions = listFile.Functions.size();
  for (size_t i = 0; i < numberFunctions; ++i) {
    cmExecutionStatus status(*this);
    ExecuteCommand(listFile.Functions[i], status);
    if (cmSystemTools::GetFatalErrorOccurred()) {
      break;
    }
    if (status.HasExitCode()) {
      // cmake_language EXIT was requested, early break.
      GetCMakeInstance()->SetScriptModeExitCode(status.GetExitCode());
      break;
    }
    if (status.GetReturnInvoked()) {
      RaiseScope(status.GetReturnVariables());
      // Exit early due to return command.
      break;
    }
  }

  // Run any deferred commands.
  if (defer) {
    // Add a backtrace level indicating calls are deferred.
    DeferScope scope(this, filenametoread);

    // Iterate by index in case one deferred call schedules another.
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (size_t i = 0; i < defer->m_commands.size(); ++i) {
      DeferCommand& d = defer->m_commands[i];
      if (d.m_id.empty()) {
        // Canceled.
        continue;
      }
      // Mark as executed.
      std::string id = std::move(d.m_id);

      // The deferred call may have come from another file.
      DeferCallScope callScope(this, d.m_filePath);

      cmExecutionStatus status(*this);
      ExecuteCommand(d.m_command, status, std::move(id));
      if (cmSystemTools::GetFatalErrorOccurred()) {
        break;
      }
    }
  }
}

void cmMakefile::EnforceDirectoryLevelRules() const
{
  // Diagnose a violation of CMP0000 if necessary.
  if (m_checkCMP0000) {
    std::string e = cmStrCat(
      "No cmake_minimum_required command is present.  "
      "A line of code such as\n"
      "  cmake_minimum_required(VERSION ",
      cmVersion::GetMajorVersion(), '.', cmVersion::GetMinorVersion(),
      ")\n"
      "should be added at the top of the file.  "
      "The version specified may be lower if you wish to "
      "support older CMake versions for this project.  "
      "For more information run "
      "\"cmake --help-policy CMP0000\".");
    GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR, e, m_backtrace);
    cmSystemTools::SetFatalErrorOccurred();
  }
}

void cmMakefile::AddEvaluationFile(
  std::string const& inputFile,
  std::string const& targetName,
  std::unique_ptr<cmCompiledGeneratorExpression> outputName,
  std::unique_ptr<cmCompiledGeneratorExpression> condition,
  std::string const& newLineCharacter,
  mode_t permissions,
  bool inputIsContent)
{
  m_evaluationFiles.push_back(
    cm::make_unique<cmGeneratorExpressionEvaluationFile>(
      inputFile, targetName, std::move(outputName), std::move(condition), inputIsContent, newLineCharacter, permissions,
      GetPolicyStatus(cmPolicies::CMP0070)));
}

std::vector<std::unique_ptr<cmGeneratorExpressionEvaluationFile>> const& cmMakefile::GetEvaluationFiles() const
{
  return m_evaluationFiles;
}

std::vector<std::unique_ptr<cmExportBuildFileGenerator>> const& cmMakefile::GetExportBuildFileGenerators() const
{
  return m_exportBuildFileGenerators;
}

void cmMakefile::AddExportBuildFileGenerator(std::unique_ptr<cmExportBuildFileGenerator> gen)
{
  m_exportBuildFileGenerators.emplace_back(std::move(gen));
}

namespace {
struct file_not_persistent
{
  bool operator()(std::string const& path) const
  {
    return !(path.find("CMakeTmp") == std::string::npos && cmSystemTools::FileExists(path));
  }
};
}

void cmMakefile::AddGeneratorAction(GeneratorAction&& action)
{
  assert(!m_generatorActionsInvoked);
  m_generatorActions.emplace_back(std::move(action), m_backtrace);
}

void cmMakefile::GeneratorAction::operator()(
  cmLocalGenerator& lg,
  cmListFileBacktrace const& lfbt,
  GeneratorActionWhen when)
{
  if (m_generatorActionWhen != when) {
    return;
  }

  if (m_pCustomCommand) {
    m_CCAction(lg, lfbt, std::move(m_pCustomCommand));
  } else {
    assert(m_action);
    m_action(lg, lfbt);
  }
}

void cmMakefile::DoGenerate(cmLocalGenerator& lg)
{
  // give all the commands a chance to do something
  // after the file has been parsed before generation
  for (auto& action : m_generatorActions) {
    action.Value(lg, action.m_backtrace, GeneratorActionWhen::AfterConfigure);
  }
  m_generatorActionsInvoked = true;

  // go through all configured files and see which ones still exist.
  // we don't want cmake to re-run if a configured file is created and deleted
  // during processing as that would make it a transient file that can't
  // influence the build process
  cm::erase_if(m_outputFiles, file_not_persistent());

  // if a configured file is used as input for another configured file,
  // and then deleted it will show up in the input list files so we
  // need to scan those too
  cm::erase_if(m_listFiles, file_not_persistent());
}

// Generate the output file
void cmMakefile::Generate(cmLocalGenerator& lg)
{
  DoGenerate(lg);
  cmValue oldValue = GetDefinition("CMAKE_BACKWARDS_COMPATIBILITY");
  if (oldValue && cmSystemTools::VersionCompare(cmSystemTools::OP_LESS, *oldValue, "2.4")) {
    GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      "You have set CMAKE_BACKWARDS_COMPATIBILITY to a CMake version less "
      "than 2.4. This version of CMake only supports backwards compatibility "
      "with CMake 2.4 or later. For compatibility with older versions please "
      "use any CMake 2.8.x release or lower.",
      m_backtrace);
  }
}

void cmMakefile::GenerateAfterGeneratorTargets(cmLocalGenerator& lg)
{
  for (auto& action : m_generatorActions) {
    action.Value(lg, action.m_backtrace, GeneratorActionWhen::AfterGeneratorTargets);
  }
}

namespace {
// There are still too many implicit backtraces through cmMakefile.  As a
// workaround we reset the backtrace temporarily.
struct BacktraceGuard
{
  BacktraceGuard(
    cmListFileBacktrace& lfbt,
    cmListFileBacktrace current)
    : m_backtrace(lfbt)
    , m_previous(lfbt)
  {
    m_backtrace = std::move(current);
  }

  ~BacktraceGuard() { m_backtrace = std::move(m_previous); }

private:
  cmListFileBacktrace& m_backtrace;
  cmListFileBacktrace m_previous;
};
}

bool cmMakefile::ValidateCustomCommand(cmCustomCommandLines const& commandLines) const
{
  // TODO: More strict?
  auto const it = std::find_if(commandLines.begin(), commandLines.end(), [](cmCustomCommandLine const& cl) {
    return !cl.empty() && !cl[0].empty() && cl[0][0] == '"';
  });
  if (it != commandLines.end()) {
    IssueMessage(MessageType::FATAL_ERROR, cmStrCat("COMMAND may not contain literal quotes:\n  ", (*it)[0], '\n'));
    return false;
  }
  return true;
}

cmTarget* cmMakefile::GetCustomCommandTarget(
  std::string const& target,
  cmObjectLibraryCommands objLibCommands,
  cmListFileBacktrace const& lfbt) const
{
  auto realTarget = target;

  auto ai = m_aliasTargets.find(target);
  if (ai != m_aliasTargets.end()) {
    realTarget = ai->second;
  }

  // Find the target to which to add the custom command.
  auto ti = m_targets.find(realTarget);
  if (ti == m_targets.end()) {
    std::string e;
    if (cmTarget const* t = FindTargetToUse(target)) {
      if (t->IsImported()) {
        e += cmStrCat("TARGET '", target, "' is IMPORTED and does not build here.");
      } else {
        e += cmStrCat("TARGET '", target, "' was not created in this directory.");
      }
    } else {
      e += cmStrCat("No TARGET '", target, "' has been created in this directory.");
    }
    GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR, e, lfbt);
    return nullptr;
  }

  cmTarget* t = &ti->second;
  if (objLibCommands == cmObjectLibraryCommands::Reject && t->GetType() == cmStateEnums::OBJECT_LIBRARY) {
    auto e = cmStrCat(
      "Target \"", target,
      "\" is an OBJECT library "
      "that may not have PRE_BUILD, PRE_LINK, or POST_BUILD commands.");
    GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR, e, lfbt);
    return nullptr;
  }
  if (t->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
    auto e = cmStrCat(
      "Target \"", target,
      "\" is an INTERFACE library "
      "that may not have PRE_BUILD, PRE_LINK, or POST_BUILD commands.");
    GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR, e, lfbt);
    return nullptr;
  }

  return t;
}

cmTarget* cmMakefile::AddCustomCommandToTarget(
  std::string const& target,
  cmCustomCommandType type,
  std::unique_ptr<cmCustomCommand> m_pCustomCommand)
{
  auto const& byproducts = m_pCustomCommand->GetByproducts();
  auto const& commandLines = m_pCustomCommand->GetCommandLines();

  cmTarget* t = GetCustomCommandTarget(target, cmObjectLibraryCommands::Reject, m_backtrace);

  // Validate custom commands.
  if (!t || !ValidateCustomCommand(commandLines)) {
    return t;
  }

  // Always create the byproduct sources and mark them generated.
  CreateGeneratedOutputs(byproducts);

  m_pCustomCommand->RecordPolicyValues(GetStateSnapshot());

  // Dispatch command creation to allow generator expressions in outputs.
  AddGeneratorAction(
    std::move(m_pCustomCommand),
    [this, t, type](cmLocalGenerator& lg, cmListFileBacktrace const& lfbt, std::unique_ptr<cmCustomCommand> tcc) {
      BacktraceGuard guard(this->m_backtrace, lfbt);
      tcc->SetBacktrace(lfbt);
      detail::AddCustomCommandToTarget(lg, cmCommandOrigin::Project, t, type, std::move(tcc));
    });

  return t;
}

void cmMakefile::AddCustomCommandToOutput(
  std::unique_ptr<cmCustomCommand> m_pCustomCommand,
  CommandSourceCallback const& callback,
  bool replace)
{
  auto const& outputs = m_pCustomCommand->GetOutputs();
  auto const& byproducts = m_pCustomCommand->GetByproducts();
  auto const& commandLines = m_pCustomCommand->GetCommandLines();

  // Make sure there is at least one output.
  if (outputs.empty()) {
    cmSystemTools::Error("Attempt to add a custom rule with no output!");
    return;
  }

  // Validate custom commands.
  if (!ValidateCustomCommand(commandLines)) {
    return;
  }

  // Always create the output sources and mark them generated.
  CreateGeneratedOutputs(outputs);
  CreateGeneratedOutputs(byproducts);

  m_pCustomCommand->RecordPolicyValues(GetStateSnapshot());

  // Dispatch command creation to allow generator expressions in outputs.
  AddGeneratorAction(
    std::move(m_pCustomCommand),
    [this, replace,
     callback](cmLocalGenerator& lg, cmListFileBacktrace const& lfbt, std::unique_ptr<cmCustomCommand> tcc) {
      BacktraceGuard guard(this->m_backtrace, lfbt);
      tcc->SetBacktrace(lfbt);
      cmSourceFile* sf = detail::AddCustomCommandToOutput(lg, cmCommandOrigin::Project, std::move(tcc), replace);
      if (callback && sf) {
        callback(sf);
      }
    });
}

void cmMakefile::AppendCustomCommandToOutput(
  std::string const& output,
  std::vector<std::string> const& depends,
  cmImplicitDependsList const& implicit_depends,
  cmCustomCommandLines const& commandLines)
{
  // Validate custom commands.
  if (ValidateCustomCommand(commandLines)) {
    // Dispatch command creation to allow generator expressions in outputs.
    AddGeneratorAction(
      [this, output, depends, implicit_depends, commandLines](cmLocalGenerator& lg, cmListFileBacktrace const& lfbt) {
        BacktraceGuard guard(this->m_backtrace, lfbt);
        detail::AppendCustomCommandToOutput(lg, lfbt, output, depends, implicit_depends, commandLines);
      });
  }
}

cmTarget* cmMakefile::AddUtilityCommand(
  std::string const& utilityName,
  bool excludeFromAll,
  std::unique_ptr<cmCustomCommand> m_pCustomCommand)
{
  auto const& depends = m_pCustomCommand->GetDepends();
  auto const& byproducts = m_pCustomCommand->GetByproducts();
  auto const& commandLines = m_pCustomCommand->GetCommandLines();
  cmTarget* target = AddNewUtilityTarget(utilityName, excludeFromAll);

  // Validate custom commands.
  if ((commandLines.empty() && depends.empty()) || !ValidateCustomCommand(commandLines)) {
    return target;
  }

  // Always create the byproduct sources and mark them generated.
  CreateGeneratedOutputs(byproducts);

  m_pCustomCommand->RecordPolicyValues(GetStateSnapshot());

  // Dispatch command creation to allow generator expressions in outputs.
  AddGeneratorAction(
    std::move(m_pCustomCommand),
    [this, target](cmLocalGenerator& lg, cmListFileBacktrace const& lfbt, std::unique_ptr<cmCustomCommand> tcc) {
      BacktraceGuard guard(this->m_backtrace, lfbt);
      tcc->SetBacktrace(lfbt);
      detail::AddUtilityCommand(lg, cmCommandOrigin::Project, target, std::move(tcc));
    });

  return target;
}

static void s_AddDefineFlag(
  std::string const& flag,
  std::string& dflags)
{
  // remove any \n\r
  std::string::size_type initSize = dflags.size();
  dflags += ' ';
  dflags += flag;
  std::string::iterator flagStart = dflags.begin() + initSize + 1;
  std::replace(flagStart, dflags.end(), '\n', ' ');
  std::replace(flagStart, dflags.end(), '\r', ' ');
}

void cmMakefile::AddDefineFlag(std::string const& flag)
{
  if (flag.empty()) {
    return;
  }

  // If this is really a definition, update COMPILE_DEFINITIONS.
  if (ParseDefineFlag(flag, false)) {
    return;
  }

  // Add this flag that does not look like a definition.
  s_AddDefineFlag(flag, m_defineFlags);
}

static void s_RemoveDefineFlag(
  std::string const& flag,
  std::string& dflags)
{
  std::string::size_type const len = flag.length();
  // Remove all instances of the flag that are surrounded by
  // whitespace or the beginning/end of the string.
  for (std::string::size_type lpos = dflags.find(flag, 0); lpos != std::string::npos; lpos = dflags.find(flag, lpos)) {
    std::string::size_type rpos = lpos + len;
    if ((lpos <= 0 || cmIsSpace(dflags[lpos - 1])) && (rpos >= dflags.size() || cmIsSpace(dflags[rpos]))) {
      dflags.erase(lpos, len);
    } else {
      ++lpos;
    }
  }
}

void cmMakefile::RemoveDefineFlag(std::string const& flag)
{
  // Check the length of the flag to remove.
  if (flag.empty()) {
    return;
  }

  // If this is really a definition, update COMPILE_DEFINITIONS.
  if (ParseDefineFlag(flag, true)) {
    return;
  }

  // Remove this flag that does not look like a definition.
  s_RemoveDefineFlag(flag, m_defineFlags);
}

void cmMakefile::AddCompileDefinition(std::string const& option)
{
  AppendProperty("COMPILE_DEFINITIONS", option);
}

void cmMakefile::AddCompileOption(std::string const& option)
{
  AppendProperty("COMPILE_OPTIONS", option);
}

void cmMakefile::AddLinkOption(std::string const& option)
{
  AppendProperty("LINK_OPTIONS", option);
}

void cmMakefile::AddLinkDirectory(
  std::string const& directory,
  bool before)
{
  if (before) {
    m_stateSnapshot.GetDirectory().PrependLinkDirectoriesEntry(BT<std::string>(directory, m_backtrace));
  } else {
    m_stateSnapshot.GetDirectory().AppendLinkDirectoriesEntry(BT<std::string>(directory, m_backtrace));
  }
}

bool cmMakefile::ParseDefineFlag(
  std::string const& def,
  bool remove)
{
  // Create a regular expression to match valid definitions.
  static cmsys::RegularExpression valid("^[-/]D[A-Za-z_][A-Za-z0-9_]*(=.*)?$");

  // Make sure the definition matches.
  if (!valid.find(def)) {
    return false;
  }

  // Get the definition part after the flag.
  char const* define = def.c_str() + 2;

  if (remove) {
    if (cmValue cdefs = GetProperty("COMPILE_DEFINITIONS")) {
      // Expand the list.
      cmList defs{ *cdefs };

      // Recompose the list without the definition.
      defs.remove_items({ define });

      // Store the new list.
      SetProperty("COMPILE_DEFINITIONS", defs.to_string());
    }
  } else {
    // Append the definition to the directory property.
    AppendProperty("COMPILE_DEFINITIONS", define);
  }

  return true;
}

void cmMakefile::InitializeFromParent(cmMakefile* parent)
{
  m_systemIncludeDirectories = parent->m_systemIncludeDirectories;

  // define flags
  m_defineFlags = parent->m_defineFlags;

  // Include transform property.  There is no per-config version.
  {
    char const* prop = "IMPLICIT_DEPENDS_INCLUDE_TRANSFORM";
    SetProperty(prop, parent->GetProperty(prop));
  }

  // labels
  SetProperty("LABELS", parent->GetProperty("LABELS"));

  // link libraries
  SetProperty("LINK_LIBRARIES", parent->GetProperty("LINK_LIBRARIES"));

  // the initial project name
  m_stateSnapshot.SetProjectName(parent->m_stateSnapshot.GetProjectName());

  // Copy include regular expressions.
  m_complainFileRegularExpression = parent->m_complainFileRegularExpression;

  // Imported targets.
  m_importedTargets = parent->m_importedTargets;

  // Non-global Alias targets.
  m_aliasTargets = parent->m_aliasTargets;

  // Recursion depth.
  m_recursionDepth = parent->m_recursionDepth;
}

void cmMakefile::AddInstallGenerator(std::unique_ptr<cmInstallGenerator> g)
{
  if (g) {
    m_installGenerators.push_back(std::move(g));
  }
}

void cmMakefile::AddTestGenerator(std::unique_ptr<cmTestGenerator> g)
{
  if (g) {
    m_testGenerators.push_back(std::move(g));
  }
}

void cmMakefile::PushFunctionScope(
  std::string const& fileName,
  cmPolicies::PolicyMap const& pm)
{
  m_stateSnapshot = GetState()->CreateFunctionCallSnapshot(m_stateSnapshot, fileName);
  assert(m_stateSnapshot.IsValid());

  PushLoopBlockBarrier();

#if !defined(CMAKE_BOOTSTRAP)
  GetGlobalGenerator()->GetFileLockPool().PushFunctionScope();
#endif

  PushFunctionBlockerBarrier();

  PushPolicy(true, pm);
}

void cmMakefile::PopFunctionScope(bool reportError)
{
  PopPolicy();

  PopSnapshot(reportError);

  PopFunctionBlockerBarrier(reportError);

#if !defined(CMAKE_BOOTSTRAP)
  GetGlobalGenerator()->GetFileLockPool().PopFunctionScope();
#endif

  PopLoopBlockBarrier();
}

void cmMakefile::PushMacroScope(
  std::string const& fileName,
  cmPolicies::PolicyMap const& pm)
{
  m_stateSnapshot = GetState()->CreateMacroCallSnapshot(m_stateSnapshot, fileName);
  assert(m_stateSnapshot.IsValid());

  PushFunctionBlockerBarrier();

  PushPolicy(true, pm);
}

void cmMakefile::PopMacroScope(bool reportError)
{
  PopPolicy();
  PopSnapshot(reportError);

  PopFunctionBlockerBarrier(reportError);
}

bool cmMakefile::IsRootMakefile() const
{
  return !m_stateSnapshot.GetBuildsystemDirectoryParent().IsValid();
}

class cmMakefile::BuildsystemFileScope : public FileScopeBase
{
public:
  BuildsystemFileScope(cmMakefile* mf)
    : FileScopeBase(mf)
  {
    std::string currentStart =
      m_pMakefile->GetCMakeInstance()->GetCMakeListFile(m_pMakefile->m_stateSnapshot.GetDirectory().GetCurrentSource());
    m_pMakefile->m_stateSnapshot.SetListFile(currentStart);
    m_pMakefile->m_stateSnapshot =
      m_pMakefile->m_stateSnapshot.GetState()->CreatePolicyScopeSnapshot(m_pMakefile->m_stateSnapshot);
    m_pMakefile->PushFunctionBlockerBarrier();
    PushListFileVars(currentStart);

    m_globalGenerator = mf->GetGlobalGenerator();
    m_currentMakefile = m_globalGenerator->GetCurrentMakefile();
    m_snapshot = m_globalGenerator->GetCMakeInstance()->GetCurrentSnapshot();
    m_globalGenerator->GetCMakeInstance()->SetCurrentSnapshot(m_snapshot);
    m_globalGenerator->SetCurrentMakefile(mf);
#if !defined(CMAKE_BOOTSTRAP)
    m_globalGenerator->GetFileLockPool().PushFileScope();
#endif
  }

  ~BuildsystemFileScope()
  {
    PopListFileVars();
    m_pMakefile->PopFunctionBlockerBarrier(m_reportError);
    m_pMakefile->PopSnapshot(m_reportError);
#if !defined(CMAKE_BOOTSTRAP)
    m_globalGenerator->GetFileLockPool().PopFileScope();
#endif
    m_globalGenerator->SetCurrentMakefile(m_currentMakefile);
    m_globalGenerator->GetCMakeInstance()->SetCurrentSnapshot(m_snapshot);
  }

  void Quiet() { m_reportError = false; }

  BuildsystemFileScope(BuildsystemFileScope const&) = delete;
  BuildsystemFileScope& operator=(BuildsystemFileScope const&) = delete;

private:
  cmGlobalGenerator* m_globalGenerator;
  cmMakefile* m_currentMakefile;
  cmStateSnapshot m_snapshot;
  bool m_reportError = true;
};

void cmMakefile::Configure()
{
  std::string currentStart = GetCMakeInstance()->GetCMakeListFile(m_stateSnapshot.GetDirectory().GetCurrentSource());

  // Add the bottom of all backtraces within this directory.
  // We will never pop this scope because it should be available
  // for messages during the generate step too.
  m_backtrace = m_backtrace.Push(cmListFileContext::FromListFilePath(currentStart));

  BuildsystemFileScope scope(this);

  // make sure the CMakeFiles dir is there
  std::string filesDir = cmStrCat(m_stateSnapshot.GetDirectory().GetCurrentBinary(), "/CMakeFiles");
  cmSystemTools::MakeDirectory(filesDir);

  assert(cmSystemTools::FileExists(currentStart, true));
  AddDefinition(kCMAKE_PARENT_LIST_FILE, currentStart);

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnBeginFileParse(this, currentStart);
  }
#endif

  cmListFile listFile;
  if (!listFile.ParseFile(currentStart.c_str(), GetMessenger(), m_backtrace)) {
#ifdef CMake_ENABLE_DEBUGGER
    if (GetCMakeInstance()->GetDebugAdapter()) {
      GetCMakeInstance()->GetDebugAdapter()->OnEndFileParse();
    }
#endif

    return;
  }

#ifdef CMake_ENABLE_DEBUGGER
  if (GetCMakeInstance()->GetDebugAdapter()) {
    GetCMakeInstance()->GetDebugAdapter()->OnEndFileParse();
    GetCMakeInstance()->GetDebugAdapter()->OnFileParsedSuccessfully(currentStart, listFile.Functions);
  }
#endif

  if (IsRootMakefile()) {
    bool hasVersion = false;
    // search for the right policy command
    for (cmListFileFunction const& func : listFile.Functions) {
      if (func.LowerCaseName() == "cmake_minimum_required"_s) {
        hasVersion = true;
        break;
      }
    }
    // if no policy command is found this is an error if they use any
    // non advanced functions or a lot of functions
    if (!hasVersion) {
      bool isProblem = true;
      if (listFile.Functions.size() < 30) {
        // the list of simple commands DO NOT ADD TO THIS LIST!!!!!
        // these commands must have backwards compatibility forever and
        // and that is a lot longer than your tiny mind can comprehend mortal
        std::set<std::string> allowedCommands;
        allowedCommands.insert("project");
        allowedCommands.insert("set");
        allowedCommands.insert("if");
        allowedCommands.insert("endif");
        allowedCommands.insert("else");
        allowedCommands.insert("elseif");
        allowedCommands.insert("add_executable");
        allowedCommands.insert("add_library");
        allowedCommands.insert("target_link_libraries");
        allowedCommands.insert("option");
        allowedCommands.insert("message");
        isProblem = false;
        for (cmListFileFunction const& func : listFile.Functions) {
          if (!cm::contains(allowedCommands, func.LowerCaseName())) {
            isProblem = true;
            break;
          }
        }
      }

      if (isProblem) {
        // Tell the top level cmMakefile to diagnose
        // this violation of CMP0000.
        SetCheckCMP0000(true);

        // Implicitly set the version for the user.
        cmPolicies::ApplyPolicyVersion(this, 3, 5, 0, cmPolicies::WarnCompat::Off);
      }
    }
    bool hasProject = false;
    // search for a project command
    for (cmListFileFunction const& func : listFile.Functions) {
      if (func.LowerCaseName() == "project"_s) {
        hasProject = true;
        break;
      }
    }
    // if no project command is found, add one
    if (!hasProject) {
      GetCMakeInstance()->IssueMessage(
        MessageType::AUTHOR_WARNING,
        "No project() command is present.  The top-level CMakeLists.txt "
        "file must contain a literal, direct call to the project() command.  "
        "Add a line of code such as\n"
        "  project(ProjectName)\n"
        "near the top of the file, but after cmake_minimum_required().\n"
        "CMake is pretending there is a \"project(Project)\" command on "
        "the first line.",
        m_backtrace);
      cmListFileFunction project{ "project", 0, 0, { { "Project", cmListFileArgument::Unquoted, 0 } } };
      listFile.Functions.insert(listFile.Functions.begin(), project);
    }
  }

  m_defer = cm::make_unique<DeferCommands>();
  RunListFile(listFile, currentStart, m_defer.get());
  m_defer.reset();
  if (cmSystemTools::GetFatalErrorOccurred()) {
    scope.Quiet();
  }

  // at the end handle any old style subdirs
  std::vector<cmMakefile*> subdirs = m_unconfiguredDirectories;

  // for each subdir recurse
  auto sdi = subdirs.begin();
  for (; sdi != subdirs.end(); ++sdi) {
    (*sdi)->m_stateSnapshot.InitializeFromParent_ForSubdirsCommand();
    ConfigureSubDirectory(*sdi);
  }

  AddCMakeDependFilesFromUser();
}

void cmMakefile::ConfigureSubDirectory(cmMakefile* mf)
{
  mf->InitializeFromParent(this);
  std::string currentStart = mf->GetCurrentSourceDirectory();
  if (GetCMakeInstance()->GetDebugOutput()) {
    std::string msg = cmStrCat("   Entering             ", currentStart);
    cmSystemTools::Message(msg);
  }

  std::string currentStartFile = GetCMakeInstance()->GetCMakeListFile(currentStart);
  if (!cmSystemTools::FileExists(currentStartFile, true)) {
    IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(
        "The source directory\n  ", currentStart,
        "\n"
        "does not contain a CMakeLists.txt file."));
    return;
  }
  // finally configure the subdir
  mf->Configure();

  if (GetCMakeInstance()->GetDebugOutput()) {
    auto msg = cmStrCat("   Returning to         ", GetCurrentSourceDirectory());
    cmSystemTools::Message(msg);
  }
}

void cmMakefile::AddSubDirectory(
  std::string const& srcPath,
  std::string const& binPath,
  bool excludeFromAll,
  bool immediate,
  bool system)
{
  if (m_deferRunning) {
    IssueMessage(MessageType::FATAL_ERROR, "Subdirectories may not be created during deferred execution.");
    return;
  }

  // Make sure the binary directory is unique.
  if (!EnforceUniqueDir(srcPath, binPath)) {
    return;
  }

  cmStateSnapshot newSnapshot = GetState()->CreateBuildsystemDirectorySnapshot(m_stateSnapshot);

  newSnapshot.GetDirectory().SetCurrentSource(srcPath);
  newSnapshot.GetDirectory().SetCurrentBinary(binPath);

  cmSystemTools::MakeDirectory(binPath);

  auto subMfu = cm::make_unique<cmMakefile>(m_pGlobalGenerator, newSnapshot);
  auto* subMf = subMfu.get();
  GetGlobalGenerator()->AddMakefile(std::move(subMfu));

  if (excludeFromAll) {
    subMf->SetProperty("EXCLUDE_FROM_ALL", "TRUE");
  }
  if (system) {
    subMf->SetProperty("SYSTEM", "TRUE");
  }

  if (immediate) {
    ConfigureSubDirectory(subMf);
  } else {
    m_unconfiguredDirectories.push_back(subMf);
  }

  AddInstallGenerator(cm::make_unique<cmInstallSubdirectoryGenerator>(subMf, binPath, GetBacktrace()));
}

std::string const& cmMakefile::GetCurrentSourceDirectory() const
{
  return m_stateSnapshot.GetDirectory().GetCurrentSource();
}

std::string const& cmMakefile::GetCurrentBinaryDirectory() const
{
  return m_stateSnapshot.GetDirectory().GetCurrentBinary();
}

cmTarget* cmMakefile::FindImportedTarget(std::string const& name) const
{
  auto const i = m_importedTargets.find(name);
  if (i != m_importedTargets.end()) {
    return i->second;
  }
  return nullptr;
}

std::vector<cmTarget*> cmMakefile::GetImportedTargets() const
{
  std::vector<cmTarget*> tgts;
  tgts.reserve(m_importedTargets.size());
  for (auto const& impTarget : m_importedTargets) {
    tgts.push_back(impTarget.second);
  }
  return tgts;
}

void cmMakefile::AddIncludeDirectories(
  std::vector<std::string> const& incs,
  bool before)
{
  if (incs.empty()) {
    return;
  }

  std::string entryString = cmList::to_string(incs);
  if (before) {
    m_stateSnapshot.GetDirectory().PrependIncludeDirectoriesEntry(BT<std::string>(entryString, m_backtrace));
  } else {
    m_stateSnapshot.GetDirectory().AppendIncludeDirectoriesEntry(BT<std::string>(entryString, m_backtrace));
  }

  // Property on each target:
  for (auto& target : m_targets) {
    cmTarget& t = target.second;
    t.InsertInclude(BT<std::string>(entryString, m_backtrace), before);
  }
}

void cmMakefile::AddSystemIncludeDirectories(std::set<std::string> const& incs)
{
  if (incs.empty()) {
    return;
  }

  m_systemIncludeDirectories.insert(incs.begin(), incs.end());

  for (auto& target : m_targets) {
    cmTarget& t = target.second;
    t.AddSystemIncludeDirectories(incs);
  }
}

void cmMakefile::AddDefinition(
  std::string const& name,
  cm::string_view value)
{
  m_stateSnapshot.SetDefinition(name, value);

#ifndef CMAKE_BOOTSTRAP
  cmVariableWatch* vv = GetVariableWatch();
  if (vv) {
    vv->VariableAccessed(name, cmVariableWatch::VARIABLE_MODIFIED_ACCESS, value.data(), this);
  }
#endif
}

void cmMakefile::AddDefinitionBool(
  std::string const& name,
  bool value)
{
  AddDefinition(name, value ? "ON" : "OFF");
}

void cmMakefile::AddCacheDefinition(
  std::string const& name,
  cmValue value,
  cmValue doc,
  cmStateEnums::CacheEntryType type,
  bool force)
{
  cmValue existingValue = GetState()->GetInitializedCacheValue(name);
  // must be outside the following if() to keep it alive long enough
  std::string nvalue;

  if (existingValue && (GetState()->GetCacheEntryType(name) == cmStateEnums::UNINITIALIZED)) {
    // if this is not a force, then use the value from the cache
    // if it is a force, then use the value being passed in
    if (!force) {
      value = existingValue;
    }
    if (type == cmStateEnums::PATH || type == cmStateEnums::FILEPATH) {
      cmList files(value);
      for (auto& file : files) {
        if (!cmIsOff(file)) {
          file = cmSystemTools::ToNormalizedPathOnDisk(file);
        }
      }
      nvalue = files.to_string();
      value = cmValue{ nvalue };

      GetCMakeInstance()->AddCacheEntry(name, value, doc, type);
      value = GetState()->GetInitializedCacheValue(name);
    }
  }
  GetCMakeInstance()->AddCacheEntry(name, value, doc, type);
  switch (GetPolicyStatus(cmPolicies::CMP0126)) {
    case cmPolicies::WARN:
      if (PolicyOptionalWarningEnabled("CMAKE_POLICY_WARNING_CMP0126") && IsNormalDefinitionSet(name)) {
        IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat(
            cmPolicies::GetPolicyWarning(cmPolicies::CMP0126),
            "\nFor compatibility with older versions of CMake, normal "
            "variable \"",
            name, "\" will be removed from the current scope."));
      }
      CM_FALLTHROUGH;
    case cmPolicies::OLD:
      // if there was a definition then remove it
      m_stateSnapshot.RemoveDefinition(name);
      break;
    case cmPolicies::NEW:
      break;
  }
}

void cmMakefile::MarkVariableAsUsed(std::string const& var)
{
  m_stateSnapshot.GetDefinition(var);
}

bool cmMakefile::VariableInitialized(std::string const& var) const
{
  return m_stateSnapshot.IsInitialized(var);
}

void cmMakefile::MaybeWarnUninitialized(
  std::string const& variable,
  char const* sourceFilename) const
{
  // check to see if we need to print a warning
  // if strict mode is on and the variable has
  // not been "cleared"/initialized with a set(foo ) call
  if (GetCMakeInstance()->GetWarnUninitialized() && !VariableInitialized(variable)) {
    if (m_checkSystemVars || (sourceFilename && IsProjectFile(sourceFilename))) {
      IssueMessage(MessageType::AUTHOR_WARNING, cmStrCat("uninitialized variable '", variable, '\''));
    }
  }
}

void cmMakefile::RemoveDefinition(std::string const& name)
{
  m_stateSnapshot.RemoveDefinition(name);
#ifndef CMAKE_BOOTSTRAP
  cmVariableWatch* vv = GetVariableWatch();
  if (vv) {
    vv->VariableAccessed(name, cmVariableWatch::VARIABLE_REMOVED_ACCESS, nullptr, this);
  }
#endif
}

void cmMakefile::RemoveCacheDefinition(std::string const& name) const
{
  GetState()->RemoveCacheEntry(name);
}

void cmMakefile::SetProjectName(std::string const& p)
{
  m_stateSnapshot.SetProjectName(p);
}

void cmMakefile::AddGlobalLinkInformation(cmTarget& target)
{
  // for these targets do not add anything
  switch (target.GetType()) {
    case cmStateEnums::UTILITY:
    case cmStateEnums::GLOBAL_TARGET:
    case cmStateEnums::INTERFACE_LIBRARY:
      return;
    default:;
  }

  if (cmValue linkLibsProp = GetProperty("LINK_LIBRARIES")) {
    cmList linkLibs{ *linkLibsProp };

    for (auto j = linkLibs.begin(); j != linkLibs.end(); ++j) {
      std::string libraryName = *j;
      cmTargetLinkLibraryType libType = GENERAL_LibraryType;
      if (libraryName == "optimized"_s) {
        libType = OPTIMIZED_LibraryType;
        ++j;
        libraryName = *j;
      } else if (libraryName == "debug"_s) {
        libType = DEBUG_LibraryType;
        ++j;
        libraryName = *j;
      }
      // This is equivalent to the target_link_libraries plain signature.
      target.AddLinkLibrary(*this, libraryName, libType);
      target.AppendProperty("INTERFACE_LINK_LIBRARIES", target.GetDebugGeneratorExpressions(libraryName, libType));
    }
  }
}

void cmMakefile::AddAlias(
  std::string const& lname,
  std::string const& tgtName,
  bool globallyVisible)
{
  m_aliasTargets[lname] = tgtName;
  if (globallyVisible) {
    GetGlobalGenerator()->AddAlias(lname, tgtName);
  }
}

cmTarget* cmMakefile::AddLibrary(
  std::string const& lname,
  cmStateEnums::TargetType type,
  std::vector<std::string> const& srcs,
  bool excludeFromAll)
{
  assert(
    type == cmStateEnums::STATIC_LIBRARY || type == cmStateEnums::SHARED_LIBRARY ||
    type == cmStateEnums::MODULE_LIBRARY || type == cmStateEnums::OBJECT_LIBRARY ||
    type == cmStateEnums::INTERFACE_LIBRARY);

  cmTarget* target = AddNewTarget(type, lname);
  // Clear its dependencies. Otherwise, dependencies might persist
  // over changes in CMakeLists.txt, making the information stale and
  // hence useless.
  target->ClearDependencyInformation(*this);
  if (excludeFromAll) {
    target->SetProperty("EXCLUDE_FROM_ALL", "TRUE");
  }
  target->AddSources(srcs);
  AddGlobalLinkInformation(*target);
  return target;
}

cmTarget* cmMakefile::AddExecutable(
  std::string const& exeName,
  std::vector<std::string> const& srcs,
  bool excludeFromAll)
{
  cmTarget* target = AddNewTarget(cmStateEnums::EXECUTABLE, exeName);
  if (excludeFromAll) {
    target->SetProperty("EXCLUDE_FROM_ALL", "TRUE");
  }
  target->AddSources(srcs);
  AddGlobalLinkInformation(*target);
  return target;
}

cmTarget* cmMakefile::AddNewTarget(
  cmStateEnums::TargetType type,
  std::string const& name)
{
  return &CreateNewTarget(name, type).first;
}

cmTarget* cmMakefile::AddSynthesizedTarget(
  cmStateEnums::TargetType type,
  std::string const& name)
{
  return &CreateNewTarget(name, type, cmTarget::PerConfig::Yes, cmTarget::Visibility::Generated).first;
}

std::pair<
  cmTarget&,
  bool>
cmMakefile::CreateNewTarget(
  std::string const& name,
  cmStateEnums::TargetType type,
  cmTarget::PerConfig perConfig,
  cmTarget::Visibility vis)
{
  auto ib = m_targets.emplace(name, cmTarget(name, type, vis, this, perConfig));
  auto it = ib.first;
  if (!ib.second) {
    return std::make_pair(std::ref(it->second), false);
  }
  m_orderedTargets.push_back(&it->second);
  GetGlobalGenerator()->IndexTarget(&it->second);
  GetStateSnapshot().GetDirectory().AddNormalTargetName(name);
  return std::make_pair(std::ref(it->second), true);
}

cmTarget* cmMakefile::AddNewUtilityTarget(
  std::string const& utilityName,
  bool excludeFromAll)
{
  cmTarget* target = AddNewTarget(cmStateEnums::UTILITY, utilityName);
  if (excludeFromAll) {
    target->SetProperty("EXCLUDE_FROM_ALL", "TRUE");
  }
  return target;
}

namespace {
}

#if !defined(CMAKE_BOOTSTRAP)
cmSourceGroup* cmMakefile::GetSourceGroup(std::vector<std::string> const& name) const
{
  cmSourceGroup* sg = nullptr;

  // first look for source group starting with the same as the one we want
  for (cmSourceGroup const& srcGroup : m_sourceGroups) {
    std::string const& sgName = srcGroup.GetName();
    if (sgName == name[0]) {
      sg = const_cast<cmSourceGroup*>(&srcGroup);
      break;
    }
  }

  if (sg) {
    // iterate through its children to find match source group
    for (unsigned int i = 1; i < name.size(); ++i) {
      sg = sg->LookupChild(name[i]);
      if (!sg) {
        break;
      }
    }
  }
  return sg;
}

void cmMakefile::AddSourceGroup(
  std::string const& name,
  char const* regex)
{
  std::vector<std::string> nameVector;
  nameVector.push_back(name);
  AddSourceGroup(nameVector, regex);
}

void cmMakefile::AddSourceGroup(
  std::vector<std::string> const& name,
  char const* regex)
{
  cmSourceGroup* sg = nullptr;
  std::vector<std::string> currentName;
  int i = 0;
  int const lastElement = static_cast<int>(name.size() - 1);
  for (i = lastElement; i >= 0; --i) {
    currentName.assign(name.begin(), name.begin() + i + 1);
    sg = GetSourceGroup(currentName);
    if (sg) {
      break;
    }
  }

  // i now contains the index of the last found component
  if (i == lastElement) {
    // group already exists, replace its regular expression
    if (regex && sg) {
      // We only want to set the regular expression.  If there are already
      // source files in the group, we don't want to remove them.
      sg->SetGroupRegex(regex);
    }
    return;
  }
  if (i == -1) {
    // group does not exist nor belong to any existing group
    // add its first component
    m_sourceGroups.emplace_back(name[0], regex);
    sg = GetSourceGroup(currentName);
    i = 0; // last component found
  }
  if (!sg) {
    cmSystemTools::Error("Could not create source group ");
    return;
  }
  // build the whole source group path
  for (++i; i <= lastElement; ++i) {
    sg->AddChild(cmSourceGroup(name[i], nullptr, sg->GetFullName().c_str()));
    sg = sg->LookupChild(name[i]);
  }

  sg->SetGroupRegex(regex);
}

cmSourceGroup* cmMakefile::GetOrCreateSourceGroup(std::vector<std::string> const& folders)
{
  cmSourceGroup* sg = GetSourceGroup(folders);
  if (!sg) {
    AddSourceGroup(folders);
    sg = GetSourceGroup(folders);
  }
  return sg;
}

cmSourceGroup* cmMakefile::GetOrCreateSourceGroup(std::string const& name)
{
  auto p = GetDefinition("SOURCE_GROUP_DELIMITER");
  return GetOrCreateSourceGroup(cmTokenize(name, p ? cm::string_view(*p) : R"(\/)"_s));
}

/**
 * Find a source group whose regular expression matches the filename
 * part of the given source name.  Search backward through the list of
 * source groups, and take the first matching group found.  This way
 * non-inherited SOURCE_GROUP commands will have precedence over
 * inherited ones.
 */
cmSourceGroup* cmMakefile::FindSourceGroup(
  std::string const& source,
  std::vector<cmSourceGroup>& groups) const
{
  // First search for a group that lists the file explicitly.
  for (auto sg = groups.rbegin(); sg != groups.rend(); ++sg) {
    cmSourceGroup* result = sg->MatchChildrenFiles(source);
    if (result) {
      return result;
    }
  }

  // Now search for a group whose regex matches the file.
  for (auto sg = groups.rbegin(); sg != groups.rend(); ++sg) {
    cmSourceGroup* result = sg->MatchChildrenRegex(source);
    if (result) {
      return result;
    }
  }

  // Shouldn't get here, but just in case, return the default group.
  return groups.data();
}
#endif

bool cmMakefile::IsOn(std::string const& name) const
{
  return GetDefinition(name).IsOn();
}

bool cmMakefile::IsSet(std::string const& name) const
{
  cmValue value = GetDefinition(name);
  if (!value) {
    return false;
  }

  if (value->empty()) {
    return false;
  }

  if (cmIsNOTFOUND(*value)) {
    return false;
  }

  return true;
}

bool cmMakefile::PlatformIs32Bit() const
{
  if (cmValue plat_abi = GetDefinition("CMAKE_INTERNAL_PLATFORM_ABI")) {
    if (*plat_abi == "ELF X32"_s) {
      return false;
    }
  }
  if (cmValue sizeof_dptr = GetDefinition("CMAKE_SIZEOF_VOID_P")) {
    return atoi(sizeof_dptr->c_str()) == 4;
  }
  return false;
}

bool cmMakefile::PlatformIs64Bit() const
{
  if (cmValue sizeof_dptr = GetDefinition("CMAKE_SIZEOF_VOID_P")) {
    return atoi(sizeof_dptr->c_str()) == 8;
  }
  return false;
}

bool cmMakefile::PlatformIsx32() const
{
  if (cmValue plat_abi = GetDefinition("CMAKE_INTERNAL_PLATFORM_ABI")) {
    if (*plat_abi == "ELF X32"_s) {
      return true;
    }
  }
  return false;
}

cmMakefile::AppleSDK cmMakefile::GetAppleSDKType() const
{
  std::string sdkRoot;
  sdkRoot = GetSafeDefinition("CMAKE_OSX_SYSROOT");
  sdkRoot = cmSystemTools::LowerCase(sdkRoot);

  struct
  {
    std::string name;
    AppleSDK sdk;
  } const sdkDatabase[]{
    { "appletvos", AppleSDK::AppleTVOS }, { "appletvsimulator", AppleSDK::AppleTVSimulator },
    { "iphoneos", AppleSDK::IPhoneOS },   { "iphonesimulator", AppleSDK::IPhoneSimulator },
    { "watchos", AppleSDK::WatchOS },     { "watchsimulator", AppleSDK::WatchSimulator },
    { "xros", AppleSDK::XROS },           { "xrsimulator", AppleSDK::XRSimulator },
  };

  for (auto const& entry : sdkDatabase) {
    if (cmHasPrefix(sdkRoot, entry.name) || sdkRoot.find(cmStrCat('/', entry.name)) != std::string::npos) {
      return entry.sdk;
    }
  }

  return AppleSDK::MacOS;
}

bool cmMakefile::PlatformIsAppleEmbedded() const
{
  return GetAppleSDKType() != AppleSDK::MacOS;
}

bool cmMakefile::PlatformIsAppleSimulator() const
{
  return std::set<AppleSDK>{
    AppleSDK::AppleTVSimulator,
    AppleSDK::IPhoneSimulator,
    AppleSDK::WatchSimulator,
    AppleSDK::XRSimulator,
  }
    .count(GetAppleSDKType());
}

bool cmMakefile::PlatformIsAppleCatalyst() const
{
  std::string systemName;
  systemName = GetSafeDefinition("CMAKE_SYSTEM_NAME");
  systemName = cmSystemTools::LowerCase(systemName);
  return systemName == "ios" && GetAppleSDKType() == AppleSDK::MacOS;
}

bool cmMakefile::PlatformSupportsAppleTextStubs() const
{
  return IsOn("APPLE") && IsSet("CMAKE_TAPI");
}

char const* cmMakefile::GetSONameFlag(std::string const& language) const
{
  std::string name = "CMAKE_SHARED_LIBRARY_SONAME";
  if (!language.empty()) {
    name += "_";
    name += language;
  }
  name += "_FLAG";
  return GetDefinition(name).GetCStr();
}

bool cmMakefile::CanIWriteThisFile(std::string const& fileName) const
{
  if (!IsOn("CMAKE_DISABLE_SOURCE_CHANGES")) {
    return true;
  }
  // If we are doing an in-source build, then the test will always fail
  if (cmSystemTools::SameFile(GetHomeDirectory(), GetHomeOutputDirectory())) {
    return !IsOn("CMAKE_DISABLE_IN_SOURCE_BUILD");
  }

  return !cmSystemTools::IsSubDirectory(fileName, GetHomeDirectory()) ||
    cmSystemTools::IsSubDirectory(fileName, GetHomeOutputDirectory()) ||
    cmSystemTools::SameFile(fileName, GetHomeOutputDirectory());
}

std::string const& cmMakefile::GetRequiredDefinition(std::string const& name) const
{
  static std::string const empty;
  cmValue def = GetDefinition(name);
  if (!def) {
    cmSystemTools::Error(
      "Error required internal CMake variable not "
      "set, cmake may not be built correctly.\n"
      "Missing variable is:\n" +
      name);
    return empty;
  }
  return *def;
}

bool cmMakefile::IsDefinitionSet(std::string const& name) const
{
  cmValue def = m_stateSnapshot.GetDefinition(name);
  if (!def) {
    def = GetState()->GetInitializedCacheValue(name);
  }
#ifndef CMAKE_BOOTSTRAP
  if (cmVariableWatch* vv = GetVariableWatch()) {
    if (!def) {
      vv->VariableAccessed(name, cmVariableWatch::UNKNOWN_VARIABLE_DEFINED_ACCESS, nullptr, this);
    }
  }
#endif
  return def != nullptr;
}

bool cmMakefile::IsNormalDefinitionSet(std::string const& name) const
{
  cmValue def = m_stateSnapshot.GetDefinition(name);
#ifndef CMAKE_BOOTSTRAP
  if (cmVariableWatch* vv = GetVariableWatch()) {
    if (!def) {
      vv->VariableAccessed(name, cmVariableWatch::UNKNOWN_VARIABLE_DEFINED_ACCESS, nullptr, this);
    }
  }
#endif
  return def != nullptr;
}

cmValue cmMakefile::GetDefinition(std::string const& name) const
{
  cmValue def = m_stateSnapshot.GetDefinition(name);
  if (!def) {
    def = GetState()->GetInitializedCacheValue(name);
  }
#ifndef CMAKE_BOOTSTRAP
  cmVariableWatch* vv = GetVariableWatch();
  if (vv) {
    bool const watch_function_executed = vv->VariableAccessed(
      name, def ? cmVariableWatch::VARIABLE_READ_ACCESS : cmVariableWatch::UNKNOWN_VARIABLE_READ_ACCESS, def.GetCStr(),
      this);

    if (watch_function_executed) {
      // A callback was executed and may have caused re-allocation of the
      // variable storage.  Look it up again for now.
      // FIXME: Refactor variable storage to avoid this problem.
      def = m_stateSnapshot.GetDefinition(name);
      if (!def) {
        def = GetState()->GetInitializedCacheValue(name);
      }
    }
  }
#endif
  return def;
}

std::string const& cmMakefile::GetSafeDefinition(std::string const& name) const
{
  return GetDefinition(name);
}

std::vector<std::string> cmMakefile::GetDefinitions() const
{
  std::vector<std::string> res = m_stateSnapshot.ClosureKeys();
  cm::append(res, GetState()->GetCacheEntryKeys());
  std::sort(res.begin(), res.end());
  return res;
}

std::string const& cmMakefile::ExpandVariablesInString(std::string& source) const
{
  return ExpandVariablesInString(source, false, false);
}

std::string const& cmMakefile::ExpandVariablesInString(
  std::string& source,
  bool escapeQuotes,
  bool noEscapes,
  bool atOnly,
  char const* filename,
  long line,
  bool removeEmpty,
  bool replaceAt) const
{
  // Sanity check the @ONLY mode.
  if (atOnly && (!noEscapes || !removeEmpty)) {
    // This case should never be called.  At-only is for
    // configure-file/string which always does no escapes.
    IssueMessage(
      MessageType::INTERNAL_ERROR,
      "ExpandVariablesInString @ONLY called "
      "on something with escapes.");
    return source;
  }

  std::string errorstr;
  MessageType mtype =
    ExpandVariablesInStringImpl(errorstr, source, escapeQuotes, noEscapes, atOnly, filename, line, replaceAt);
  if (mtype != MessageType::LOG) {
    if (mtype == MessageType::FATAL_ERROR) {
      cmSystemTools::SetFatalErrorOccurred();
    }
    IssueMessage(mtype, errorstr);
  }

  return source;
}

enum t_domain
{
  NORMAL,
  ENVIRONMENT,
  CACHE
};

struct t_lookup
{
  t_domain domain = NORMAL;
  size_t loc = 0;
};

bool cmMakefile::IsProjectFile(char const* filename) const
{
  return cmSystemTools::IsSubDirectory(filename, GetHomeDirectory()) ||
    (cmSystemTools::IsSubDirectory(filename, GetHomeOutputDirectory()) &&
     !cmSystemTools::IsSubDirectory(filename, "/CMakeFiles"));
}

size_t cmMakefile::GetRecursionDepthLimit() const
{
  size_t depth = CMake_DEFAULT_RECURSION_LIMIT;
  if (cmValue depthStr = GetDefinition("CMAKE_MAXIMUM_RECURSION_DEPTH")) {
    unsigned long depthUL;
    if (cmStrToULong(depthStr.GetCStr(), &depthUL)) {
      depth = depthUL;
    }
  } else if (cm::optional<std::string> depthEnv = cmSystemTools::GetEnvVar("CMAKE_MAXIMUM_RECURSION_DEPTH")) {
    unsigned long depthUL;
    if (cmStrToULong(*depthEnv, &depthUL)) {
      depth = depthUL;
    }
  }
  return depth;
}

size_t cmMakefile::GetRecursionDepth() const
{
  return m_recursionDepth;
}

void cmMakefile::SetRecursionDepth(size_t recursionDepth)
{
  m_recursionDepth = recursionDepth;
}

std::string cmMakefile::NewDeferId() const
{
  return GetGlobalGenerator()->NewDeferId();
}

bool cmMakefile::DeferCall(
  std::string id,
  std::string file,
  cmListFileFunction lff)
{
  if (!m_defer) {
    return false;
  }
  m_defer->m_commands.emplace_back(DeferCommand{ std::move(id), std::move(file), std::move(lff) });
  return true;
}

bool cmMakefile::DeferCancelCall(std::string const& id)
{
  if (!m_defer) {
    return false;
  }
  for (DeferCommand& dc : m_defer->m_commands) {
    if (dc.m_id == id) {
      dc.m_id.clear();
    }
  }
  return true;
}

cm::optional<std::string> cmMakefile::DeferGetCallIds() const
{
  cm::optional<std::string> ids;
  if (m_defer) {
    ids = cmList::to_string(cmMakeRange(m_defer->m_commands)
                              .filter([](DeferCommand const& dc) -> bool { return !dc.m_id.empty(); })
                              .transform([](DeferCommand const& dc) -> std::string const& { return dc.m_id; }));
  }
  return ids;
}

cm::optional<std::string> cmMakefile::DeferGetCall(std::string const& id) const
{
  cm::optional<std::string> call;
  if (m_defer) {
    std::string tmp;
    for (DeferCommand const& dc : m_defer->m_commands) {
      if (dc.m_id == id) {
        tmp = dc.m_command.OriginalName();
        for (cmListFileArgument const& arg : dc.m_command.Arguments()) {
          tmp = cmStrCat(tmp, ';', arg.Value);
        }
        break;
      }
    }
    call = std::move(tmp);
  }
  return call;
}

MessageType cmMakefile::ExpandVariablesInStringImpl(
  std::string& errorstr,
  std::string& source,
  bool escapeQuotes,
  bool noEscapes,
  bool atOnly,
  char const* filename,
  long line,
  bool replaceAt) const
{
  // This method replaces ${VAR} and @VAR@ where VAR is looked up
  // with GetDefinition(), if not found in the map, nothing is expanded.
  // It also supports the $ENV{VAR} syntax where VAR is looked up in
  // the current environment variables.

  char const* in = source.c_str();
  char const* last = in;
  std::string result;
  result.reserve(source.size());
  std::vector<t_lookup> openstack;
  bool error = false;
  bool done = false;
  MessageType mtype = MessageType::LOG;

  cmState* state = GetCMakeInstance()->GetState();

  static std::string const lineVar = "CMAKE_CURRENT_LIST_LINE";
  do {
    char inc = *in;
    switch (inc) {
      case '}':
        if (!openstack.empty()) {
          t_lookup var = openstack.back();
          openstack.pop_back();
          result.append(last, in - last);
          std::string const& lookup = result.substr(var.loc);
          cmValue value = nullptr;
          std::string varresult;
          std::string svalue;
          switch (var.domain) {
            case NORMAL:
              if (filename && lookup == lineVar) {
                cmListFileContext const& top = m_backtrace.Top();
                if (top.DeferId) {
                  varresult = cmStrCat("DEFERRED:"_s, *top.DeferId);
                } else {
                  varresult = std::to_string(line);
                }
              } else {
                value = GetDefinition(lookup);
              }
              break;
            case ENVIRONMENT:
              if (cmSystemTools::GetEnv(lookup, svalue)) {
                value = cmValue(svalue);
              }
              break;
            case CACHE:
              value = state->GetCacheEntryValue(lookup);
              break;
          }
          // Get the string we're meant to append to.
          if (value) {
            if (escapeQuotes) {
              varresult = cmEscapeQuotes(*value);
            } else {
              varresult = *value;
            }
          } else {
            MaybeWarnUninitialized(lookup, filename);
          }
          result.replace(var.loc, result.size() - var.loc, varresult);
          // Start looking from here on out.
          last = in + 1;
        }
        break;
      case '$':
        if (!atOnly) {
          t_lookup lookup;
          char const* next = in + 1;
          char const* start = nullptr;
          char nextc = *next;
          if (nextc == '{') {
            // Looking for a variable.
            start = in + 2;
            lookup.domain = NORMAL;
          } else if (nextc == '<') {
          } else if (!nextc) {
            result.append(last, next - last);
            last = next;
          } else if (cmHasLiteralPrefix(next, "ENV{")) {
            // Looking for an environment variable.
            start = in + 5;
            lookup.domain = ENVIRONMENT;
          } else if (cmHasLiteralPrefix(next, "CACHE{")) {
            // Looking for a cache variable.
            start = in + 7;
            lookup.domain = CACHE;
          } else {
            if (m_cmNamedCurly.find(next)) {
              errorstr = "Syntax $" + std::string(next, m_cmNamedCurly.end()) +
                "{} is not supported.  Only ${}, $ENV{}, "
                "and $CACHE{} are allowed.";
              mtype = MessageType::FATAL_ERROR;
              error = true;
            }
          }
          if (start) {
            result.append(last, in - last);
            last = start;
            in = start - 1;
            lookup.loc = result.size();
            openstack.push_back(lookup);
          }
          break;
        }
        CM_FALLTHROUGH;
      case '\\':
        if (!noEscapes) {
          char const* next = in + 1;
          char nextc = *next;
          if (nextc == 't') {
            result.append(last, in - last);
            result.push_back('\t');
            last = next + 1;
          } else if (nextc == 'n') {
            result.append(last, in - last);
            result.push_back('\n');
            last = next + 1;
          } else if (nextc == 'r') {
            result.append(last, in - last);
            result.push_back('\r');
            last = next + 1;
          } else if (nextc == ';' && openstack.empty()) {
            // Handled in ExpandListArgument; pass the backslash literally.
          } else if (isalnum(nextc) || nextc == '\0') {
            errorstr += "Invalid character escape '\\";
            if (nextc) {
              errorstr += nextc;
              errorstr += "'.";
            } else {
              errorstr += "' (at end of input).";
            }
            error = true;
          } else {
            // Take what we've found so far, skipping the escape character.
            result.append(last, in - last);
            // Start tracking from the next character.
            last = in + 1;
          }
          // Skip the next character since it was escaped, but don't read past
          // the end of the string.
          if (*last) {
            ++in;
          }
        }
        break;
      case '\n':
        // Onto the next line.
        ++line;
        break;
      case '\0':
        done = true;
        break;
      case '@':
        if (replaceAt) {
          char const* nextAt = strchr(in + 1, '@');
          if (
            nextAt && nextAt != in + 1 &&
            nextAt ==
              in + 1 +
                std::strspn(
                  in + 1,
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                  "abcdefghijklmnopqrstuvwxyz"
                  "0123456789/_.+-")) {
            std::string variable(in + 1, nextAt - in - 1);

            std::string varresult;
            if (filename && variable == lineVar) {
              varresult = std::to_string(line);
            } else {
              cmValue def = GetDefinition(variable);
              if (def) {
                varresult = *def;
              } else {
                MaybeWarnUninitialized(variable, filename);
              }
            }

            if (escapeQuotes) {
              varresult = cmEscapeQuotes(varresult);
            }
            // Skip over the variable.
            result.append(last, in - last);
            result.append(varresult);
            in = nextAt;
            last = in + 1;
            break;
          }
        }
        // Failed to find a valid @ expansion; treat it as literal.
        CM_FALLTHROUGH;
      default: {
        if (
          !openstack.empty() && !(isalnum(inc) || inc == '_' || inc == '/' || inc == '.' || inc == '+' || inc == '-')) {
          errorstr += cmStrCat("Invalid character ('", inc);
          result.append(last, in - last);
          errorstr += cmStrCat("') in a variable name: '", result.substr(openstack.back().loc), '\'');
          mtype = MessageType::FATAL_ERROR;
          error = true;
        }
        break;
      }
    }
    // Look at the next character.
  } while (!error && !done && *++in);

  // Check for open variable references yet.
  if (!error && !openstack.empty()) {
    errorstr += "There is an unterminated variable reference.";
    error = true;
  }

  if (error) {
    std::string e = "Syntax error in cmake code ";
    if (filename) {
      // This filename and line number may be more specific than the
      // command context because one command invocation can have
      // arguments on multiple lines.
      e += cmStrCat("at\n  ", filename, ':', line, '\n');
    }
    errorstr = cmStrCat(e, "when parsing string\n  ", source, '\n', errorstr);
    mtype = MessageType::FATAL_ERROR;
  } else {
    // Append the rest of the unchanged part of the string.
    result.append(last);

    source = result;
  }

  return mtype;
}

void cmMakefile::RemoveVariablesInString(
  std::string& source,
  bool atOnly) const
{
  if (!atOnly) {
    cmsys::RegularExpression var("(\\${[A-Za-z_0-9]*})");
    while (var.find(source)) {
      source.erase(var.start(), var.end() - var.start());
    }
  }

  if (!atOnly) {
    cmsys::RegularExpression varb("(\\$ENV{[A-Za-z_0-9]*})");
    while (varb.find(source)) {
      source.erase(varb.start(), varb.end() - varb.start());
    }
  }
  cmsys::RegularExpression var2("(@[A-Za-z_0-9]*@)");
  while (var2.find(source)) {
    source.erase(var2.start(), var2.end() - var2.start());
  }
}

void cmMakefile::InitCMAKE_CONFIGURATION_TYPES(std::string const& genDefault)
{
  if (GetDefinition("CMAKE_CONFIGURATION_TYPES")) {
    return;
  }
  std::string initConfigs;
  if (GetCMakeInstance()->GetIsInTryCompile() || !cmSystemTools::GetEnv("CMAKE_CONFIGURATION_TYPES", initConfigs)) {
    initConfigs = genDefault;
  }
  AddCacheDefinition(
    "CMAKE_CONFIGURATION_TYPES", initConfigs,
    "Semicolon separated list of supported configuration types, "
    "only supports Debug, Release, MinSizeRel, and RelWithDebInfo, "
    "anything else will be ignored.",
    cmStateEnums::STRING);
}

std::string cmMakefile::GetDefaultConfiguration() const
{
  if (GetGlobalGenerator()->IsMultiConfig()) {
    return std::string{};
  }
  return GetSafeDefinition("CMAKE_BUILD_TYPE");
}

std::vector<std::string> cmMakefile::GetGeneratorConfigs(GeneratorConfigQuery mode) const
{
  cmList configs;
  if (GetGlobalGenerator()->IsMultiConfig()) {
    configs.assign(GetDefinition("CMAKE_CONFIGURATION_TYPES"));
  } else if (mode != cmMakefile::OnlyMultiConfig) {
    std::string const& buildType = GetSafeDefinition("CMAKE_BUILD_TYPE");
    if (!buildType.empty()) {
      configs.emplace_back(buildType);
    }
  }
  if (mode == cmMakefile::IncludeEmptyConfig && configs.empty()) {
    configs.emplace_back();
  }
  return std::move(configs.data());
}

bool cmMakefile::IsFunctionBlocked(
  cmListFileFunction const& lff,
  cmExecutionStatus& status)
{
  // if there are no blockers get out of here
  if (m_functionBlockers.empty()) {
    return false;
  }

  return m_functionBlockers.top()->IsFunctionBlocked(lff, status);
}

void cmMakefile::PushFunctionBlockerBarrier()
{
  m_functionBlockerBarriers.push_back(m_functionBlockers.size());
}

void cmMakefile::PopFunctionBlockerBarrier(bool reportError)
{
  // Remove any extra entries pushed on the barrier.
  FunctionBlockersType::size_type barrier = m_functionBlockerBarriers.back();
  while (m_functionBlockers.size() > barrier) {
    std::unique_ptr<cmFunctionBlocker> fb(std::move(m_functionBlockers.top()));
    m_functionBlockers.pop();
    if (reportError) {
      // Report the context in which the unclosed block was opened.
      cmListFileContext const& lfc = fb->GetStartingContext();
      std::ostringstream e;
      /* clang-format off */
      e << "A logical block opening on the line\n"
           "  " << lfc << "\n"
           "is not closed.";
      /* clang-format on */
      IssueMessage(MessageType::FATAL_ERROR, e.str());
      reportError = false;
    }
  }

  // Remove the barrier.
  m_functionBlockerBarriers.pop_back();
}

void cmMakefile::PushLoopBlock()
{
  assert(!m_loopBlockCounter.empty());
  m_loopBlockCounter.top()++;
}

void cmMakefile::PopLoopBlock()
{
  assert(!m_loopBlockCounter.empty());
  assert(m_loopBlockCounter.top() > 0);
  m_loopBlockCounter.top()--;
}

void cmMakefile::PushLoopBlockBarrier()
{
  m_loopBlockCounter.push(0);
}

void cmMakefile::PopLoopBlockBarrier()
{
  assert(!m_loopBlockCounter.empty());
  assert(m_loopBlockCounter.top() == 0);
  m_loopBlockCounter.pop();
}

bool cmMakefile::IsLoopBlock() const
{
  assert(!m_loopBlockCounter.empty());
  return !m_loopBlockCounter.empty() && m_loopBlockCounter.top() > 0;
}

bool cmMakefile::ExpandArguments(
  std::vector<cmListFileArgument> const& inArgs,
  std::vector<std::string>& outArgs) const
{
  std::string const& filename = GetBacktrace().Top().m_filePath;
  std::string value;
  outArgs.reserve(inArgs.size());
  for (cmListFileArgument const& i : inArgs) {
    // No expansion in a bracket argument.
    if (i.Delim == cmListFileArgument::Bracket) {
      outArgs.push_back(i.Value);
      continue;
    }
    // Expand the variables in the argument.
    value = i.Value;
    ExpandVariablesInString(value, false, false, false, filename.c_str(), i.Line, false, false);

    // If the argument is quoted, it should be one argument.
    // Otherwise, it may be a list of arguments.
    if (i.Delim == cmListFileArgument::Quoted) {
      outArgs.push_back(value);
    } else {
      cmExpandList(value, outArgs);
    }
  }
  return !cmSystemTools::GetFatalErrorOccurred();
}

bool cmMakefile::ExpandArguments(
  std::vector<cmListFileArgument> const& inArgs,
  std::vector<cmExpandedCommandArgument>& outArgs) const
{
  std::string const& filename = GetBacktrace().Top().m_filePath;
  std::string value;
  outArgs.reserve(inArgs.size());
  for (cmListFileArgument const& i : inArgs) {
    // No expansion in a bracket argument.
    if (i.Delim == cmListFileArgument::Bracket) {
      outArgs.emplace_back(i.Value, true);
      continue;
    }
    // Expand the variables in the argument.
    value = i.Value;
    ExpandVariablesInString(value, false, false, false, filename.c_str(), i.Line, false, false);

    // If the argument is quoted, it should be one argument.
    // Otherwise, it may be a list of arguments.
    if (i.Delim == cmListFileArgument::Quoted) {
      outArgs.emplace_back(value, true);
    } else {
      cmList stringArgs{ value };
      for (std::string const& stringArg : stringArgs) {
        outArgs.emplace_back(stringArg, false);
      }
    }
  }
  return !cmSystemTools::GetFatalErrorOccurred();
}

void cmMakefile::AddFunctionBlocker(std::unique_ptr<cmFunctionBlocker> fb)
{
  if (!m_executionStatusStack.empty()) {
    // Record the context in which the blocker is created.
    fb->SetStartingContext(m_backtrace.Top());
  }

  m_functionBlockers.push(std::move(fb));
}

std::unique_ptr<cmFunctionBlocker> cmMakefile::RemoveFunctionBlocker()
{
  assert(!m_functionBlockers.empty());
  assert(m_functionBlockerBarriers.empty() || m_functionBlockers.size() > m_functionBlockerBarriers.back());

  auto b = std::move(m_functionBlockers.top());
  m_functionBlockers.pop();
  return b;
}

std::string const& cmMakefile::GetHomeDirectory() const
{
  return GetCMakeInstance()->GetHomeDirectory();
}

std::string const& cmMakefile::GetHomeOutputDirectory() const
{
  return GetCMakeInstance()->GetHomeOutputDirectory();
}

void cmMakefile::SetScriptModeFile(std::string const& scriptfile)
{
  AddDefinition("CMAKE_SCRIPT_MODE_FILE", scriptfile);
}

void cmMakefile::SetArgcArgv(std::vector<std::string> const& args)
{
  AddDefinition("CMAKE_ARGC", std::to_string(args.size()));

  for (auto i = 0u; i < args.size(); ++i) {
    AddDefinition(cmStrCat("CMAKE_ARGV", i), args[i]);
  }
}

cmSourceFile* cmMakefile::GetSource(
  std::string const& sourceName,
  cmSourceFileLocationKind kind) const
{
  FunctionTrace f(__func__, sourceName);

  // First check "Known" paths (avoids the creation of cmSourceFileLocation)
  if (kind == cmSourceFileLocationKind::Known) {
    auto sfsi = m_knownFileSearchIndex.find(sourceName);
    if (sfsi != m_knownFileSearchIndex.end()) {
      return sfsi->second;
    }
  }

  cmSourceFileLocation sfl(this, sourceName, kind);
  auto name = GetCMakeInstance()->StripExtension(sfl.GetName());
#if defined(_WIN32) || defined(__APPLE__)
  name = cmSystemTools::LowerCase(name);
#endif
  auto sfsi = m_sourceFileSearchIndex.find(name);
  if (sfsi != m_sourceFileSearchIndex.end()) {
    for (auto* sf : sfsi->second) {
      if (sf->Matches(sfl)) {
        return sf;
      }
    }
  }
  return nullptr;
}

cmSourceFile* cmMakefile::CreateSource(
  std::string const& sourceName,
  bool generated,
  cmSourceFileLocationKind kind)
{
  FunctionTrace f(__func__, sourceName);
  auto sf = cm::make_unique<cmSourceFile>(this, sourceName, generated, kind);
  auto name = GetCMakeInstance()->StripExtension(sf->GetLocation().GetName());
#if defined(_WIN32) || defined(__APPLE__)
  name = cmSystemTools::LowerCase(name);
#endif
  m_sourceFileSearchIndex[name].push_back(sf.get());
  // for "Known" paths add direct lookup (used for faster lookup in GetSource)
  if (kind == cmSourceFileLocationKind::Known) {
    m_knownFileSearchIndex[sourceName] = sf.get();
  }

  m_sourceFiles.push_back(std::move(sf));

  return m_sourceFiles.back().get();
}

cmSourceFile* cmMakefile::GetOrCreateSource(
  std::string const& sourceName,
  bool generated,
  cmSourceFileLocationKind kind)
{
  FunctionTrace f(__func__, sourceName);

  if (cmSourceFile* esf = GetSource(sourceName, kind)) {
    return esf;
  }
  return CreateSource(sourceName, generated, kind);
}

cmSourceFile* cmMakefile::GetOrCreateGeneratedSource(std::string const& sourceName)
{
  FunctionTrace f(__func__, sourceName);

  cmSourceFile* sf = GetOrCreateSource(sourceName, true, cmSourceFileLocationKind::Known);
  sf->MarkAsGenerated(); // In case we did not create the source file.
  return sf;
}

void cmMakefile::CreateGeneratedOutputs(std::vector<std::string> const& outputs)
{
  FunctionTrace f(__func__);

  for (std::string const& o : outputs) {
    if (cmGeneratorExpression::Find(o) == std::string::npos) {
      GetOrCreateGeneratedSource(o);
    }
  }
}

void cmMakefile::AddTargetObject(
  std::string const& tgtName,
  std::string const& objFile)
{
  cmSourceFile* sf = GetOrCreateSource(objFile, true, cmSourceFileLocationKind::Known);
  sf->SetObjectLibrary(tgtName);
  sf->SetProperty("EXTERNAL_OBJECT", "1");
  // TODO: Compute a language for this object based on the associated source
  // file that compiles to it. Needs a policy as it likely affects link
  // language selection if done unconditionally.
#if !defined(CMAKE_BOOTSTRAP)
  m_sourceGroups[m_objectLibrariesSourceGroupIndex].AddGroupFile(sf->ResolveFullPath());
#endif
}

void cmMakefile::EnableLanguage(
  std::vector<std::string> const& languages,
  bool optional)
{
  if (m_deferRunning) {
    IssueMessage(MessageType::FATAL_ERROR, "Languages may not be enabled during deferred execution.");
    return;
  }
  if (char const* def = GetGlobalGenerator()->GetCMakeCFGIntDir()) {
    AddDefinition("CMAKE_CFG_INTDIR", def);
  }

  std::vector<std::string> unique_languages;
  {
    std::vector<std::string> duplicate_languages;
    for (std::string const& language : languages) {
      if (!cm::contains(unique_languages, language)) {
        unique_languages.push_back(language);
      } else if (!cm::contains(duplicate_languages, language)) {
        duplicate_languages.push_back(language);
      }
    }
    if (!duplicate_languages.empty()) {
      auto quantity = duplicate_languages.size() == 1 ? " has"_s : "s have"_s;
      IssueMessage(
        MessageType::AUTHOR_WARNING,
        cmStrCat(
          "Languages to be enabled may not be specified more "
          "than once at the same time. The following language",
          quantity, " been specified multiple times: ", cmJoin(duplicate_languages, ", ")));
    }
  }

  // If RC is explicitly listed we need to do it after other languages.
  // On some platforms we enable RC implicitly while enabling others.
  // Do not let that look like recursive enable_language(RC).
  std::vector<std::string> languages_without_RC;
  std::vector<std::string> languages_for_RC;
  languages_without_RC.reserve(unique_languages.size());
  for (std::string const& language : unique_languages) {
    if (language == "RC"_s) {
      languages_for_RC.push_back(language);
    } else {
      languages_without_RC.push_back(language);
    }
  }
  if (!languages_without_RC.empty()) {
    GetGlobalGenerator()->EnableLanguage(languages_without_RC, this, optional);
  }
  if (!languages_for_RC.empty()) {
    GetGlobalGenerator()->EnableLanguage(languages_for_RC, this, optional);
  }
}

int cmMakefile::TryCompile(
  std::string const& srcdir,
  std::string const& bindir,
  std::string const& projectName,
  std::string const& targetName,
  bool fast,
  int jobs,
  std::vector<std::string> const* cmakeArgs,
  std::string& output)
{
  m_isSourceFileTryCompile = fast;
  // does the binary directory exist ? If not create it...
  if (!cmSystemTools::FileIsDirectory(bindir)) {
    cmSystemTools::MakeDirectory(bindir);
  }

  // change to the tests directory and run cmake
  // use the cmake object instead of calling cmake
  cmWorkingDirectory workdir(bindir);
  if (workdir.Failed()) {
    IssueMessage(MessageType::FATAL_ERROR, workdir.GetError());
    cmSystemTools::SetFatalErrorOccurred();
    m_isSourceFileTryCompile = false;
    return 1;
  }

  // make sure the same generator is used
  // use this program as the cmake to be run, it should not
  // be run that way but the cmake object requires a valid path
  CMake cm(CMake::RoleProject, cmState::Project, cmState::ProjectKind::TryCompile);
  auto gg = cm.CreateGlobalGenerator(GetGlobalGenerator()->GetName());
  if (!gg) {
    IssueMessage(
      MessageType::INTERNAL_ERROR, "Global generator '" + GetGlobalGenerator()->GetName() + "' could not be created.");
    cmSystemTools::SetFatalErrorOccurred();
    m_isSourceFileTryCompile = false;
    return 1;
  }
  gg->m_recursionDepth = m_recursionDepth;
  cm.SetGlobalGenerator(std::move(gg));

  // copy trace state
  cm.SetTraceRedirect(GetCMakeInstance());

  // do a configure
  cm.SetHomeDirectory(srcdir);
  cm.SetHomeOutputDirectory(bindir);
  cm.SetGeneratorInstance(GetSafeDefinition("CMAKE_GENERATOR_INSTANCE"));
  cm.SetGeneratorPlatform(GetSafeDefinition("CMAKE_GENERATOR_PLATFORM"));
  cm.SetGeneratorToolset(GetSafeDefinition("CMAKE_GENERATOR_TOOLSET"));
  cm.LoadCache();
  if (!cm.GetGlobalGenerator()->IsMultiConfig()) {
    if (cmValue config = GetDefinition("CMAKE_TRY_COMPILE_CONFIGURATION")) {
      // Tell the single-configuration generator which one to use.
      // Add this before the user-provided CMake arguments in case
      // one of the arguments is -DCMAKE_BUILD_TYPE=...
      cm.AddCacheEntry("CMAKE_BUILD_TYPE", config, "Build configuration", cmStateEnums::STRING);
    }
  }
  cmValue recursionDepth = GetDefinition("CMAKE_MAXIMUM_RECURSION_DEPTH");
  if (recursionDepth) {
    cm.AddCacheEntry("CMAKE_MAXIMUM_RECURSION_DEPTH", recursionDepth, "Maximum recursion depth", cmStateEnums::STRING);
  }
  // if cmake args were provided then pass them in
  if (cmakeArgs) {
    // FIXME: Workaround to ignore unused CLI variables in try-compile.
    //
    // Ideally we should use SetArgs for options like --no-warn-unused-cli.
    // However, there is a subtle problem when certain arguments are passed to
    // a macro wrapping around try_compile or try_run that does not escape
    // semicolons in its parameters but just passes ${ARGV} or ${ARGN}.  In
    // this case a list argument like "-DVAR=a;b" gets split into multiple
    // cmake arguments "-DVAR=a" and "b".  Currently SetCacheArgs ignores
    // argument "b" and uses just "-DVAR=a", leading to a subtle bug in that
    // the try_compile or try_run does not get the proper value of VAR.  If we
    // call SetArgs here then it would treat "b" as the source directory and
    // cause an error such as "The source directory .../CMakeFiles/CMakeTmp/b
    // does not exist", thus breaking the try_compile or try_run completely.
    //
    // Strictly speaking the bug is in the wrapper macro because the CMake
    // language has always flattened nested lists and the macro should escape
    // the semicolons in its arguments before forwarding them.  However, this
    // bug is so subtle that projects typically work anyway, usually because
    // the value VAR=a is sufficient for the try_compile or try_run to get the
    // correct result.  Calling SetArgs here would break such projects that
    // previously built.  Instead we work around the issue by never reporting
    // unused arguments and ignoring options such as --no-warn-unused-cli.
    cm.SetWarnUnusedCli(false);
    // cm.SetArgs(*cmakeArgs, true);

    cm.SetCacheArgs(*cmakeArgs);
  }
  // to save time we pass the EnableLanguage info directly
  cm.GetGlobalGenerator()->EnableLanguagesFromGenerator(GetGlobalGenerator(), this);
  if (IsOn("CMAKE_SUPPRESS_DEVELOPER_WARNINGS")) {
    cm.AddCacheEntry("CMAKE_SUPPRESS_DEVELOPER_WARNINGS", "TRUE", "", cmStateEnums::INTERNAL);
  } else {
    cm.AddCacheEntry("CMAKE_SUPPRESS_DEVELOPER_WARNINGS", "FALSE", "", cmStateEnums::INTERNAL);
  }
  if (cm.Configure() != 0) {
    IssueMessage(MessageType::FATAL_ERROR, "Failed to configure test project build system.");
    cmSystemTools::SetFatalErrorOccurred();
    m_isSourceFileTryCompile = false;
    return 1;
  }

  if (cm.Generate() != 0) {
    IssueMessage(MessageType::FATAL_ERROR, "Failed to generate test project build system.");
    cmSystemTools::SetFatalErrorOccurred();
    m_isSourceFileTryCompile = false;
    return 1;
  }

  // finally call the generator to actually build the resulting project
  int ret = GetGlobalGenerator()->TryCompile(jobs, srcdir, bindir, projectName, targetName, fast, output, this);

  m_isSourceFileTryCompile = false;
  return ret;
}

bool cmMakefile::GetIsSourceFileTryCompile() const
{
  return m_isSourceFileTryCompile;
}

CMake* cmMakefile::GetCMakeInstance() const
{
  return m_pGlobalGenerator->GetCMakeInstance();
}

cmMessenger* cmMakefile::GetMessenger() const
{
  return GetCMakeInstance()->GetMessenger();
}

cmGlobalGenerator* cmMakefile::GetGlobalGenerator() const
{
  return m_pGlobalGenerator;
}

#ifndef CMAKE_BOOTSTRAP
cmVariableWatch* cmMakefile::GetVariableWatch() const
{
  if (GetCMakeInstance() && GetCMakeInstance()->GetVariableWatch()) {
    return GetCMakeInstance()->GetVariableWatch();
  }
  return nullptr;
}
#endif

cmState* cmMakefile::GetState() const
{
  return GetCMakeInstance()->GetState();
}

void cmMakefile::DisplayStatus(
  std::string const& message,
  float s) const
{
  CMake* cm = GetCMakeInstance();
  if (cm->GetWorkingMode() == CMake::FIND_PACKAGE_MODE) {
    // don't output any STATUS message in FIND_PACKAGE_MODE, since they will
    // directly be fed to the compiler, which will be confused.
    return;
  }
  cm->UpdateProgress(message, s);

#ifdef CMake_ENABLE_DEBUGGER
  if (cm->GetDebugAdapter()) {
    cm->GetDebugAdapter()->OnMessageOutput(MessageType::MESSAGE, message);
  }
#endif
}

std::string cmMakefile::GetModulesFile(
  cm::string_view filename,
  bool& system,
  bool debug,
  std::string& debugBuffer) const
{
  std::string result;

  std::string moduleInCMakeRoot;
  std::string moduleInCMakeModulePath;

  // Always search in CMAKE_MODULE_PATH:
  cmValue cmakeModulePath = GetDefinition("CMAKE_MODULE_PATH");
  if (cmakeModulePath) {
    cmList modulePath{ *cmakeModulePath };

    // Look through the possible module directories.
    for (std::string itempl : modulePath) {
      cmSystemTools::ConvertToUnixSlashes(itempl);
      itempl += "/";
      itempl += filename;
      if (cmSystemTools::FileExists(itempl)) {
        moduleInCMakeModulePath = itempl;
        break;
      }
      if (debug) {
        debugBuffer = cmStrCat(debugBuffer, "  ", itempl, '\n');
      }
    }
  }

  // Always search in the standard modules location.
  moduleInCMakeRoot = cmStrCat(cmSystemTools::GetCMakeRoot(), "/Modules/", filename);
  cmSystemTools::ConvertToUnixSlashes(moduleInCMakeRoot);
  if (!cmSystemTools::FileExists(moduleInCMakeRoot)) {
    if (debug) {
      debugBuffer = cmStrCat(debugBuffer, "  ", moduleInCMakeRoot, '\n');
    }
    moduleInCMakeRoot.clear();
  }

  // Normally, prefer the files found in CMAKE_MODULE_PATH. Only when the file
  // from which we are being called is located itself in CMAKE_ROOT, then
  // prefer results from CMAKE_ROOT depending on the policy setting.
  if (!moduleInCMakeModulePath.empty() && !moduleInCMakeRoot.empty()) {
    cmValue currentFile = GetDefinition(kCMAKE_CURRENT_LIST_FILE);
    std::string mods = cmStrCat(cmSystemTools::GetCMakeRoot(), "/Modules/");
    if (currentFile && cmSystemTools::IsSubDirectory(*currentFile, mods)) {
      system = true;
      result = moduleInCMakeRoot;
    } else {
      system = false;
      result = moduleInCMakeModulePath;
    }
  } else if (!moduleInCMakeModulePath.empty()) {
    system = false;
    result = moduleInCMakeModulePath;
  } else {
    system = true;
    result = moduleInCMakeRoot;
  }

  return result;
}

void cmMakefile::ConfigureString(
  std::string const& input,
  std::string& output,
  bool atOnly,
  bool escapeQuotes) const
{
  // Split input to handle one line at a time.
  std::string::const_iterator lineStart = input.begin();
  while (lineStart != input.end()) {
    // Find the end of this line.
    std::string::const_iterator lineEnd = lineStart;
    while (lineEnd != input.end() && *lineEnd != '\n') {
      ++lineEnd;
    }

    // Copy the line.
    std::string line(lineStart, lineEnd);

    // Skip the newline character.
    bool haveNewline = (lineEnd != input.end());
    if (haveNewline) {
      ++lineEnd;
    }

    // Replace #cmakedefine instances.
    if (m_cmDefineRegex.find(line)) {
      cmValue def = GetDefinition(m_cmDefineRegex.match(2));
      if (!def.IsOff()) {
        std::string const indentation = m_cmDefineRegex.match(1);
        cmSystemTools::ReplaceString(
          line, cmStrCat('#', indentation, "cmakedefine"), cmStrCat('#', indentation, "define"));
        output += line;
      } else {
        output += "/* #undef ";
        output += m_cmDefineRegex.match(2);
        output += " */";
      }
    } else if (m_cmDefine01Regex.find(line)) {
      std::string const indentation = m_cmDefine01Regex.match(1);
      cmValue def = GetDefinition(m_cmDefine01Regex.match(2));
      cmSystemTools::ReplaceString(
        line, cmStrCat('#', indentation, "cmakedefine01"), cmStrCat('#', indentation, "define"));
      output += line;
      if (!def.IsOff()) {
        output += " 1";
      } else {
        output += " 0";
      }
    } else {
      output += line;
    }

    if (haveNewline) {
      output += '\n';
    }

    // Move to the next line.
    lineStart = lineEnd;
  }

  // Perform variable replacements.
  char const* filename = nullptr;
  long lineNumber = -1;
  if (!m_backtrace.Empty()) {
    auto const& currentTrace = m_backtrace.Top();
    filename = currentTrace.m_filePath.c_str();
    lineNumber = currentTrace.Line;
  }
  ExpandVariablesInString(output, escapeQuotes, true, atOnly, filename, lineNumber, true, true);
}

int cmMakefile::ConfigureFile(
  std::string const& infile,
  std::string const& outfile,
  bool copyonly,
  bool atOnly,
  bool escapeQuotes,
  mode_t permissions,
  cmNewLineStyle newLine)
{
  int res = 1;
  if (!CanIWriteThisFile(outfile)) {
    cmSystemTools::Error(cmStrCat("Attempt to write file: ", outfile, " into a source directory."));
    return 0;
  }
  if (!cmSystemTools::FileExists(infile)) {
    cmSystemTools::Error(cmStrCat("File ", infile, " does not exist."));
    return 0;
  }
  std::string soutfile = outfile;
  std::string const& sinfile = infile;
  AddCMakeDependFile(sinfile);
  cmSystemTools::ConvertToUnixSlashes(soutfile);

  // Re-generate if non-temporary outputs are missing.
  // when we finalize the configuration we will remove all
  // output files that now don't exist.
  AddCMakeOutputFile(soutfile);

  if (permissions == 0) {
    cmSystemTools::GetPermissions(sinfile, permissions);
  }

  std::string::size_type pos = soutfile.rfind('/');
  if (pos != std::string::npos) {
    std::string path = soutfile.substr(0, pos);
    cmSystemTools::MakeDirectory(path);
  }

  if (copyonly) {
    auto const copy_status = cmSystemTools::CopyFileIfDifferent(sinfile, soutfile);
    if (!copy_status) {
      IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat(
          "Fail to copy ", copy_status.Path == cmsys::SystemTools::CopyStatus::SourcePath ? "source" : "destination",
          "file: ", copy_status.GetString()));
      res = 0;
    } else {
      auto const status = cmSystemTools::SetPermissions(soutfile, permissions);
      if (!status) {
        IssueMessage(MessageType::FATAL_ERROR, status.GetString());
        res = 0;
      }
    }
    return res;
  }

  std::string newLineCharacters;
  std::ios::openmode omode = std::ios::out | std::ios::trunc;
  if (newLine.IsValid()) {
    newLineCharacters = newLine.GetCharacters();
    omode |= std::ios::binary;
  } else {
    newLineCharacters = "\n";
  }
  std::string tempOutputFile = cmStrCat(soutfile, ".tmp");
  cmsys::ofstream fout(tempOutputFile.c_str(), omode);
  if (!fout) {
    cmSystemTools::Error("Could not open file for write in copy operation " + tempOutputFile);
    cmSystemTools::ReportLastSystemError("");
    return 0;
  }
  cmsys::ifstream fin(sinfile.c_str());
  if (!fin) {
    cmSystemTools::Error("Could not open file for read in copy operation " + sinfile);
    return 0;
  }

  cmsys::FStream::BOM bom = cmsys::FStream::ReadBOM(fin);
  if (bom != cmsys::FStream::BOM_None && bom != cmsys::FStream::BOM_UTF8) {
    IssueMessage(
      MessageType::FATAL_ERROR, cmStrCat("File starts with a Byte-Order-Mark that is not UTF-8:\n  ", sinfile));
    return 0;
  }
  // rewind to copy BOM to output file
  fin.seekg(0);

  // now copy input to output and expand variables in the
  // input file at the same time
  std::string inLine;
  std::string outLine;
  while (cmSystemTools::GetLineFromStream(fin, inLine)) {
    outLine.clear();
    ConfigureString(inLine, outLine, atOnly, escapeQuotes);
    fout << outLine << newLineCharacters;
  }
  // close the files before attempting to copy
  fin.close();
  fout.close();

  auto status = cmSystemTools::MoveFileIfDifferent(tempOutputFile, soutfile);
  if (!status) {
    IssueMessage(MessageType::FATAL_ERROR, status.GetString());
    res = 0;
  } else {
    status = cmSystemTools::SetPermissions(soutfile, permissions);
    if (!status) {
      IssueMessage(MessageType::FATAL_ERROR, status.GetString());
      res = 0;
    }
  }

  return res;
}

void cmMakefile::SetProperty(
  std::string const& prop,
  cmValue value)
{
  m_stateSnapshot.GetDirectory().SetProperty(prop, value, m_backtrace);
}

void cmMakefile::AppendProperty(
  std::string const& prop,
  std::string const& value,
  bool asString)
{
  m_stateSnapshot.GetDirectory().AppendProperty(prop, value, asString, m_backtrace);
}

cmValue cmMakefile::GetProperty(std::string const& prop) const
{
  // Check for computed properties.
  static std::string output;
  if (prop == "TESTS"_s) {
    std::vector<std::string> keys;
    // get list of keys
    auto const* t = this;
    std::transform(
      t->m_tests.begin(), t->m_tests.end(), std::back_inserter(keys),
      [](decltype(t->m_tests)::value_type const& pair) { return pair.first; });
    output = cmList::to_string(keys);
    return cmValue(output);
  }

  return m_stateSnapshot.GetDirectory().GetProperty(prop);
}

cmValue cmMakefile::GetProperty(
  std::string const& prop,
  bool chain) const
{
  return m_stateSnapshot.GetDirectory().GetProperty(prop, chain);
}

bool cmMakefile::GetPropertyAsBool(std::string const& prop) const
{
  return GetProperty(prop).IsOn();
}

std::vector<std::string> cmMakefile::GetPropertyKeys() const
{
  return m_stateSnapshot.GetDirectory().GetPropertyKeys();
}

cmTarget* cmMakefile::FindLocalNonAliasTarget(std::string const& name) const
{
  auto i = m_targets.find(name);
  if (i != m_targets.end()) {
    return &i->second;
  }
  return nullptr;
}

cmTest* cmMakefile::CreateTest(std::string const& testName)
{
  cmTest* test = GetTest(testName);
  if (test) {
    return test;
  }
  auto newTest = cm::make_unique<cmTest>(this);
  test = newTest.get();
  newTest->SetName(testName);
  m_tests[testName] = std::move(newTest);
  return test;
}

cmTest* cmMakefile::GetTest(std::string const& testName) const
{
  auto mi = m_tests.find(testName);
  if (mi != m_tests.end()) {
    return mi->second.get();
  }
  return nullptr;
}

void cmMakefile::GetTests(
  std::string const& config,
  std::vector<cmTest*>& tests) const
{
  for (auto const& generator : GetTestGenerators()) {
    if (generator->TestsForConfig(config)) {
      tests.push_back(generator->GetTest());
    }
  }
}

void cmMakefile::AddCMakeDependFilesFromUser()
{
  cmList deps;
  if (cmValue deps_str = GetProperty("CMAKE_CONFIGURE_DEPENDS")) {
    deps.assign(*deps_str);
  }
  for (auto const& dep : deps) {
    if (cmSystemTools::FileIsFullPath(dep)) {
      AddCMakeDependFile(dep);
    } else {
      std::string f = cmStrCat(GetCurrentSourceDirectory(), '/', dep);
      AddCMakeDependFile(f);
    }
  }
}

std::string cmMakefile::FormatListFileStack() const
{
  std::vector<std::string> listFiles;
  for (auto snp = m_stateSnapshot; snp.IsValid(); snp = snp.GetCallStackParent()) {
    listFiles.emplace_back(snp.GetExecutionListFile());
  }

  if (listFiles.empty()) {
    return {};
  }

  auto depth = 1;
  std::transform(listFiles.begin(), listFiles.end(), listFiles.begin(), [&depth](std::string const& file) {
    return cmStrCat('[', depth++, "]\t", file);
  });

  return cmJoinStrings(cmMakeRange(listFiles.rbegin(), listFiles.rend()), "\n                "_s, {});
}

void cmMakefile::PushScope()
{
  m_stateSnapshot = GetState()->CreateVariableScopeSnapshot(m_stateSnapshot);
  PushLoopBlockBarrier();

#if !defined(CMAKE_BOOTSTRAP)
  GetGlobalGenerator()->GetFileLockPool().PushFunctionScope();
#endif
}

void cmMakefile::PopScope()
{
#if !defined(CMAKE_BOOTSTRAP)
  GetGlobalGenerator()->GetFileLockPool().PopFunctionScope();
#endif

  PopLoopBlockBarrier();

  PopSnapshot();
}

void cmMakefile::RaiseScope(
  std::string const& var,
  char const* varDef)
{
  if (var.empty()) {
    return;
  }

  if (!m_stateSnapshot.RaiseScope(var, varDef)) {
    IssueMessage(MessageType::AUTHOR_WARNING, cmStrCat("Cannot set \"", var, "\": current scope has no parent."));
    return;
  }

#ifndef CMAKE_BOOTSTRAP
  cmVariableWatch* vv = GetVariableWatch();
  if (vv) {
    vv->VariableAccessed(var, cmVariableWatch::VARIABLE_MODIFIED_ACCESS, varDef, this);
  }
#endif
}

void cmMakefile::RaiseScope(std::vector<std::string> const& variables)
{
  for (auto const& varName : variables) {
    if (IsNormalDefinitionSet(varName)) {
      RaiseScope(varName, GetDefinition(varName));
    } else {
      // unset variable in parent scope
      RaiseScope(varName, nullptr);
    }
  }
}

cmTarget* cmMakefile::AddImportedTarget(
  std::string const& name,
  cmStateEnums::TargetType type,
  bool global)
{
  // Create the target.
  std::unique_ptr<cmTarget> target(new cmTarget(
    name, type, global ? cmTarget::Visibility::ImportedGlobally : cmTarget::Visibility::Imported, this,
    cmTarget::PerConfig::Yes));

  // Add to the set of available imported targets.
  m_importedTargets[name] = target.get();
  GetGlobalGenerator()->IndexTarget(target.get());
  GetStateSnapshot().GetDirectory().AddImportedTargetName(name);

  // Transfer ownership to this cmMakefile object.
  m_importedTargetsOwned.push_back(std::move(target));
  return m_importedTargetsOwned.back().get();
}

cmTarget* cmMakefile::AddForeignTarget(
  std::string const& origin,
  std::string const& name)
{
  auto foreign_name = cmStrCat("@foreign_", origin, "::", name);
  std::unique_ptr<cmTarget> target(new cmTarget(
    foreign_name, cmStateEnums::TargetType::INTERFACE_LIBRARY, cmTarget::Visibility::Foreign, this,
    cmTarget::PerConfig::Yes));

  m_importedTargets[foreign_name] = target.get();
  GetGlobalGenerator()->IndexTarget(target.get());
  GetStateSnapshot().GetDirectory().AddImportedTargetName(foreign_name);

  m_importedTargetsOwned.push_back(std::move(target));
  return m_importedTargetsOwned.back().get();
}

cmTarget* cmMakefile::FindTargetToUse(
  std::string const& name,
  cmStateEnums::TargetDomainSet domains) const
{
  // Look for an imported target.  These take priority because they
  // are more local in scope and do not have to be globally unique.
  auto targetName = name;
  if (domains.contains(cmStateEnums::TargetDomain::ALIAS)) {
    // Look for local alias targets.
    auto alias = m_aliasTargets.find(name);
    if (alias != m_aliasTargets.end()) {
      targetName = alias->second;
    }
  }
  auto const imported = m_importedTargets.find(targetName);

  bool const useForeign = domains.contains(cmStateEnums::TargetDomain::FOREIGN);
  bool const useNative = domains.contains(cmStateEnums::TargetDomain::NATIVE);

  if (imported != m_importedTargets.end()) {
    if (imported->second->IsForeign() ? useForeign : useNative) {
      return imported->second;
    }
  }

  // Look for a target built in this directory.
  if (cmTarget* t = FindLocalNonAliasTarget(name)) {
    if (t->IsForeign() ? useForeign : useNative) {
      return t;
    }
  }

  // Look for a target built in this project.
  return GetGlobalGenerator()->FindTarget(name, domains);
}

bool cmMakefile::IsAlias(std::string const& name) const
{
  if (cm::contains(m_aliasTargets, name)) {
    return true;
  }
  return GetGlobalGenerator()->IsAlias(name);
}

bool cmMakefile::EnforceUniqueName(
  std::string const& name,
  std::string& msg,
  bool isCustom) const
{
  if (IsAlias(name)) {
    msg = cmStrCat("cannot create target \"", name, "\" because an alias with the same name already exists.");
    return false;
  }
  if (cmTarget* existing = FindTargetToUse(name)) {
    // The name given conflicts with an existing target.  Produce an
    // error in a compatible way.
    if (existing->IsImported()) {
      // Imported targets were not supported in previous versions.
      // This is new code, so we can make it an error.
      msg =
        cmStrCat("cannot create target \"", name, "\" because an imported target with the same name already exists.");
      return false;
    }

    // The conflict is with a non-imported target.
    // Allow this if the user has requested support.
    CMake* cm = GetCMakeInstance();
    if (
      isCustom && existing->GetType() == cmStateEnums::UTILITY && this != existing->GetMakefile() &&
      cm->GetState()->GetGlobalPropertyAsBool("ALLOW_DUPLICATE_CUSTOM_TARGETS")) {
      return true;
    }

    // Produce an error that tells the user how to work around the
    // problem.
    std::ostringstream e;
    e << "cannot create target \"" << name
      << "\" because another target with the same name already exists.  "
         "The existing target is ";
    switch (existing->GetType()) {
      case cmStateEnums::EXECUTABLE:
        e << "an executable ";
        break;
      case cmStateEnums::STATIC_LIBRARY:
        e << "a static library ";
        break;
      case cmStateEnums::SHARED_LIBRARY:
        e << "a shared library ";
        break;
      case cmStateEnums::MODULE_LIBRARY:
        e << "a module library ";
        break;
      case cmStateEnums::UTILITY:
        e << "a custom target ";
        break;
      case cmStateEnums::INTERFACE_LIBRARY:
        e << "an interface library ";
        break;
      default:
        break;
    }
    e << "created in source directory \"" << existing->GetMakefile()->GetCurrentSourceDirectory()
      << "\".  "
         "See documentation for policy CMP0002 for more details.";
    msg = e.str();
    return false;
  }
  return true;
}

bool cmMakefile::EnforceUniqueDir(
  std::string const& srcPath,
  std::string const& binPath) const
{
  // Make sure the binary directory is unique.
  cmGlobalGenerator* gg = GetGlobalGenerator();
  if (gg->BinaryDirectoryIsNew(binPath)) {
    return true;
  }
  IssueMessage(
    MessageType::FATAL_ERROR,
    cmStrCat(
      "The binary directory\n"
      "  ",
      binPath,
      "\n"
      "is already used to build a source directory.  "
      "It cannot be used to build source directory\n"
      "  ",
      srcPath,
      "\n"
      "Specify a unique binary directory name."));

  return false;
}

static std::string const matchVariables[] = { "CMAKE_MATCH_0", "CMAKE_MATCH_1", "CMAKE_MATCH_2", "CMAKE_MATCH_3",
                                              "CMAKE_MATCH_4", "CMAKE_MATCH_5", "CMAKE_MATCH_6", "CMAKE_MATCH_7",
                                              "CMAKE_MATCH_8", "CMAKE_MATCH_9" };

static std::string const nMatchesVariable = "CMAKE_MATCH_COUNT";

void cmMakefile::ClearMatches()
{
  cmValue nMatchesStr = GetDefinition(nMatchesVariable);
  if (!nMatchesStr) {
    return;
  }
  int nMatches = atoi(nMatchesStr->c_str());
  for (int i = 0; i <= nMatches; i++) {
    std::string const& var = matchVariables[i];
    std::string const& s = GetSafeDefinition(var);
    if (!s.empty()) {
      AddDefinition(var, "");
      MarkVariableAsUsed(var);
    }
  }
  AddDefinition(nMatchesVariable, "0");
  MarkVariableAsUsed(nMatchesVariable);
}

void cmMakefile::StoreMatches(cmsys::RegularExpression& re)
{
  char highest = 0;
  for (int i = 0; i < 10; i++) {
    std::string const& m = re.match(i);
    if (!m.empty()) {
      std::string const& var = matchVariables[i];
      AddDefinition(var, m);
      MarkVariableAsUsed(var);
      highest = static_cast<char>('0' + i);
    }
  }
  char nMatches[] = { highest, '\0' };
  AddDefinition(nMatchesVariable, nMatches);
  MarkVariableAsUsed(nMatchesVariable);
}

cmStateSnapshot cmMakefile::GetStateSnapshot() const
{
  return m_stateSnapshot;
}

cmPolicies::PolicyStatus cmMakefile::GetPolicyStatus(
  cmPolicies::PolicyID id,
  bool parent_scope) const
{
  return m_stateSnapshot.GetPolicy(id, parent_scope);
}

bool cmMakefile::PolicyOptionalWarningEnabled(std::string const& var) const
{
  // Check for an explicit CMAKE_POLICY_WARNING_CMP<NNNN> setting.
  if (cmValue val = GetDefinition(var)) {
    return val.IsOn();
  }
  // Enable optional policy warnings with --debug-output, --trace,
  // or --trace-expand.
  CMake* cm = GetCMakeInstance();
  return cm->GetDebugOutput() || cm->GetTrace();
}

bool cmMakefile::SetPolicy(
  char const* id,
  cmPolicies::PolicyStatus status)
{
  cmPolicies::PolicyID pid;
  if (!cmPolicies::GetPolicyID(id, /* out */ pid)) {
    IssueMessage(MessageType::FATAL_ERROR, cmStrCat("Policy \"", id, "\" is not known to this version of CMake."));
    return false;
  }
  return SetPolicy(pid, status);
}

bool cmMakefile::SetPolicy(
  cmPolicies::PolicyID id,
  cmPolicies::PolicyStatus status)
{
  // A removed policy may be set only to NEW.
  if (cmPolicies::IsRemoved(id) && status != cmPolicies::NEW) {
    std::string msg = cmPolicies::GetRemovedPolicyError(id);
    IssueMessage(MessageType::FATAL_ERROR, msg);
    return false;
  }

  // Deprecate old policies.
  if (
    status == cmPolicies::OLD && id <= cmPolicies::CMP0142 &&
    !(GetCMakeInstance()->GetIsInTryCompile() &&
      (
        // Policies set by cmCoreTryCompile::TryCompileCode.
        id == cmPolicies::CMP0083 || id == cmPolicies::CMP0091 || id == cmPolicies::CMP0104 ||
        id == cmPolicies::CMP0123 || id == cmPolicies::CMP0126 || id == cmPolicies::CMP0128 ||
        id == cmPolicies::CMP0136 || id == cmPolicies::CMP0141)) &&
    (!IsSet("CMAKE_WARN_DEPRECATED") || IsOn("CMAKE_WARN_DEPRECATED"))) {
    IssueMessage(MessageType::DEPRECATION_WARNING, cmPolicies::GetPolicyDeprecatedWarning(id));
  }

  m_stateSnapshot.SetPolicy(id, status);
  return true;
}

cmMakefile::PolicyPushPop::PolicyPushPop(cmMakefile* m)
  : m_pMakefile(m)
{
  m_pMakefile->PushPolicy();
}

cmMakefile::PolicyPushPop::~PolicyPushPop()
{
  m_pMakefile->PopPolicy();
}

void cmMakefile::PushPolicy(
  bool weak,
  cmPolicies::PolicyMap const& pm)
{
  m_stateSnapshot.PushPolicy(pm, weak);
}

void cmMakefile::PopPolicy()
{
  if (!m_stateSnapshot.PopPolicy()) {
    IssueMessage(MessageType::FATAL_ERROR, "cmake_policy POP without matching PUSH");
  }
}

void cmMakefile::PopSnapshot(bool reportError)
{
  // cmStateSnapshot manages nested policy scopes within it.
  // Since the scope corresponding to the snapshot is closing,
  // reject any still-open nested policy scopes with an error.
  while (m_stateSnapshot.CanPopPolicyScope()) {
    if (reportError) {
      IssueMessage(MessageType::FATAL_ERROR, "cmake_policy PUSH without matching POP");
      reportError = false;
    }
    PopPolicy();
  }

  m_stateSnapshot = GetState()->Pop(m_stateSnapshot);
  assert(m_stateSnapshot.IsValid());
}

bool cmMakefile::SetPolicyVersion(
  std::string const& version_min,
  std::string const& version_max)
{
  return cmPolicies::ApplyPolicyVersion(this, version_min, version_max, cmPolicies::WarnCompat::On);
}

cmMakefile::VariablePushPop::VariablePushPop(cmMakefile* m)
  : m_pMakefile(m)
{
  m_pMakefile->m_stateSnapshot = m_pMakefile->GetState()->CreateVariableScopeSnapshot(m_pMakefile->m_stateSnapshot);
}

cmMakefile::VariablePushPop::~VariablePushPop()
{
  m_pMakefile->PopSnapshot();
}

void cmMakefile::RecordPolicies(cmPolicies::PolicyMap& pm) const
{
  /* Record the setting of every policy.  */
  using PolicyID = cmPolicies::PolicyID;
  for (PolicyID pid = cmPolicies::CMP0000; pid != cmPolicies::CMPCOUNT; pid = static_cast<PolicyID>(pid + 1)) {
    pm.m_set(pid, GetPolicyStatus(pid));
  }
}

cmMakefile::FunctionPushPop::FunctionPushPop(
  cmMakefile* mf,
  std::string const& fileName,
  cmPolicies::PolicyMap const& pm)
  : m_pMakefile(mf)
{
  m_pMakefile->PushFunctionScope(fileName, pm);
}

cmMakefile::FunctionPushPop::~FunctionPushPop()
{
  m_pMakefile->PopFunctionScope(m_reportError);
}

cmMakefile::MacroPushPop::MacroPushPop(
  cmMakefile* mf,
  std::string const& fileName,
  cmPolicies::PolicyMap const& pm)
  : m_pMakefile(mf)
{
  m_pMakefile->PushMacroScope(fileName, pm);
}

cmMakefile::MacroPushPop::~MacroPushPop()
{
  m_pMakefile->PopMacroScope(m_reportError);
}

cmMakefile::FindPackageStackRAII::FindPackageStackRAII(
  cmMakefile* mf,
  std::string const& name)
  : m_pMakefile(mf)
{
  m_pMakefile->m_findPackageStack = m_pMakefile->m_findPackageStack.Push(
    cmFindPackageCall{
      name,
      m_pMakefile->m_findPackageStackNextIndex,
    });
  m_pMakefile->m_findPackageStackNextIndex++;
}

cmMakefile::FindPackageStackRAII::~FindPackageStackRAII()
{
  m_pMakefile->m_findPackageStackNextIndex = m_pMakefile->m_findPackageStack.Top().Index + 1;
  m_pMakefile->m_findPackageStack = m_pMakefile->m_findPackageStack.Pop();

  if (!m_pMakefile->m_findPackageStack.Empty()) {
    auto top = m_pMakefile->m_findPackageStack.Top();
    m_pMakefile->m_findPackageStack = m_pMakefile->m_findPackageStack.Pop();

    top.Index = m_pMakefile->m_findPackageStackNextIndex;
    m_pMakefile->m_findPackageStackNextIndex++;

    m_pMakefile->m_findPackageStack = m_pMakefile->m_findPackageStack.Push(top);
  }
}

cmMakefile::DebugFindPkgRAII::DebugFindPkgRAII(
  cmMakefile* mf,
  std::string const& pkg)
  : m_pMakefile(mf)
  , m_oldValue(m_pMakefile->m_debugFindPkg)
{
  m_pMakefile->m_debugFindPkg = m_pMakefile->GetCMakeInstance()->GetDebugFindPkgOutput(pkg);
}

cmMakefile::DebugFindPkgRAII::~DebugFindPkgRAII()
{
  m_pMakefile->m_debugFindPkg = m_oldValue;
}

bool cmMakefile::GetDebugFindPkgMode() const
{
  return m_debugFindPkg;
}
