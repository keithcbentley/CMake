/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cm/string_view>
#include <cmext/string_view>

#include "cmDocumentationEntry.h" // IWYU pragma: keep
#include "cmGeneratedFileStream.h"
#include "cmGlobalGeneratorFactory.h"
#include "cmInstalledFile.h"
#include "cmListFileCache.h"
#include "cmMessageType.h"
#include "cmState.h"
#include "cmStateSnapshot.h"
#include "cmStateTypes.h"
#include "cmValue.h"

#if !defined(CMAKE_BOOTSTRAP)
#  include <type_traits>

#  include <cm/optional>

#  include <cm3p/json/value.h>

#  include "cmCMakePresetsGraph.h"
#  include "cmMakefileProfilingData.h"
#endif

class cmConfigureLog;

#ifdef CMake_ENABLE_DEBUGGER
namespace cmDebugger {
class cmDebuggerAdapter;
}
#endif

class cmExternalMakefileProjectGeneratorFactory;
class cmFileAPI;
//class cmInstrumentation;
class cmFileTimeCache;
class cmGlobalGenerator;
class cmMakefile;
class cmMessenger;
class cmVariableWatch;
struct cmBuildOptions;
struct cmGlobCacheEntry;

/** \brief Represents a cmake invocation.
 *
 * This class represents a cmake invocation. It is the top level class when
 * running cmake. Most cmake based GUIs should primarily create an instance
 * of this class and communicate with it.
 *
 * The basic process for a GUI is as follows:
 *
 * -# Create a cmake instance
 * -# Set the Home directories, generator, and cmake command. this
 *    can be done using the Set methods or by using SetArgs and passing in
 *    command line arguments.
 * -# Load the cache by calling LoadCache (duh)
 * -# if you are using command line arguments with -D or -C flags then
 *    call SetCacheArgs (or if for some other reason you want to modify the
 *    cache), do it now.
 * -# Finally call Configure
 * -# Let the user change values and go back to step 5
 * -# call Generate

 * If your GUI allows the user to change the home directories then
 * you must at a minimum redo steps 2 through 7.
 */

class CMake
{
public:
  using ProgressCallbackType = std::function<void(std::string const&, float)>;

  enum WorkingMode
  {
    NORMAL_MODE, ///< Cmake runs to create project files
    SCRIPT_MODE,
    HELP_MODE,
    FIND_PACKAGE_MODE
  };

  enum class CommandFailureAction
  {
    FATAL_ERROR,
    EXIT_CODE,
  };

  using TraceFormat = cmTraceEnums::TraceOutputFormat;

  struct FileExtensions
  {
    bool Test(cm::string_view ext) const { return (this->unordered.find(ext) != this->unordered.end()); }

    std::vector<std::string> ordered;
    std::unordered_set<cm::string_view> unordered;
  };

  using InstalledFilesMap = std::map<std::string, cmInstalledFile>;

private:
  std::vector<std::string> m_cmdArgs;
  std::string m_cmakeWorkingDirectory;
  ProgressCallbackType m_progressCallback;
  WorkingMode m_currentWorkingMode = NORMAL_MODE;
  CommandFailureAction m_currentCommandFailureAction = CommandFailureAction::FATAL_ERROR;
  bool m_debugOutput = false;
  bool m_debugFindOutput = false;
  bool m_trace = false;
  bool m_traceExpand = false;
  TraceFormat m_traceFormatVar = TraceFormat::Human;
  cmGeneratedFileStream m_traceFile;
  CMake* m_traceRedirect = nullptr;
#ifndef CMAKE_BOOTSTRAP
  std::unique_ptr<cmConfigureLog> m_configureLog;
#endif
  bool m_warnUninitialized = false;
  bool m_warnUnusedCli = true;
  bool m_checkSystemVars = false;
  bool m_ignoreCompileWarningAsError = false;
  bool m_ignoreLinkWarningAsError = false;
  std::map<std::string, bool> m_usedCliVariables;
  std::string m_cmakeEditCommand;
  std::string m_CXXEnvironment;
  std::string m_CCEnvironment;
  std::string m_checkBuildSystemArgument;
  std::string m_checkStampFile;
  std::string m_checkStampList;
  std::string m_VSSolutionFile;
  std::string m_environmentGenerator;
  FileExtensions m_CLikeSourceFileExtensions;
  FileExtensions m_headerFileExtensions;
  FileExtensions m_cudaFileExtensions;
  FileExtensions m_ISPCFileExtensions;
  FileExtensions m_FortranFileExtensions;
  FileExtensions m_HipFileExtensions;
  bool m_clearBuildSystem = false;
  bool m_debugTryCompile = false;
  bool m_freshCache = false;
  bool m_regenerateDuringBuild = false;
  std::string m_cmakeListName;
  std::unique_ptr<cmFileTimeCache> m_fileTimeCache;
  std::string m_graphVizFile;
  InstalledFilesMap m_installedFiles;
#ifndef CMAKE_BOOTSTRAP
  std::map<std::string, cm::optional<cmCMakePresetsGraph::CacheVariable>> m_unprocessedPresetVariables;
  std::map<std::string, cm::optional<std::string>> m_unprocessedPresetEnvironment;
#endif

#if !defined(CMAKE_BOOTSTRAP)
  std::unique_ptr<cmVariableWatch> m_pVariableWatch;
  std::unique_ptr<cmFileAPI> m_pFileAPI;
//  std::unique_ptr<cmInstrumentation> m_pInstrumentation;
#endif

  std::unique_ptr<cmState> m_pState;
  cmStateSnapshot m_currentSnapshot;
  std::unique_ptr<cmMessenger> m_pMessenger;

#ifndef CMAKE_BOOTSTRAP
  bool m_SarifFileOutput = false;
  std::string m_SarifFilePath;
#endif

  std::vector<std::string> m_traceOnlyThisSources;

  std::set<std::string> m_debugFindPkgs;
  std::set<std::string> m_debugFindVars;

  Message::LogLevel m_messageLogLevel = Message::LogLevel::LOG_STATUS;
  bool m_logLevelWasSetViaCLI = false;
  bool m_logContext = false;

  std::vector<std::string> m_checkInProgressMessages;

  std::unique_ptr<cmGlobalGenerator> m_pGlobalGenerator;

#if !defined(CMAKE_BOOTSTRAP)
  std::unique_ptr<cmMakefileProfilingData> m_profilingOutput;
#endif

#ifdef CMake_ENABLE_DEBUGGER
  std::shared_ptr<cmDebugger::cmDebuggerAdapter> m_debugAdapter;
  bool m_debuggerOn = false;
  std::string m_debuggerPipe;
  std::string m_debuggerDapLogFile;
#endif

public:
  enum Role
  {
    RoleInternal, // no commands
    RoleScript,   // script commands
    RoleProject   // all commands
  };

  enum DiagLevel
  {
    DIAG_IGNORE,
    DIAG_WARN,
    DIAG_ERROR
  };

  struct GeneratorInfo
  {
    std::string name;
    std::string baseName;
    std::string extraName;
    bool supportsToolset;
    bool supportsPlatform;
    std::vector<std::string> supportedPlatforms;
    std::string defaultPlatform;
    bool isAlias;
  };

  static int const NO_BUILD_PARALLEL_LEVEL = -1;
  static int const DEFAULT_BUILD_PARALLEL_LEVEL = 0;

  /// Default constructor
  CMake(
    Role role,
    cmState::Mode mode,
    cmState::ProjectKind projectKind = cmState::ProjectKind::Normal);
  ~CMake();

  CMake(CMake const&) = delete;
  CMake& operator=(CMake const&) = delete;

#if !defined(CMAKE_BOOTSTRAP)
  Json::Value ReportVersionJson() const;
  Json::Value ReportCapabilitiesJson() const;
#endif
  std::string ReportCapabilities() const;

  /**
   * Set the home directory from `-S` or from a known location
   * that contains a CMakeLists.txt. Will generate warnings
   * when overriding an existing source directory.
   *
   *  |    args           | src dir| warning        |
   *  | ----------------- | ------ | -------------- |
   *  | `dirA dirA`       | dirA   | N/A            |
   *  | `-S dirA -S dirA` | dirA   | N/A            |
   *  | `-S dirA -S dirB` | dirB   | Ignoring dirA  |
   *  | `-S dirA dirB`    | dirB   | Ignoring dirA  |
   *  | `dirA -S dirB`    | dirB   | Ignoring dirA  |
   *  | `dirA dirB`       | dirB   | Ignoring dirA  |
   */
  void SetHomeDirectoryViaCommandLine(std::string const& path);
  void SetHomeDirectory(std::string const& dir);
  std::string const& GetHomeDirectory() const;
  void SetHomeOutputDirectory(std::string const& dir);
  std::string const& GetHomeOutputDirectory() const;

  std::string const& GetCMakeWorkingDirectory() const { return m_cmakeWorkingDirectory; }

  /**
   * Handle a command line invocation of cmake.
   */
  int Run(std::vector<std::string> const& args) { return Run(args, false); }
  int Run(
    std::vector<std::string> const& args,
    bool noconfigure);

  /**
   * Run the global generator Generate step.
   */
  int Generate();

  /**
   * Configure the cmMakefiles. This routine will create a GlobalGenerator if
   * one has not already been set. It will then Call Configure on the
   * GlobalGenerator. This in turn will read in an process all the CMakeList
   * files for the tree. It will not produce any actual Makefiles, or
   * workspaces. Generate does that.  */
  int Configure();
  int ActualConfigure();

  //! Break up a line like VAR:type="value" into var, type and value
  static bool ParseCacheEntry(
    std::string const& entry,
    std::string& var,
    std::string& value,
    cmStateEnums::CacheEntryType& type);

  int LoadCache();
  bool LoadCache(std::string const& path);
  bool LoadCache(
    std::string const& path,
    bool internal,
    std::set<std::string>& excludes,
    std::set<std::string>& includes);
  bool SaveCache(std::string const& path);
  bool DeleteCache(std::string const& path);
  void PreLoadCMakeFiles();

  std::unique_ptr<cmGlobalGenerator> CreateGlobalGenerator(std::string const& name);
  bool CreateAndSetGlobalGenerator(std::string const& name);
  cmGlobalGenerator* GetGlobalGenerator() { return m_pGlobalGenerator.get(); }
  cmGlobalGenerator const* GetGlobalGenerator() const { return m_pGlobalGenerator.get(); }
  void SetGlobalGenerator(std::unique_ptr<cmGlobalGenerator>);

  void GetRegisteredGenerators(std::vector<GeneratorInfo>& generators) const;

  void SetGeneratorInstance(std::string const& instance)
  {
    m_generatorInstance = instance;
    m_generatorInstanceSet = true;
  }

  void SetGeneratorPlatform(std::string const& ts)
  {
    m_generatorPlatform = ts;
    m_generatorPlatformSet = true;
  }

  void SetGeneratorToolset(std::string const& ts)
  {
    m_generatorToolset = ts;
    m_generatorToolsetSet = true;
  }


#ifndef CMAKE_BOOTSTRAP
  //! Print list of configure presets
  void PrintPresetList(cmCMakePresetsGraph const& graph) const;
#endif


  //! Return the full path to where the CMakeCache.txt file should be.
  static std::string FindCacheFile(std::string const& binaryDir);

  //! Set the name of the graphviz file.
  void SetGraphVizFile(std::string const& ts) { m_graphVizFile = ts; }

  bool IsAKnownSourceExtension(cm::string_view ext) const
  {
    return m_CLikeSourceFileExtensions.Test(ext) || m_cudaFileExtensions.Test(ext) ||
      m_FortranFileExtensions.Test(ext) || m_HipFileExtensions.Test(ext) || m_ISPCFileExtensions.Test(ext);
  }

  bool IsACLikeSourceExtension(cm::string_view ext) const { return m_CLikeSourceFileExtensions.Test(ext); }

  bool IsAKnownExtension(cm::string_view ext) const { return IsAKnownSourceExtension(ext) || IsAHeaderExtension(ext); }

  std::vector<std::string> GetAllExtensions() const;

  std::vector<std::string> const& GetHeaderExtensions() const { return m_headerFileExtensions.ordered; }

  bool IsAHeaderExtension(cm::string_view ext) const { return m_headerFileExtensions.Test(ext); }

  // Strips the extension (if present and known) from a filename
  std::string StripExtension(std::string const& file) const;

  /**
   * Given a variable name, return its value (as a string).
   */
  cmValue GetCacheDefinition(std::string const&) const;
  void AddCacheEntry(
    std::string const& key,
    std::string const& value,
    std::string const& helpString,
    int type)
  {
    AddCacheEntry(key, cmValue{ value }, cmValue{ helpString }, type);
  }
  void AddCacheEntry(
    std::string const& key,
    cmValue value,
    std::string const& helpString,
    int type)
  {
    AddCacheEntry(key, value, cmValue{ helpString }, type);
  }
  void AddCacheEntry(
    std::string const& key,
    cmValue value,
    cmValue helpString,
    int type);

  bool DoWriteGlobVerifyTarget() const;
  std::string const& GetGlobVerifyScript() const;
  std::string const& GetGlobVerifyStamp() const;
  void AddGlobCacheEntry(
    cmGlobCacheEntry const& entry,
    std::string const& variable,
    cmListFileBacktrace const& bt);
  std::vector<cmGlobCacheEntry> GetGlobCacheEntries() const;

  /**
   * Get the system information and write it to the file specified
   */
  int GetSystemInformation(std::vector<std::string>&);

  //! Parse environment variables
  void LoadEnvironmentPresets();

  //! Parse command line arguments
  void SetArgs(std::vector<std::string> const& args);

  //! Is this cmake running as a result of a TRY_COMPILE command
  bool GetIsInTryCompile() const;

#ifndef CMAKE_BOOTSTRAP
  void SetWarningFromPreset(
    std::string const& name,
    cm::optional<bool> const& warning,
    cm::optional<bool> const& error);
  void ProcessPresetVariables();
  void PrintPresetVariables();
  void ProcessPresetEnvironment();
  void PrintPresetEnvironment();
#endif

  //! Parse command line arguments that might set cache values
  bool SetCacheArgs(std::vector<std::string> const&);

  void ProcessCacheArg(
    std::string const& var,
    std::string const& value,
    cmStateEnums::CacheEntryType type);

  /**
   *  Set the function used by GUIs to receive progress updates
   *  Function gets passed: message as a const char*, a progress
   *  amount ranging from 0 to 1.0 and client data. The progress
   *  number provided may be negative in cases where a message is
   *  to be displayed without any progress percentage.
   */
  void SetProgressCallback(ProgressCallbackType f);

  //! this is called by generators to update the progress
  void UpdateProgress(
    std::string const& msg,
    float prog);

#if !defined(CMAKE_BOOTSTRAP)
  //! Get the variable watch object
  cmVariableWatch* GetVariableWatch() { return m_pVariableWatch.get(); }
#endif

//  std::vector<cmDocumentationEntry> GetGeneratorsDocumentation();

  //! Set/Get a property of this target file
  void SetProperty(
    std::string const& prop,
    cmValue value);
  void SetProperty(
    std::string const& prop,
    std::nullptr_t)
  {
    SetProperty(prop, cmValue{ nullptr });
  }
  void SetProperty(
    std::string const& prop,
    std::string const& value)
  {
    SetProperty(prop, cmValue(value));
  }
  void AppendProperty(
    std::string const& prop,
    std::string const& value,
    bool asString = false);
  cmValue GetProperty(std::string const& prop);
  bool GetPropertyAsBool(std::string const& prop);

  //! Get or create an cmInstalledFile instance and return a pointer to it
  cmInstalledFile* GetOrCreateInstalledFile(
    cmMakefile* mf,
    std::string const& name);

  cmInstalledFile const* GetInstalledFile(std::string const& name) const;

  InstalledFilesMap const& GetInstalledFiles() const { return m_installedFiles; }

  //! Do all the checks before running configure
  int DoPreConfigureChecks();

  void SetWorkingMode(
    WorkingMode mode,
    CommandFailureAction policy)
  {
    m_currentWorkingMode = mode;
    m_currentCommandFailureAction = policy;
  }

  WorkingMode GetWorkingMode() const { return m_currentWorkingMode; }

  CommandFailureAction GetCommandFailureAction() const { return m_currentCommandFailureAction; }

  //! Debug the try compile stuff by not deleting the files
  bool GetDebugTryCompile() const { return m_debugTryCompile; }
  void DebugTryCompileOn() { m_debugTryCompile = true; }

  /**
   * Generate CMAKE_ROOT and CMAKE_COMMAND cache entries
   */
  int AddCMakePaths();

  /**
   * Get the file comparison class
   */
  cmFileTimeCache* GetFileTimeCache() { return m_fileTimeCache.get(); }

  bool WasLogLevelSetViaCLI() const { return m_logLevelWasSetViaCLI; }

  //! Get the selected log level for `message()` commands during the cmake run.
  Message::LogLevel GetLogLevel() const { return m_messageLogLevel; }
  void SetLogLevel(Message::LogLevel level) { m_messageLogLevel = level; }
  static Message::LogLevel StringToLogLevel(cm::string_view levelStr);
  static std::string LogLevelToString(Message::LogLevel level);
  static TraceFormat StringToTraceFormat(std::string const& levelStr);

  bool HasCheckInProgress() const { return !m_checkInProgressMessages.empty(); }
  std::size_t GetCheckInProgressSize() const { return m_checkInProgressMessages.size(); }
  std::string GetTopCheckInProgressMessage()
  {
    auto message = m_checkInProgressMessages.back();
    m_checkInProgressMessages.pop_back();
    return message;
  }
  void PushCheckInProgressMessage(std::string message) { m_checkInProgressMessages.emplace_back(std::move(message)); }
  std::vector<std::string> const& GetCheckInProgressMessages() const { return m_checkInProgressMessages; }

  //! Should `message` command display context.
  bool GetShowLogContext() const { return m_logContext; }
  void SetShowLogContext(bool b) { m_logContext = b; }

  //! Do we want debug output during the cmake run.
  bool GetDebugOutput() const { return m_debugOutput; }
  void SetDebugOutputOn(bool b) { m_debugOutput = b; }

  //! Do we want debug output from the find commands during the cmake run.
  bool GetDebugFindOutput() const { return m_debugFindOutput; }
  bool GetDebugFindOutput(std::string const& var) const;
  bool GetDebugFindPkgOutput(std::string const& pkg) const;
  void SetDebugFindOutput(bool b) { m_debugFindOutput = b; }
  void SetDebugFindOutputPkgs(std::string const& args);
  void SetDebugFindOutputVars(std::string const& args);

  //! Do we want trace output during the cmake run.
  bool GetTrace() const { return m_trace; }
  void SetTrace(bool b) { m_trace = b; }
  bool GetTraceExpand() const { return m_traceExpand; }
  void SetTraceExpand(bool b) { m_traceExpand = b; }
  TraceFormat GetTraceFormat() const { return m_traceFormatVar; }
  void SetTraceFormat(TraceFormat f) { m_traceFormatVar = f; }
  void AddTraceSource(std::string const& file) { m_traceOnlyThisSources.push_back(file); }
  std::vector<std::string> const& GetTraceSources() const { return m_traceOnlyThisSources; }
  cmGeneratedFileStream& GetTraceFile()
  {
    if (m_traceRedirect) {
      return m_traceRedirect->GetTraceFile();
    }
    return m_traceFile;
  }
  void SetTraceFile(std::string const& file);
  void PrintTraceFormatVersion();

#ifndef CMAKE_BOOTSTRAP
  cmConfigureLog* GetConfigureLog() const { return m_configureLog.get(); }
#endif

  //! Use trace from another ::cmake instance.
  void SetTraceRedirect(CMake* other);

  bool GetWarnUninitialized() const { return m_warnUninitialized; }
  void SetWarnUninitialized(bool b) { m_warnUninitialized = b; }
  bool GetWarnUnusedCli() const { return m_warnUnusedCli; }
  void SetWarnUnusedCli(bool b) { m_warnUnusedCli = b; }
  bool GetCheckSystemVars() const { return m_checkSystemVars; }
  void SetCheckSystemVars(bool b) { m_checkSystemVars = b; }
  bool GetIgnoreCompileWarningAsError() const { return m_ignoreCompileWarningAsError; }
  void SetIgnoreCompileWarningAsError(bool b) { m_ignoreCompileWarningAsError = b; }
  bool GetIgnoreLinkWarningAsError() const { return m_ignoreLinkWarningAsError; }
  void SetIgnoreLinkWarningAsError(bool b) { m_ignoreLinkWarningAsError = b; }

  void MarkCliAsUsed(std::string const& variable);

  /** Get the list of configurations (in upper case) considered to be
      debugging configurations.*/
  std::vector<std::string> GetDebugConfigs();

  void SetCMakeEditCommand(std::string const& s) { m_cmakeEditCommand = s; }
  std::string const& GetCMakeEditCommand() const { return m_cmakeEditCommand; }

  cmMessenger* GetMessenger() const { return m_pMessenger.get(); }

#ifndef CMAKE_BOOTSTRAP
  /// Get the SARIF file path if set manually for this run
  cm::optional<std::string> GetSarifFilePath() const
  {
    return (m_SarifFileOutput ? cm::make_optional(m_SarifFilePath) : cm::nullopt);
  }
#endif

  /**
   * Get the state of the suppression of developer (author) warnings.
   * Returns false, by default, if developer warnings should be shown, true
   * otherwise.
   */
  bool GetSuppressDevWarnings() const;
  /**
   * Set the state of the suppression of developer (author) warnings.
   */
  void SetSuppressDevWarnings(bool v);

  /**
   * Get the state of the suppression of deprecated warnings.
   * Returns false, by default, if deprecated warnings should be shown, true
   * otherwise.
   */
  bool GetSuppressDeprecatedWarnings() const;
  /**
   * Set the state of the suppression of deprecated warnings.
   */
  void SetSuppressDeprecatedWarnings(bool v);

  /**
   * Get the state of treating developer (author) warnings as errors.
   * Returns false, by default, if warnings should not be treated as errors,
   * true otherwise.
   */
  bool GetDevWarningsAsErrors() const;
  /**
   * Set the state of treating developer (author) warnings as errors.
   */
  void SetDevWarningsAsErrors(bool v);

  /**
   * Get the state of treating deprecated warnings as errors.
   * Returns false, by default, if warnings should not be treated as errors,
   * true otherwise.
   */
  bool GetDeprecatedWarningsAsErrors() const;
  /**
   * Set the state of treating developer (author) warnings as errors.
   */
  void SetDeprecatedWarningsAsErrors(bool v);

  /** Display a message to the user.  */
  void IssueMessage(
    MessageType t,
    std::string const& text,
    cmListFileBacktrace const& backtrace = cmListFileBacktrace()) const;

  //! run the --build option
  int Build(
    int jobs,
    std::string dir,
    std::vector<std::string> targets,
    std::string config,
    std::vector<std::string> nativeOptions,
    cmBuildOptions& buildOptions,
    bool verbose,
    std::string const& presetName,
    bool listPresets,
    std::vector<std::string> const& args);

  enum class DryRun
  {
    No,
    Yes,
  };

  //! run the --open option
  bool Open(
    std::string const& dir,
    DryRun dryRun);

  //! run the --workflow option
  enum class WorkflowListPresets
  {
    No,
    Yes,
  };
  enum class WorkflowFresh
  {
    No,
    Yes,
  };
  int Workflow(
    std::string const& presetName,
    WorkflowListPresets listPresets,
    WorkflowFresh fresh);

  void UnwatchUnusedCli(std::string const& var);
  void WatchUnusedCli(std::string const& var);

#if !defined(CMAKE_BOOTSTRAP)
  cmFileAPI* GetFileAPI() const { return m_pFileAPI.get(); }
  //cmInstrumentation* GetInstrumentation() const { return m_pInstrumentation.get(); }
#endif

  cmState* GetState() const { return m_pState.get(); }
  void SetCurrentSnapshot(cmStateSnapshot const& snapshot) { m_currentSnapshot = snapshot; }
  cmStateSnapshot GetCurrentSnapshot() const { return m_currentSnapshot; }

  bool GetRegenerateDuringBuild() const { return m_regenerateDuringBuild; }

  void SetCMakeListName(std::string const& name);
  std::string GetCMakeListFile(std::string const& dir) const;

#if !defined(CMAKE_BOOTSTRAP)
  cmMakefileProfilingData& GetProfilingOutput();
  bool IsProfilingEnabled() const;

  cm::optional<cmMakefileProfilingData::RAII> CreateProfilingEntry(
    std::string const& category,
    std::string const& name)
  {
    return CreateProfilingEntry(category, name, []() -> cm::nullopt_t { return cm::nullopt; });
  }

  template <typename ArgsFunc>
  cm::optional<cmMakefileProfilingData::RAII> CreateProfilingEntry(
    std::string const& category,
    std::string const& name,
    ArgsFunc&& argsFunc)
  {
    if (IsProfilingEnabled()) {
      return cm::make_optional<cmMakefileProfilingData::RAII>(GetProfilingOutput(), category, name, argsFunc());
    }
    return cm::nullopt;
  }
#endif

#ifdef CMake_ENABLE_DEBUGGER
  bool GetDebuggerOn() const { return m_debuggerOn; }
  std::string GetDebuggerPipe() const { return m_debuggerPipe; }
  std::string GetDebuggerDapLogFile() const { return m_debuggerDapLogFile; }
  void SetDebuggerOn(bool b) { m_debuggerOn = b; }
  bool StartDebuggerIfEnabled();
  void StopDebuggerIfNeeded(int exitCode);
  std::shared_ptr<cmDebugger::cmDebuggerAdapter> GetDebugAdapter() const noexcept { return m_debugAdapter; }
#endif

protected:
  void RunCheckForUnusedVariables();
  int HandleDeleteCacheVariables(std::string const& var);

  using RegisteredGeneratorsVector = std::vector<std::unique_ptr<cmGlobalGeneratorFactory>>;
  RegisteredGeneratorsVector m_generators;
  using RegisteredExtraGeneratorsVector = std::vector<cmExternalMakefileProjectGeneratorFactory*>;
  RegisteredExtraGeneratorsVector m_extraGenerators;
  void AddScriptingCommands() const;
  void AddProjectCommands() const;
  void AddDefaultGenerators();
  void AddDefaultExtraGenerators();

  std::map<std::string, DiagLevel> m_diagLevels;
  std::string m_generatorInstance;
  std::string m_generatorPlatform;
  std::string m_generatorToolset;
  bool m_generatorInstanceSet = false;
  bool m_generatorPlatformSet = false;
  bool m_generatorToolsetSet = false;

  //! read in a cmake list file to initialize the cache
  void ReadListFile(
    std::vector<std::string> const& args,
    std::string const& path);
  bool FindPackage(std::vector<std::string> const& args);

  //! Check if CMAKE_CACHEFILE_DIR is set. If it is not, delete the log file.
  ///  If it is set, truncate it to 50kb
  void TruncateOutputLog(char const* fname);

  /**
   * Method called to check build system integrity at build time.
   * Returns 1 if CMake should rerun and 0 otherwise.
   */
  int CheckBuildSystem();

  bool SetDirectoriesFromFile(std::string const& arg);

  //! Make sure all commands are what they say they are and there is no
  /// macros.
  void CleanupCommandsAndMacros();

  void GenerateGraphViz(std::string const& fileName) const;

private:
  //! Print a list of valid generators to stderr.
//  void PrintGeneratorList();

  std::unique_ptr<cmGlobalGenerator> EvaluateDefaultGlobalGenerator();
  void CreateDefaultGlobalGenerator();

  //void AppendGlobalGeneratorsDocumentation(std::vector<cmDocumentationEntry>&);
  //void AppendExtraGeneratorsDocumentation(std::vector<cmDocumentationEntry>&);

#if !defined(CMAKE_BOOTSTRAP)
  template <typename T>
  T const* FindPresetForWorkflow(
    cm::static_string_view type,
    std::map<
      std::string,
      cmCMakePresetsGraph::PresetPair<T>> const& presets,
    cmCMakePresetsGraph::WorkflowPreset::WorkflowStep const& step);

  std::function<int()> BuildWorkflowStep(std::vector<std::string> const& args);
#endif

  cm::optional<int> m_scriptModeExitCode;

public:
  bool HasScriptModeExitCode() const { return m_scriptModeExitCode.has_value(); }
  void SetScriptModeExitCode(int code) { m_scriptModeExitCode = code; }
  int GetScriptModeExitCode() const { return m_scriptModeExitCode.value_or(-1); }

//  static cmDocumentationEntry CMAKE_STANDARD_OPTIONS_TABLE[19];
};

#define FOR_EACH_C90_FEATURE(F) F(c_function_prototypes)

#define FOR_EACH_C99_FEATURE(F)                                                                                        \
  F(c_restrict)                                                                                                        \
  F(c_variadic_macros)

#define FOR_EACH_C11_FEATURE(F) F(c_static_assert)

#define FOR_EACH_C_FEATURE(F)                                                                                          \
  F(c_std_90)                                                                                                          \
  F(c_std_99)                                                                                                          \
  F(c_std_11)                                                                                                          \
  F(c_std_17)                                                                                                          \
  F(c_std_23)                                                                                                          \
  FOR_EACH_C90_FEATURE(F)                                                                                              \
  FOR_EACH_C99_FEATURE(F)                                                                                              \
  FOR_EACH_C11_FEATURE(F)

#define FOR_EACH_CXX98_FEATURE(F) F(cxx_template_template_parameters)

#define FOR_EACH_CXX11_FEATURE(F)                                                                                      \
  F(cxx_alias_templates)                                                                                               \
  F(cxx_alignas)                                                                                                       \
  F(cxx_alignof)                                                                                                       \
  F(cxx_attributes)                                                                                                    \
  F(cxx_auto_type)                                                                                                     \
  F(cxx_constexpr)                                                                                                     \
  F(cxx_decltype)                                                                                                      \
  F(cxx_decltype_incomplete_return_types)                                                                              \
  F(cxx_default_function_template_args)                                                                                \
  F(cxx_defaulted_functions)                                                                                           \
  F(cxx_defaulted_move_initializers)                                                                                   \
  F(cxx_delegating_constructors)                                                                                       \
  F(cxx_deleted_functions)                                                                                             \
  F(cxx_enum_forward_declarations)                                                                                     \
  F(cxx_explicit_conversions)                                                                                          \
  F(cxx_extended_friend_declarations)                                                                                  \
  F(cxx_extern_templates)                                                                                              \
  F(cxx_final)                                                                                                         \
  F(cxx_func_identifier)                                                                                               \
  F(cxx_generalized_initializers)                                                                                      \
  F(cxx_inheriting_constructors)                                                                                       \
  F(cxx_inline_namespaces)                                                                                             \
  F(cxx_lambdas)                                                                                                       \
  F(cxx_local_type_template_args)                                                                                      \
  F(cxx_long_long_type)                                                                                                \
  F(cxx_noexcept)                                                                                                      \
  F(cxx_nonstatic_member_init)                                                                                         \
  F(cxx_nullptr)                                                                                                       \
  F(cxx_override)                                                                                                      \
  F(cxx_range_for)                                                                                                     \
  F(cxx_raw_string_literals)                                                                                           \
  F(cxx_reference_qualified_functions)                                                                                 \
  F(cxx_right_angle_brackets)                                                                                          \
  F(cxx_rvalue_references)                                                                                             \
  F(cxx_sizeof_member)                                                                                                 \
  F(cxx_static_assert)                                                                                                 \
  F(cxx_strong_enums)                                                                                                  \
  F(cxx_thread_local)                                                                                                  \
  F(cxx_trailing_return_types)                                                                                         \
  F(cxx_unicode_literals)                                                                                              \
  F(cxx_uniform_initialization)                                                                                        \
  F(cxx_unrestricted_unions)                                                                                           \
  F(cxx_user_literals)                                                                                                 \
  F(cxx_variadic_macros)                                                                                               \
  F(cxx_variadic_templates)

#define FOR_EACH_CXX14_FEATURE(F)                                                                                      \
  F(cxx_aggregate_default_initializers)                                                                                \
  F(cxx_attribute_deprecated)                                                                                          \
  F(cxx_binary_literals)                                                                                               \
  F(cxx_contextual_conversions)                                                                                        \
  F(cxx_decltype_auto)                                                                                                 \
  F(cxx_digit_separators)                                                                                              \
  F(cxx_generic_lambdas)                                                                                               \
  F(cxx_lambda_init_captures)                                                                                          \
  F(cxx_relaxed_constexpr)                                                                                             \
  F(cxx_return_type_deduction)                                                                                         \
  F(cxx_variable_templates)

#define FOR_EACH_CXX_FEATURE(F)                                                                                        \
  F(cxx_std_98)                                                                                                        \
  F(cxx_std_11)                                                                                                        \
  F(cxx_std_14)                                                                                                        \
  F(cxx_std_17)                                                                                                        \
  F(cxx_std_20)                                                                                                        \
  F(cxx_std_23)                                                                                                        \
  F(cxx_std_26)                                                                                                        \
  FOR_EACH_CXX98_FEATURE(F)                                                                                            \
  FOR_EACH_CXX11_FEATURE(F)                                                                                            \
  FOR_EACH_CXX14_FEATURE(F)

#define FOR_EACH_CUDA_FEATURE(F)                                                                                       \
  F(cuda_std_03)                                                                                                       \
  F(cuda_std_11)                                                                                                       \
  F(cuda_std_14)                                                                                                       \
  F(cuda_std_17)                                                                                                       \
  F(cuda_std_20)                                                                                                       \
  F(cuda_std_23)                                                                                                       \
  F(cuda_std_26)

#define FOR_EACH_HIP_FEATURE(F)                                                                                        \
  F(hip_std_98)                                                                                                        \
  F(hip_std_11)                                                                                                        \
  F(hip_std_14)                                                                                                        \
  F(hip_std_17)                                                                                                        \
  F(hip_std_20)                                                                                                        \
  F(hip_std_23)                                                                                                        \
  F(hip_std_26)
