/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cm/optional>

#include "cmDefinitions.h"
#include "cmDependencyProvider.h"
#include "cmLinkedTree.h"
#include "cmPolicies.h"
#include "cmProperty.h"
#include "cmPropertyDefinition.h"
#include "cmPropertyMap.h"
#include "cmStatePrivate.h"
#include "cmStateTypes.h"
#include "cmValue.h"

class cmCacheManager;
class cmGlobVerificationManager;
class cmMakefile;
class cmStateSnapshot;
class cmMessenger;
class cmExecutionStatus;
class cmListFileBacktrace;
struct cmGlobCacheEntry;
struct cmListFileArgument;

template <typename T>
class BT;

class cmState
{
  friend class cmStateSnapshot;

public:
  enum Mode
  {
    Unknown,
    Project,
    Script,
    FindPackage,
    CTest,
    CPack,
    Help
  };

  enum class ProjectKind
  {
    Normal,
    TryCompile,
  };

  cmState(
    Mode mode,
    ProjectKind projectKind = ProjectKind::Normal);
  ~cmState();

  cmState(cmState const&) = delete;
  cmState& operator=(cmState const&) = delete;

  static std::string const& GetTargetTypeName(cmStateEnums::TargetType targetType);

  cmStateSnapshot CreateBaseSnapshot();
  cmStateSnapshot CreateBuildsystemDirectorySnapshot(cmStateSnapshot const& originSnapshot);
  cmStateSnapshot CreateDeferCallSnapshot(
    cmStateSnapshot const& originSnapshot,
    std::string const& fileName);
  cmStateSnapshot CreateFunctionCallSnapshot(
    cmStateSnapshot const& originSnapshot,
    std::string const& fileName);
  cmStateSnapshot CreateMacroCallSnapshot(
    cmStateSnapshot const& originSnapshot,
    std::string const& fileName);
  cmStateSnapshot CreateIncludeFileSnapshot(
    cmStateSnapshot const& originSnapshot,
    std::string const& fileName);
  cmStateSnapshot CreateVariableScopeSnapshot(cmStateSnapshot const& originSnapshot);
  cmStateSnapshot CreateInlineListFileSnapshot(
    cmStateSnapshot const& originSnapshot,
    std::string const& fileName);
  cmStateSnapshot CreatePolicyScopeSnapshot(cmStateSnapshot const& originSnapshot);
  cmStateSnapshot Pop(cmStateSnapshot const& originSnapshot);

  static cmStateEnums::CacheEntryType StringToCacheEntryType(std::string const&);
  static bool StringToCacheEntryType(
    std::string const&,
    cmStateEnums::CacheEntryType& type);
  static std::string const& CacheEntryTypeToString(cmStateEnums::CacheEntryType);
  static bool IsCacheEntryType(std::string const& key);

  bool LoadCache(
    std::string const& path,
    bool internal,
    std::set<std::string>& excludes,
    std::set<std::string>& includes);

  bool SaveCache(
    std::string const& path,
    cmMessenger* messenger);

  bool DeleteCache(std::string const& path);

  bool IsCacheLoaded() const;

  std::vector<std::string> GetCacheEntryKeys() const;
  cmValue GetCacheEntryValue(std::string const& key) const;
  std::string GetSafeCacheEntryValue(std::string const& key) const;
  cmValue GetInitializedCacheValue(std::string const& key) const;
  cmStateEnums::CacheEntryType GetCacheEntryType(std::string const& key) const;
  void SetCacheEntryValue(
    std::string const& key,
    std::string const& value);

  void RemoveCacheEntry(std::string const& key);

  void SetCacheEntryProperty(
    std::string const& key,
    std::string const& propertyName,
    std::string const& value);
  void SetCacheEntryBoolProperty(
    std::string const& key,
    std::string const& propertyName,
    bool value);
  std::vector<std::string> GetCacheEntryPropertyList(std::string const& key);
  cmValue GetCacheEntryProperty(
    std::string const& key,
    std::string const& propertyName);
  bool GetCacheEntryPropertyAsBool(
    std::string const& key,
    std::string const& propertyName);
  void AppendCacheEntryProperty(
    std::string const& key,
    std::string const& property,
    std::string const& value,
    bool asString = false);
  void RemoveCacheEntryProperty(
    std::string const& key,
    std::string const& propertyName);

  //! Break up a line like VAR:type="value" into var, type and value
  static bool ParseCacheEntry(
    std::string const& entry,
    std::string& var,
    std::string& value,
    cmStateEnums::CacheEntryType& type);

  cmStateSnapshot Reset();
  // Define a property
  void DefineProperty(
    std::string const& name,
    cmProperty::ScopeType scope,
    std::string const& ShortDescription,
    std::string const& FullDescription,
    bool chain = false,
    std::string const& initializeFromVariable = "");

  // get property definition
  cmPropertyDefinition const* GetPropertyDefinition(
    std::string const& name,
    cmProperty::ScopeType scope) const;

  cmPropertyDefinitionMap const& GetPropertyDefinitions() const { return m_propertyDefinitions; }

  bool IsPropertyChained(
    std::string const& name,
    cmProperty::ScopeType scope) const;

  void SetLanguageEnabled(std::string const& l);
  bool GetLanguageEnabled(std::string const& l) const;
  std::vector<std::string> GetEnabledLanguages() const;
  void SetEnabledLanguages(std::vector<std::string> const& langs);
  void ClearEnabledLanguages();

  bool GetIsGeneratorMultiConfig() const;
  void SetIsGeneratorMultiConfig(bool b);

  using m_command = std::function<bool(std::vector<cmListFileArgument> const&, cmExecutionStatus&)>;
  using BuiltinCommand = bool (*)(
    std::vector<std::string> const&,
    cmExecutionStatus&);

  // Returns a command from its name, case insensitive, or nullptr
  m_command GetCommand(std::string const& name) const;
  // Returns a command from its name, or nullptr
  m_command GetCommandByExactName(std::string const& name) const;

  void AddBuiltinCommand(
    std::string const& name,
    m_command command);
  void AddBuiltinCommand(
    std::string const& name,
    BuiltinCommand command);
  void AddFlowControlCommand(
    std::string const& name,
    m_command command);
  void AddFlowControlCommand(
    std::string const& name,
    BuiltinCommand command);
  void AddDisallowedCommand(
    std::string const& name,
    BuiltinCommand command,
    cmPolicies::PolicyID policy,
    char const* message,
    char const* additionalWarning = nullptr);
  void AddRemovedCommand(
    std::string const& name,
    std::string const& message);
  void AddUnexpectedCommand(
    std::string const& name,
    char const* error);
  void AddUnexpectedFlowControlCommand(
    std::string const& name,
    char const* error);
  bool AddScriptedCommand(
    std::string const& name,
    BT<m_command> command,
    cmMakefile& mf);
  void RemoveBuiltinCommand(std::string const& name);
  void RemoveUserDefinedCommands();
  std::vector<std::string> GetCommandNames() const;

  void SetGlobalProperty(
    std::string const& prop,
    std::string const& value);
  void SetGlobalProperty(
    std::string const& prop,
    cmValue value);
  void AppendGlobalProperty(
    std::string const& prop,
    std::string const& value,
    bool asString = false);
  cmValue GetGlobalProperty(std::string const& prop);
  bool GetGlobalPropertyAsBool(std::string const& prop);

  std::string const& GetSourceDirectory() const;
  void SetSourceDirectory(std::string const& sourceDirectory);
  std::string const& GetBinaryDirectory() const;
  void SetBinaryDirectory(std::string const& binaryDirectory);

  void SetWindowsShell(bool windowsShell);
  bool UseWindowsShell() const;
  void SetWindowsVSIDE(bool windowsVSIDE);
  bool UseWindowsVSIDE() const;
  void SetGhsMultiIDE(bool ghsMultiIDE);
  bool UseGhsMultiIDE() const;
  void SetBorlandMake(bool borlandMake);
  bool UseBorlandMake() const;
  void SetWatcomWMake(bool watcomWMake);
  bool UseWatcomWMake() const;
  void SetMinGWMake(bool minGWMake);
  bool UseMinGWMake() const;
  void SetNMake(bool nMake);
  bool UseNMake() const;
  void SetMSYSShell(bool mSYSShell);
  bool UseMSYSShell() const;
  void SetNinja(bool ninja);
  bool UseNinja() const;
  void SetNinjaMulti(bool ninjaMulti);
  bool UseNinjaMulti() const;

  unsigned int GetCacheMajorVersion() const;
  unsigned int GetCacheMinorVersion() const;

  Mode GetMode() const;
  std::string GetModeString() const;

  static std::string ModeToString(Mode mode);

  ProjectKind GetProjectKind() const;

  void ClearDependencyProvider() { m_dependencyProvider.reset(); }
  void SetDependencyProvider(cmDependencyProvider provider) { m_dependencyProvider = std::move(provider); }
  cm::optional<cmDependencyProvider> const& GetDependencyProvider() const { return m_dependencyProvider; }
  m_command GetDependencyProviderCommand(cmDependencyProvider::Method method) const;

  void SetInTopLevelIncludes(bool inTopLevelIncludes) { m_processingTopLevelIncludes = inTopLevelIncludes; }
  bool InTopLevelIncludes() const { return m_processingTopLevelIncludes; }

private:
  friend class CMake;

  void AddCacheEntry(
    std::string const& key,
    cmValue value,
    std::string const& helpString,
    cmStateEnums::CacheEntryType type);

  bool DoWriteGlobVerifyTarget() const;
  std::string const& GetGlobVerifyScript() const;
  std::string const& GetGlobVerifyStamp() const;
  bool SaveVerificationScript(
    std::string const& path,
    cmMessenger* messenger);
  void AddGlobCacheEntry(
    cmGlobCacheEntry const& entry,
    std::string const& variable,
    cmListFileBacktrace const& bt,
    cmMessenger* messenger);
  std::vector<cmGlobCacheEntry> GetGlobCacheEntries() const;

  cmPropertyDefinitionMap m_propertyDefinitions;
  std::vector<std::string> m_enabledLanguages;
  std::unordered_map<std::string, m_command> m_builtinCommands;
  std::unordered_map<std::string, m_command> m_scriptedCommands;
  std::unordered_set<std::string> m_flowControlCommands;
  cmPropertyMap m_globalProperties;
  std::unique_ptr<cmCacheManager> m_cacheManager;
  std::unique_ptr<cmGlobVerificationManager> m_globVerificationManager;

  cmLinkedTree<cmStateDetail::BuildsystemDirectoryStateType> m_buildsystemDirectory;

  cmLinkedTree<std::string> m_executionListFiles;

  cmLinkedTree<cmStateDetail::PolicyStackEntry> m_policyStack;
  cmLinkedTree<cmStateDetail::SnapshotDataType> m_snapshotData;
  cmLinkedTree<cmDefinitions> m_varTree;

  std::string m_sourceDirectory;
  std::string m_binaryDirectory;
  bool m_isGeneratorMultiConfig = false;
  bool m_windowsShell = false;
  bool m_windowsVSIDE = false;
  bool m_ghsMultiIDE = false;
  bool m_borlandMake = false;
  bool m_watcomWMake = false;
  bool m_minGWMake = false;
  bool m_nmake = false;
  bool m_msysShell = false;
  bool m_ninja = false;
  bool m_ninjaMulti = false;
  Mode m_stateMode = Unknown;
  ProjectKind m_stateProjectKind = ProjectKind::Normal;
  cm::optional<cmDependencyProvider> m_dependencyProvider;
  bool m_processingTopLevelIncludes = false;
};
