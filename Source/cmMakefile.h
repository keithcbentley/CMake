/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cm/optional>
#include <cm/string_view>

#include "cmsys/RegularExpression.hxx"

#include "cm_sys_stat.h"

#include "cmAlgorithms.h"
#include "cmCustomCommand.h"
#include "cmFindPackageStack.h"
#include "cmFunctionBlocker.h"
#include "cmListFileCache.h"
#include "cmMessageType.h" // IWYU pragma: keep
#include "cmNewLineStyle.h"
#include "cmPolicies.h"
#include "cmSourceFileLocationKind.h"
#include "cmStateSnapshot.h"
#include "cmStateTypes.h"
#include "cmValue.h"

// IWYU does not see that 'std::unordered_map<std::string, cmTarget>'
// will not compile without the complete type.
#include "cmTarget.h" // IWYU pragma: keep

#if !defined(CMAKE_BOOTSTRAP)
#  include "cmSourceGroup.h"
#endif

enum class cmCustomCommandType;
enum class cmObjectLibraryCommands;

class cmCompiledGeneratorExpression;
class cmCustomCommandLines;
class cmExecutionStatus;
class cmExpandedCommandArgument;
class cmExportBuildFileGenerator;
class cmGeneratorExpressionEvaluationFile;
class cmGlobalGenerator;
class cmInstallGenerator;
class cmLocalGenerator;
class cmMessenger;
class cmSourceFile;
class cmState;
class cmTest;
class cmTestGenerator;
class cmVariableWatch;
class CMake;

/** A type-safe wrapper for a string representing a directory id.  */
//  TODO: this is not what it says.  It's just a wrapper for a string.
class cmDirectoryId
{
public:
  cmDirectoryId(std::string s);
  std::string String;
};

/** \class cmMakefile
 * \brief Process the input CMakeLists.txt file.
 *
 * Process and store into memory the input CMakeLists.txt file.
 * Each CMakeLists.txt file is parsed and the commands found there
 * are added into the build process.
 */
class cmMakefile
{
public:
  void MarkVariableAsUsed(std::string const& var);
  bool VariableInitialized(std::string const&) const;

  cmMakefile(
    cmGlobalGenerator* globalGenerator,
    cmStateSnapshot const& snapshot);

  ~cmMakefile();

  cmMakefile(cmMakefile const&) = delete;
  cmMakefile& operator=(cmMakefile const&) = delete;

  cmDirectoryId GetDirectoryId() const;

  bool ReadListFile(std::string const& filename);

  bool ReadListFileAsString(
    std::string const& content,
    std::string const& virtualFileName);

  bool ReadDependentFile(
    std::string const& filename,
    bool noPolicyScope = true);

  void AddFunctionBlocker(std::unique_ptr<cmFunctionBlocker> fb);

  bool IsRootMakefile() const;

  std::unique_ptr<cmFunctionBlocker> RemoveFunctionBlocker();

  int TryCompile(
    std::string const& srcdir,
    std::string const& bindir,
    std::string const& projectName,
    std::string const& targetName,
    bool fast,
    int jobs,
    std::vector<std::string> const* cmakeArgs,
    std::string& output);

  bool GetIsSourceFileTryCompile() const;

  /**
   * Help enforce global target name uniqueness.
   */
  bool EnforceUniqueName(
    std::string const& name,
    std::string& msg,
    bool isCustom = false) const;

  enum class GeneratorActionWhen
  {
    // Run after all CMake code has been parsed.
    AfterConfigure,
    // Run after generator targets have been constructed.
    AfterGeneratorTargets,
  };

  class GeneratorAction
  {
    using ActionT = std::function<void(cmLocalGenerator&, cmListFileBacktrace const&)>;
    using CCActionT = std::function<
      void(cmLocalGenerator&, cmListFileBacktrace const&, std::unique_ptr<cmCustomCommand> m_pCustomCommand)>;

  public:
    GeneratorAction(
      ActionT&& action,
      GeneratorActionWhen when = GeneratorActionWhen::AfterConfigure)
      : m_generatorActionWhen(when)
      , m_action(std::move(action))
    {
    }

    GeneratorAction(
      std::unique_ptr<cmCustomCommand> tcc,
      CCActionT&& action,
      GeneratorActionWhen when = GeneratorActionWhen::AfterConfigure)
      : m_generatorActionWhen(when)
      , m_CCAction(std::move(action))
      , m_pCustomCommand(std::move(tcc))
    {
    }

    void operator()(
      cmLocalGenerator& lg,
      cmListFileBacktrace const& lfbt,
      GeneratorActionWhen when);

  private:
    GeneratorActionWhen m_generatorActionWhen;

    ActionT m_action;

    // FIXME: Use std::variant
    CCActionT m_CCAction;
    std::unique_ptr<cmCustomCommand> m_pCustomCommand;
  };

  void AddGeneratorAction(GeneratorAction&& action);

  /// Helper to insert the constructor GeneratorAction(args...)
  template <class... Args>
  void AddGeneratorAction(Args&&... args)
  {
    AddGeneratorAction(GeneratorAction(std::move(args)...));
  }

  void Generate(cmLocalGenerator& lg);
  void GenerateAfterGeneratorTargets(cmLocalGenerator& lg);

  cmTarget* GetCustomCommandTarget(
    std::string const& target,
    cmObjectLibraryCommands objLibCommands,
    cmListFileBacktrace const& lfbt) const;

  /**
   * Dispatch adding a custom PRE_BUILD, PRE_LINK, or POST_BUILD command to a
   * target.
   */
  cmTarget* AddCustomCommandToTarget(
    std::string const& target,
    cmCustomCommandType type,
    std::unique_ptr<cmCustomCommand> m_pCustomCommand);

  /**
   * Called for each file with custom command.
   */
  using CommandSourceCallback = std::function<void(cmSourceFile*)>;

  /**
   * Dispatch adding a custom command to a source file.
   */
  void AddCustomCommandToOutput(
    std::unique_ptr<cmCustomCommand> m_pCustomCommand,
    CommandSourceCallback const& callback = nullptr,
    bool replace = false);
  void AppendCustomCommandToOutput(
    std::string const& output,
    std::vector<std::string> const& depends,
    cmImplicitDependsList const& implicit_depends,
    cmCustomCommandLines const& commandLines);

  void AddDefineFlag(std::string const& definition);
  void RemoveDefineFlag(std::string const& definition);
  void AddCompileDefinition(std::string const& definition);
  void AddCompileOption(std::string const& option);
  void AddLinkOption(std::string const& option);
  void AddLinkDirectory(
    std::string const& directory,
    bool before = false);

  cmTarget* AddImportedTarget(
    std::string const& name,
    cmStateEnums::TargetType type,
    bool global);

  cmTarget* AddForeignTarget(
    std::string const& origin,
    std::string const& name);

  std::pair<
    cmTarget&,
    bool>
  CreateNewTarget(
    std::string const& name,
    cmStateEnums::TargetType type,
    cmTarget::PerConfig perConfig = cmTarget::PerConfig::Yes,
    cmTarget::Visibility vis = cmTarget::Visibility::Normal);

  cmTarget* AddNewTarget(
    cmStateEnums::TargetType type,
    std::string const& name);

  cmTarget* AddSynthesizedTarget(
    cmStateEnums::TargetType type,
    std::string const& name);

  cmTarget* AddNewUtilityTarget(
    std::string const& utilityName,
    bool excludeFromAll);

  cmTarget* AddExecutable(
    std::string const& exename,
    std::vector<std::string> const& srcs,
    bool excludeFromAll = false);

  cmTarget* AddUtilityCommand(
    std::string const& utilityName,
    bool excludeFromAll,
    std::unique_ptr<cmCustomCommand> m_pCustomCommand);

  void AddSubDirectory(
    std::string const& fullSrcDir,
    std::string const& fullBinDir,
    bool excludeFromAll,
    bool immediate,
    bool system);

  void Configure();

  void ConfigureSubDirectory(cmMakefile* mf);

  void AddIncludeDirectories(
    std::vector<std::string> const& incs,
    bool before = false);

  void AddDefinition(
    std::string const& name,
    cm::string_view value);

  void AddDefinition(
    std::string const& name,
    cmValue value)
  {
    this->AddDefinition(name, *value);
  }

  void AddDefinitionBool(
    std::string const& name,
    bool);

  void AddCacheDefinition(
    std::string const& name,
    cmValue value,
    cmValue doc,
    cmStateEnums::CacheEntryType type,
    bool force = false);

  void AddCacheDefinition(
    std::string const& name,
    cmValue value,
    std::string const& doc,
    cmStateEnums::CacheEntryType type,
    bool force = false)
  {
    this->AddCacheDefinition(name, value, cmValue{ doc }, type, force);
  }

  void AddCacheDefinition(
    std::string const& name,
    std::string const& value,
    std::string const& doc,
    cmStateEnums::CacheEntryType type,
    bool force = false)
  {
    this->AddCacheDefinition(name, cmValue{ value }, cmValue{ doc }, type, force);
  }

  void RemoveDefinition(std::string const& name);
  void RemoveCacheDefinition(std::string const& name) const;

  void SetProjectName(std::string const& name);

  void InitCMAKE_CONFIGURATION_TYPES(std::string const& genDefault);

  std::string GetDefaultConfiguration() const;

  enum GeneratorConfigQuery
  {
    IncludeEmptyConfig, // Include "" aka noconfig
    ExcludeEmptyConfig, // Exclude "" aka noconfig
    OnlyMultiConfig,
  };

  /** Get the configurations for dependency checking.  */
  std::vector<std::string> GetGeneratorConfigs(GeneratorConfigQuery mode) const;

  cmTarget* AddLibrary(
    std::string const& libname,
    cmStateEnums::TargetType type,
    std::vector<std::string> const& srcs,
    bool excludeFromAll = false);

  void AddAlias(
    std::string const& libname,
    std::string const& tgt,
    bool globallyVisible = true);

  bool SetPolicy(
    cmPolicies::PolicyID id,
    cmPolicies::PolicyStatus status);

  bool SetPolicy(
    char const* id,
    cmPolicies::PolicyStatus status);

  cmPolicies::PolicyStatus GetPolicyStatus(
    cmPolicies::PolicyID id,
    bool parent_scope = false) const;

  bool SetPolicyVersion(
    std::string const& version_min,
    std::string const& version_max);

  void RecordPolicies(cmPolicies::PolicyMap& pm) const;

  class PolicyPushPop
  {
  public:
    PolicyPushPop(cmMakefile* m);
    ~PolicyPushPop();

    PolicyPushPop(PolicyPushPop const&) = delete;
    PolicyPushPop& operator=(PolicyPushPop const&) = delete;

  private:
    cmMakefile* m_pMakefile;
  };

  friend class PolicyPushPop;

  /** Helper class to push and pop variables scopes automatically. */
  class VariablePushPop
  {
  public:
    VariablePushPop(cmMakefile* m);
    ~VariablePushPop();

    VariablePushPop(VariablePushPop const&) = delete;
    VariablePushPop& operator=(VariablePushPop const&) = delete;

  private:
    cmMakefile* m_pMakefile;
  };

  std::string const& GetHomeDirectory() const;
  std::string const& GetHomeOutputDirectory() const;

  /**
   * Set CMAKE_SCRIPT_MODE_FILE variable when running a -P script.
   */
  void SetScriptModeFile(std::string const& scriptfile);

  /**
   * Set CMAKE_ARGC, CMAKE_ARGV0 ... variables.
   */
  void SetArgcArgv(std::vector<std::string> const& args);

  std::string const& GetCurrentSourceDirectory() const;
  std::string const& GetCurrentBinaryDirectory() const;

  //@}

  /**
   * Set a regular expression that include files must match
   * in order to be considered as part of the depend information.
   */
  void SetIncludeRegularExpression(std::string const& regex) { this->SetProperty("INCLUDE_REGULAR_EXPRESSION", regex); }
  std::string const& GetIncludeRegularExpression() const { return this->GetProperty("INCLUDE_REGULAR_EXPRESSION"); }

  /**
   * Set a regular expression that include files that are not found
   * must match in order to be considered a problem.
   */
  void SetComplainRegularExpression(std::string const& regex) { this->m_complainFileRegularExpression = regex; }
  std::string const& GetComplainRegularExpression() const { return this->m_complainFileRegularExpression; }

  // -- List of targets
  using cmTargetMap = std::unordered_map<std::string, cmTarget>;
  /** Get the target map */
  cmTargetMap& GetTargets() { return this->m_targets; }
  /** Get the target map - const version */
  cmTargetMap const& GetTargets() const { return this->m_targets; }

  std::vector<std::unique_ptr<cmTarget>> const& GetOwnedImportedTargets() const { return this->m_importedTargetsOwned; }
  std::vector<cmTarget*> GetImportedTargets() const;

  cmTarget* FindImportedTarget(std::string const& name) const;

  cmTarget* FindLocalNonAliasTarget(std::string const& name) const;

  /** Find a target to use in place of the given name.  The target
      returned may be imported or built within the project.  */
  cmTarget* FindTargetToUse(
    std::string const& name,
    cmStateEnums::TargetDomainSet domains = { cmStateEnums::TargetDomain::NATIVE,
                                              cmStateEnums::TargetDomain::ALIAS }) const;
  bool IsAlias(std::string const& name) const;

  std::map<
    std::string,
    std::string>
  GetAliasTargets() const
  {
    return this->m_aliasTargets;
  }

  /**
   * Mark include directories as system directories.
   */
  void AddSystemIncludeDirectories(std::set<std::string> const& incs);

  /** Get a cmSourceFile pointer for a given source name, if the name is
   *  not found, then a null pointer is returned.
   */
  cmSourceFile* GetSource(
    std::string const& sourceName,
    cmSourceFileLocationKind kind = cmSourceFileLocationKind::Ambiguous) const;

  /** Create the source file and return it. generated
   * indicates if it is a generated file, this is used in determining
   * how to create the source file instance e.g. name
   */
  cmSourceFile* CreateSource(
    std::string const& sourceName,
    bool generated = false,
    cmSourceFileLocationKind kind = cmSourceFileLocationKind::Ambiguous);

  /** Get a cmSourceFile pointer for a given source name, if the name is
   *  not found, then create the source file and return it. generated
   * indicates if it is a generated file, this is used in determining
   * how to create the source file instance e.g. name
   */
  cmSourceFile* GetOrCreateSource(
    std::string const& sourceName,
    bool generated = false,
    cmSourceFileLocationKind kind = cmSourceFileLocationKind::Ambiguous);

  /** Get a cmSourceFile pointer for a given source name and always mark the
   * file as generated, if the name is not found, then create the source file
   * and return it.
   */
  cmSourceFile* GetOrCreateGeneratedSource(std::string const& sourceName);

  void AddTargetObject(
    std::string const& tgtName,
    std::string const& objFile);

  /**
   * Given a variable name, return its value (as a string).
   * If the variable is not found in this makefile instance, the
   * cache is then queried.
   */
  cmValue GetDefinition(std::string const&) const;
  std::string const& GetSafeDefinition(std::string const&) const;
  std::string const& GetRequiredDefinition(std::string const& name) const;
  bool IsDefinitionSet(std::string const&) const;
  bool IsNormalDefinitionSet(std::string const&) const;
  /**
   * Get the list of all variables in the current space. If argument
   * cacheonly is specified and is greater than 0, then only cache
   * variables will be listed.
   */
  std::vector<std::string> GetDefinitions() const;

  /**
   * Test a boolean variable to see if it is true or false.
   * If the variable is not found in this makefile instance, the
   * cache is then queried.
   * Returns false if no entry defined.
   */
  bool IsOn(std::string const& name) const;
  bool IsSet(std::string const& name) const;

  /** Return whether the target platform is 32-bit. */
  bool PlatformIs32Bit() const;

  /** Return whether the target platform is 64-bit.  */
  bool PlatformIs64Bit() const;
  /** Return whether the target platform is x32.  */
  bool PlatformIsx32() const;

  /** Apple SDK Type */
  enum class AppleSDK
  {
    MacOS,
    IPhoneOS,
    IPhoneSimulator,
    AppleTVOS,
    AppleTVSimulator,
    WatchOS,
    WatchSimulator,
    XROS,
    XRSimulator,
  };

  /** What SDK type points CMAKE_OSX_SYSROOT to? */
  AppleSDK GetAppleSDKType() const;

  /** Return whether the target platform is Apple iOS.  */
  bool PlatformIsAppleEmbedded() const;

  /** Return whether the target platform is an Apple simulator.  */
  bool PlatformIsAppleSimulator() const;

  /** Return whether the target platform is an Apple catalyst.  */
  bool PlatformIsAppleCatalyst() const;

  /** Return whether the target platform supports generation of text base stubs
     (.tbd file) describing exports (Apple specific). */
  bool PlatformSupportsAppleTextStubs() const;

  /** Retrieve soname flag for the specified language if supported */
  char const* GetSONameFlag(std::string const& language) const;

  /**
   * Get a list of preprocessor define flags.
   */
  std::string GetDefineFlags() const { return this->m_defineFlags; }

  /**
   * Make sure CMake can write this file
   */
  bool CanIWriteThisFile(std::string const& fileName) const;

#if !defined(CMAKE_BOOTSTRAP)
  /**
   * Get the vector source groups.
   */
  std::vector<cmSourceGroup> const& GetSourceGroups() const { return this->m_sourceGroups; }

  /**
   * Get the source group
   */
  cmSourceGroup* GetSourceGroup(std::vector<std::string> const& name) const;

  /**
   * Add a root source group for consideration when adding a new source.
   */
  void AddSourceGroup(
    std::string const& name,
    char const* regex = nullptr);

  /**
   * Add a source group for consideration when adding a new source.
   * name is tokenized.
   */
  void AddSourceGroup(
    std::vector<std::string> const& name,
    char const* regex = nullptr);

  /**
   * Get and existing or create a new source group.
   */
  cmSourceGroup* GetOrCreateSourceGroup(std::vector<std::string> const& folders);

  /**
   * Get and existing or create a new source group.
   * The name will be tokenized.
   */
  cmSourceGroup* GetOrCreateSourceGroup(std::string const& name);

  /**
   * find what source group this source is in
   */
  cmSourceGroup* FindSourceGroup(
    std::string const& source,
    std::vector<cmSourceGroup>& groups) const;
#endif

  /**
   * Get the vector of list files on which this makefile depends
   */
  std::vector<std::string> const& GetListFiles() const { return this->m_listFiles; }
  //! When the file changes cmake will be re-run from the build system.
  void AddCMakeDependFile(std::string const& file) { this->m_listFiles.push_back(file); }
  void AddCMakeDependFilesFromUser();

  std::string FormatListFileStack() const;

  /**
   * Get the current context backtrace.
   */
  cmListFileBacktrace GetBacktrace() const;

  /**
   * Get the current stack of find_package calls.
   */
  cmFindPackageStack GetFindPackageStack() const;

  /**
   * Get the vector of  files created by this makefile
   */
  std::vector<std::string> const& GetOutputFiles() const { return this->m_outputFiles; }
  void AddCMakeOutputFile(std::string const& file) { this->m_outputFiles.push_back(file); }

  /**
   * Expand all defined variables in the string.
   * Defined variables come from the this->Definitions map.
   * They are expanded with ${var} where var is the
   * entry in the this->Definitions map.  Also \@var\@ is
   * expanded to match autoconf style expansions.
   */
  std::string const& ExpandVariablesInString(std::string& source) const;
  std::string const& ExpandVariablesInString(
    std::string& source,
    bool escapeQuotes,
    bool noEscapes,
    bool atOnly = false,
    char const* filename = nullptr,
    long line = -1,
    bool removeEmpty = false,
    bool replaceAt = false) const;

  /**
   * Remove any remaining variables in the string. Anything with ${var} or
   * \@var\@ will be removed.
   */
  void RemoveVariablesInString(
    std::string& source,
    bool atOnly = false) const;

  /**
   * Replace variables and #cmakedefine lines in the given string.
   * See cmConfigureFileCommand for details.
   */
  void ConfigureString(
    std::string const& input,
    std::string& output,
    bool atOnly,
    bool escapeQuotes) const;

  /**
   * Copy file but change lines according to ConfigureString
   */
  int ConfigureFile(
    std::string const& infile,
    std::string const& outfile,
    bool copyonly,
    bool atOnly,
    bool escapeQuotes,
    mode_t permissions = 0,
    cmNewLineStyle = cmNewLineStyle());

  enum class CommandMissingFromStack
  {
    No,
    Yes,
  };

  /**
   * Print a command's invocation
   */
  void PrintCommandTrace(
    cmListFileFunction const& lff,
    cmListFileBacktrace const& bt,
    CommandMissingFromStack missing = CommandMissingFromStack::No) const;

  /**
   * Set a callback that is invoked whenever ExecuteCommand is called.
   */
  void OnExecuteCommand(std::function<void()> callback);

  /**
   * Execute a single CMake command.  Returns true if the command
   * succeeded or false if it failed.
   */
  bool ExecuteCommand(
    cmListFileFunction const& lff,
    cmExecutionStatus& status,
    cm::optional<std::string> deferId = {});

  //! Enable support for named language, if nil then all languages are
  /// enabled.
  void EnableLanguage(
    std::vector<std::string> const& languages,
    bool optional);

  cmState* GetState() const;

/**
 * Get the variable watch. This is used to determine when certain variables
 * are accessed.
 */
#ifndef CMAKE_BOOTSTRAP
  cmVariableWatch* GetVariableWatch() const;
#endif

  //! Display progress or status message.
  void DisplayStatus(
    std::string const&,
    float) const;

  /**
   * Expand the given list file arguments into the full set after
   * variable replacement and list expansion.
   */
  bool ExpandArguments(
    std::vector<cmListFileArgument> const& inArgs,
    std::vector<std::string>& outArgs) const;
  bool ExpandArguments(
    std::vector<cmListFileArgument> const& inArgs,
    std::vector<cmExpandedCommandArgument>& outArgs) const;

  /**
   * Get the instance
   */
  CMake* GetCMakeInstance() const;
  cmMessenger* GetMessenger() const;
  cmGlobalGenerator* GetGlobalGenerator() const;

  /**
   * Get all the source files this makefile knows about
   */
  std::vector<std::unique_ptr<cmSourceFile>> const& GetSourceFiles() const { return this->m_sourceFiles; }

  std::vector<cmTarget*> const& GetOrderedTargets() const { return this->m_orderedTargets; }

  //! Add a new cmTest to the list of tests for this makefile.
  cmTest* CreateTest(std::string const& testName);

  /** Get a cmTest pointer for a given test name, if the name is
   *  not found, then a null pointer is returned.
   */
  cmTest* GetTest(std::string const& testName) const;

  /**
   * Get all tests that run under the given configuration.
   */
  void GetTests(
    std::string const& config,
    std::vector<cmTest*>& tests) const;

  /**
   * Return a location of a file in cmake or custom modules directory
   */
  std::string GetModulesFile(cm::string_view name) const
  {
    bool system;
    std::string debugBuffer;
    return this->GetModulesFile(name, system, false, debugBuffer);
  }

  /**
   * Return a location of a file in cmake or custom modules directory
   */
  std::string GetModulesFile(
    cm::string_view name,
    bool& system) const
  {
    std::string debugBuffer;
    return this->GetModulesFile(name, system, false, debugBuffer);
  }

  std::string GetModulesFile(
    cm::string_view name,
    bool& system,
    bool debug,
    std::string& debugBuffer) const;

  //! Set/Get a property of this directory
  void SetProperty(
    std::string const& prop,
    cmValue value);
  void SetProperty(
    std::string const& prop,
    std::nullptr_t)
  {
    this->SetProperty(prop, cmValue{ nullptr });
  }
  void SetProperty(
    std::string const& prop,
    std::string const& value)
  {
    this->SetProperty(prop, cmValue(value));
  }
  void AppendProperty(
    std::string const& prop,
    std::string const& value,
    bool asString = false);
  cmValue GetProperty(std::string const& prop) const;
  cmValue GetProperty(
    std::string const& prop,
    bool chain) const;
  bool GetPropertyAsBool(std::string const& prop) const;
  std::vector<std::string> GetPropertyKeys() const;

  //! Initialize a makefile from its parent
  void InitializeFromParent(cmMakefile* parent);

  void AddInstallGenerator(std::unique_ptr<cmInstallGenerator> g);

  std::vector<std::unique_ptr<cmInstallGenerator>>& GetInstallGenerators() { return this->m_installGenerators; }
  std::vector<std::unique_ptr<cmInstallGenerator>> const& GetInstallGenerators() const
  {
    return this->m_installGenerators;
  }

  void AddTestGenerator(std::unique_ptr<cmTestGenerator> g);

  std::vector<std::unique_ptr<cmTestGenerator>> const& GetTestGenerators() const { return this->m_testGenerators; }

  class FunctionPushPop
  {
  public:
    FunctionPushPop(
      cmMakefile* mf,
      std::string const& fileName,
      cmPolicies::PolicyMap const& pm);
    ~FunctionPushPop();

    FunctionPushPop(FunctionPushPop const&) = delete;
    FunctionPushPop& operator=(FunctionPushPop const&) = delete;

    void Quiet() { this->m_reportError = false; }

  private:
    cmMakefile* m_pMakefile;
    bool m_reportError = true;
  };

  class MacroPushPop
  {
  public:
    MacroPushPop(
      cmMakefile* mf,
      std::string const& fileName,
      cmPolicies::PolicyMap const& pm);
    ~MacroPushPop();

    MacroPushPop(MacroPushPop const&) = delete;
    MacroPushPop& operator=(MacroPushPop const&) = delete;

    void Quiet() { this->m_reportError = false; }

  private:
    cmMakefile* m_pMakefile;
    bool m_reportError = true;
  };

  void PushFunctionScope(
    std::string const& fileName,
    cmPolicies::PolicyMap const& pm);
  void PopFunctionScope(bool reportError);
  void PushMacroScope(
    std::string const& fileName,
    cmPolicies::PolicyMap const& pm);
  void PopMacroScope(bool reportError);
  void PushScope();
  void PopScope();
  void RaiseScope(
    std::string const& var,
    char const* value);
  void RaiseScope(
    std::string const& var,
    cmValue value)
  {
    this->RaiseScope(var, value.GetCStr());
  }
  void RaiseScope(std::vector<std::string> const& variables);

  // push and pop loop scopes
  void PushLoopBlockBarrier();
  void PopLoopBlockBarrier();

  bool IsImportedTargetGlobalScope() const;

  enum class ImportedTargetScope
  {
    Local,
    Global,
  };

  /** Helper class to manage whether imported packages
   * should be globally scoped based off the find package command
   */
  class SetGlobalTargetImportScope
  {
  public:
    SetGlobalTargetImportScope(
      cmMakefile* mk,
      ImportedTargetScope const scope)
      : m_pMakefile(mk)
    {
      if (scope == ImportedTargetScope::Global && !this->m_pMakefile->IsImportedTargetGlobalScope()) {
        this->m_pMakefile->m_currentImportedTargetScope = scope;
        this->m_set = true;
      } else {
        this->m_set = false;
      }
    }
    ~SetGlobalTargetImportScope()
    {
      if (this->m_set) {
        this->m_pMakefile->m_currentImportedTargetScope = ImportedTargetScope::Local;
      }
    }

  private:
    cmMakefile* m_pMakefile;
    bool m_set;
  };

  /** Helper class to push and pop scopes automatically.  */
  class ScopePushPop
  {
  public:
    ScopePushPop(cmMakefile* m)
      : m_pMakefile(m)
    {
      this->m_pMakefile->PushScope();
    }

    ~ScopePushPop() { this->m_pMakefile->PopScope(); }

    ScopePushPop(ScopePushPop const&) = delete;
    ScopePushPop& operator=(ScopePushPop const&) = delete;

  private:
    cmMakefile* m_pMakefile;
  };

  void IssueMessage(
    MessageType t,
    std::string const& text) const;
  Message::LogLevel GetCurrentLogLevel() const;

  /** Set whether or not to report a CMP0000 violation.  */
  void SetCheckCMP0000(bool b) { this->m_checkCMP0000 = b; }

  void IssueInvalidTargetNameError(std::string const& targetName) const;

  cmBTStringRange GetIncludeDirectoriesEntries() const;
  cmBTStringRange GetCompileOptionsEntries() const;
  cmBTStringRange GetCompileDefinitionsEntries() const;
  cmBTStringRange GetLinkOptionsEntries() const;
  cmBTStringRange GetLinkDirectoriesEntries() const;

  std::set<std::string> const& GetSystemIncludeDirectories() const { return this->m_systemIncludeDirectories; }

  bool PolicyOptionalWarningEnabled(std::string const& var) const;

  void PushLoopBlock();
  void PopLoopBlock();
  bool IsLoopBlock() const;

  void ClearMatches();
  void StoreMatches(cmsys::RegularExpression& re);

  cmStateSnapshot GetStateSnapshot() const;

  void EnforceDirectoryLevelRules() const;

  void AddEvaluationFile(
    std::string const& inputFile,
    std::string const& targetName,
    std::unique_ptr<cmCompiledGeneratorExpression> outputName,
    std::unique_ptr<cmCompiledGeneratorExpression> condition,
    std::string const& newLineCharacter,
    mode_t permissions,
    bool inputIsContent);
  std::vector<std::unique_ptr<cmGeneratorExpressionEvaluationFile>> const& GetEvaluationFiles() const;

  std::vector<std::unique_ptr<cmExportBuildFileGenerator>> const& GetExportBuildFileGenerators() const;
  void AddExportBuildFileGenerator(std::unique_ptr<cmExportBuildFileGenerator> gen);

  // Maintain a stack of package roots to allow nested PACKAGE_ROOT_PATH
  // searches
  std::deque<std::vector<std::string>> m_findPackageRootPathStack;

  class FindPackageStackRAII
  {
    cmMakefile* m_pMakefile;

  public:
    FindPackageStackRAII(
      cmMakefile* mf,
      std::string const& pkg);
    ~FindPackageStackRAII();

    FindPackageStackRAII(FindPackageStackRAII const&) = delete;
    FindPackageStackRAII& operator=(FindPackageStackRAII const&) = delete;
  };

  class DebugFindPkgRAII
  {
    cmMakefile* m_pMakefile;
    bool m_oldValue;

  public:
    DebugFindPkgRAII(
      cmMakefile* mf,
      std::string const& pkg);
    ~DebugFindPkgRAII();

    DebugFindPkgRAII(DebugFindPkgRAII const&) = delete;
    DebugFindPkgRAII& operator=(DebugFindPkgRAII const&) = delete;
  };

  class CallRAII
  {
  public:
    CallRAII(
      cmMakefile* mf,
      std::string const& file,
      cmExecutionStatus& status);
    ~CallRAII();

    CallRAII(CallRAII const&) = delete;
    CallRAII& operator=(CallRAII const&) = delete;

  protected:
    CallRAII(
      cmMakefile* mf,
      cmListFileContext const& lfc,
      cmExecutionStatus& status);

    cmMakefile* Detach();

    cmMakefile* m_pMakefile;
  };

  bool GetDebugFindPkgMode() const;

  void MaybeWarnCMP0074(
    std::string const& rootVar,
    cmValue rootDef,
    cm::optional<std::string> const& rootEnv);
  void MaybeWarnCMP0144(
    std::string const& rootVAR,
    cmValue rootDEF,
    cm::optional<std::string> const& rootENV);
  void MaybeWarnUninitialized(
    std::string const& variable,
    char const* sourceFilename) const;
  bool IsProjectFile(char const* filename) const;

  size_t GetRecursionDepthLimit() const;

  size_t GetRecursionDepth() const;
  void SetRecursionDepth(size_t recursionDepth);

  std::string NewDeferId() const;
  bool DeferCall(
    std::string id,
    std::string fileName,
    cmListFileFunction lff);
  bool DeferCancelCall(std::string const& id);
  cm::optional<std::string> DeferGetCallIds() const;
  cm::optional<std::string> DeferGetCall(std::string const& id) const;

protected:
  // add link libraries and directories to the target
  void AddGlobalLinkInformation(cmTarget& target);

  // libraries, classes, and executables
  //    TODO: why mutable.
  mutable cmTargetMap m_targets;
  std::map<std::string, std::string> m_aliasTargets;

  std::vector<cmTarget*> m_orderedTargets;

  std::vector<std::unique_ptr<cmSourceFile>> m_sourceFiles;

  // Because cmSourceFile names are compared in a fuzzy way (see
  // cmSourceFileLocation::Match()) we can't have a straight mapping from
  // filename to cmSourceFile.  To make lookups more efficient we store the
  // Name portion of the cmSourceFileLocation and then compare on the list of
  // cmSourceFiles that might match that name.  Note that on platforms which
  // have a case-insensitive filesystem we store the key in all lowercase.
  using SourceFileMap = std::unordered_map<std::string, std::vector<cmSourceFile*>>;
  SourceFileMap m_sourceFileSearchIndex;

  // For "Known" paths we can store a direct filename to cmSourceFile map
  std::unordered_map<std::string, cmSourceFile*> m_knownFileSearchIndex;

  // Tests
  std::map<std::string, std::unique_ptr<cmTest>> m_tests;

  // The set of include directories that are marked as system include
  // directories.
  std::set<std::string> m_systemIncludeDirectories;

  std::vector<std::string> m_listFiles;
  std::vector<std::string> m_outputFiles;

  std::vector<std::unique_ptr<cmInstallGenerator>> m_installGenerators;
  std::vector<std::unique_ptr<cmTestGenerator>> m_testGenerators;

  std::string m_complainFileRegularExpression;
  std::string m_defineFlags;

#if !defined(CMAKE_BOOTSTRAP)
  std::vector<cmSourceGroup> m_sourceGroups;
  size_t m_objectLibrariesSourceGroupIndex;
#endif

  cmGlobalGenerator* m_pGlobalGenerator;

  bool IsFunctionBlocked(
    cmListFileFunction const& lff,
    cmExecutionStatus& status);

private:
  cmStateSnapshot m_stateSnapshot;
  cmListFileBacktrace m_backtrace;
  size_t m_recursionDepth = 0;

  struct DeferCommand
  {
    // Id is empty for an already-executed or canceled operation.
    std::string m_id;
    std::string m_filePath;
    cmListFileFunction m_command;
  };
  struct DeferCommands
  {
    std::vector<DeferCommand> m_commands;
  };
  std::unique_ptr<DeferCommands> m_defer;
  bool m_deferRunning = false;

  void DoGenerate(cmLocalGenerator& lg);

  void RunListFile(
    cmListFile const& listFile,
    std::string const& filenametoread,
    DeferCommands* defer = nullptr);

  bool ParseDefineFlag(
    std::string const& definition,
    bool remove);

  bool EnforceUniqueDir(
    std::string const& srcPath,
    std::string const& binPath) const;

  std::function<void()> m_executeCommandCallback;
  using FunctionBlockerPtr = std::unique_ptr<cmFunctionBlocker>;
  using FunctionBlockersType = std::stack<FunctionBlockerPtr, std::vector<FunctionBlockerPtr>>;
  FunctionBlockersType m_functionBlockers;
  std::vector<FunctionBlockersType::size_type> m_functionBlockerBarriers;
  void PushFunctionBlockerBarrier();
  void PopFunctionBlockerBarrier(bool reportError = true);

  std::stack<int> m_loopBlockCounter;

  mutable cmsys::RegularExpression m_cmDefineRegex;
  mutable cmsys::RegularExpression m_cmDefine01Regex;
  mutable cmsys::RegularExpression m_cmNamedCurly;

  std::vector<cmMakefile*> m_unconfiguredDirectories;
  std::vector<std::unique_ptr<cmExportBuildFileGenerator>> m_exportBuildFileGenerators;

  std::vector<std::unique_ptr<cmGeneratorExpressionEvaluationFile>> m_evaluationFiles;

  class CallScope;
  friend class CallScope;

  std::vector<cmExecutionStatus*> m_executionStatusStack;
  friend class cmParseFileScope;

  std::vector<std::unique_ptr<cmTarget>> m_importedTargetsOwned;
  using TargetMap = std::unordered_map<std::string, cmTarget*>;
  TargetMap m_importedTargets;

  // Internal policy stack management.
  void PushPolicy(
    bool weak = false,
    cmPolicies::PolicyMap const& pm = cmPolicies::PolicyMap());
  void PopPolicy();
  void PopSnapshot(bool reportError = true);
  friend bool cmCMakePolicyCommand(
    std::vector<std::string> const& args,
    cmExecutionStatus& status);
  class IncludeScope;
  friend class IncludeScope;

  class ListFileScope;
  friend class ListFileScope;

  class DeferScope;
  friend class DeferScope;

  class DeferCallScope;
  friend class DeferCallScope;

  class BuildsystemFileScope;
  friend class BuildsystemFileScope;

  MessageType ExpandVariablesInStringImpl(
    std::string& errorstr,
    std::string& source,
    bool escapeQuotes,
    bool noEscapes,
    bool atOnly,
    char const* filename,
    long line,
    bool replaceAt) const;

  bool ValidateCustomCommand(cmCustomCommandLines const& commandLines) const;

  void CreateGeneratedOutputs(std::vector<std::string> const& outputs);

  std::vector<BT<GeneratorAction>> m_generatorActions;
  bool m_generatorActionsInvoked = false;

  cmFindPackageStack m_findPackageStack;
  unsigned int m_findPackageStackNextIndex = 0;

  bool m_debugFindPkg = false;

  bool m_checkSystemVars;
  bool m_checkCMP0000;
  std::set<std::string> m_warnedCMP0074;
  std::set<std::string> m_warnedCMP0144;
  bool m_isSourceFileTryCompile;
  ImportedTargetScope m_currentImportedTargetScope = ImportedTargetScope::Local;
};
