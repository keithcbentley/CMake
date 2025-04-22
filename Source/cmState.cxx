/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmState.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <utility>

#include <cm/memory>

#include "cmsys/RegularExpression.hxx"

#include "cmCacheManager.h"
#include "cmDefinitions.h"
#include "cmExecutionStatus.h"
#include "cmGlobCacheEntry.h"
#include "cmGlobVerificationManager.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmStatePrivate.h"
#include "cmStateSnapshot.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmake.h"

cmState::cmState(
  Mode mode,
  ProjectKind projectKind)
  : m_stateMode(mode)
  , m_stateProjectKind(projectKind)
{
  m_cacheManager = cm::make_unique<cmCacheManager>();
  m_globVerificationManager = cm::make_unique<cmGlobVerificationManager>();
}

cmState::~cmState() = default;

std::string const& cmState::GetTargetTypeName(cmStateEnums::TargetType targetType)
{
#define MAKE_STATIC_PROP(PROP) static const std::string prop##PROP = #PROP
  MAKE_STATIC_PROP(STATIC_LIBRARY);
  MAKE_STATIC_PROP(MODULE_LIBRARY);
  MAKE_STATIC_PROP(SHARED_LIBRARY);
  MAKE_STATIC_PROP(OBJECT_LIBRARY);
  MAKE_STATIC_PROP(EXECUTABLE);
  MAKE_STATIC_PROP(UTILITY);
  MAKE_STATIC_PROP(GLOBAL_TARGET);
  MAKE_STATIC_PROP(INTERFACE_LIBRARY);
  MAKE_STATIC_PROP(UNKNOWN_LIBRARY);
  static std::string const propEmpty;
#undef MAKE_STATIC_PROP

  switch (targetType) {
    case cmStateEnums::STATIC_LIBRARY:
      return propSTATIC_LIBRARY;
    case cmStateEnums::MODULE_LIBRARY:
      return propMODULE_LIBRARY;
    case cmStateEnums::SHARED_LIBRARY:
      return propSHARED_LIBRARY;
    case cmStateEnums::OBJECT_LIBRARY:
      return propOBJECT_LIBRARY;
    case cmStateEnums::EXECUTABLE:
      return propEXECUTABLE;
    case cmStateEnums::UTILITY:
      return propUTILITY;
    case cmStateEnums::GLOBAL_TARGET:
      return propGLOBAL_TARGET;
    case cmStateEnums::INTERFACE_LIBRARY:
      return propINTERFACE_LIBRARY;
    case cmStateEnums::UNKNOWN_LIBRARY:
      return propUNKNOWN_LIBRARY;
  }
  assert(false && "Unexpected target type");
  return propEmpty;
}

static std::array<std::string, 7> const cmCacheEntryTypes = { { "BOOL", "PATH", "FILEPATH", "STRING", "INTERNAL",
                                                                "STATIC", "UNINITIALIZED" } };

std::string const& cmState::CacheEntryTypeToString(cmStateEnums::CacheEntryType type)
{
  if (type < cmStateEnums::BOOL || type > cmStateEnums::UNINITIALIZED) {
    type = cmStateEnums::UNINITIALIZED;
  }
  return cmCacheEntryTypes[type];
}

cmStateEnums::CacheEntryType cmState::StringToCacheEntryType(std::string const& s)
{
  cmStateEnums::CacheEntryType type = cmStateEnums::STRING;
  StringToCacheEntryType(s, type);
  return type;
}

bool cmState::StringToCacheEntryType(
  std::string const& s,
  cmStateEnums::CacheEntryType& type)
{
  // NOLINTNEXTLINE(readability-qualified-auto)
  auto const entry = std::find(cmCacheEntryTypes.begin(), cmCacheEntryTypes.end(), s);
  if (entry != cmCacheEntryTypes.end()) {
    type = static_cast<cmStateEnums::CacheEntryType>(entry - cmCacheEntryTypes.begin());
    return true;
  }
  return false;
}

bool cmState::IsCacheEntryType(std::string const& key)
{
  return std::any_of(
    cmCacheEntryTypes.begin(), cmCacheEntryTypes.end(), [&key](std::string const& i) -> bool { return key == i; });
}

bool cmState::LoadCache(
  std::string const& path,
  bool internal,
  std::set<std::string>& excludes,
  std::set<std::string>& includes)
{
  return m_cacheManager->LoadCache(path, internal, excludes, includes);
}

bool cmState::SaveCache(
  std::string const& path,
  cmMessenger* messenger)
{
  return m_cacheManager->SaveCache(path, messenger);
}

bool cmState::DeleteCache(std::string const& path)
{
  return m_cacheManager->DeleteCache(path);
}

bool cmState::IsCacheLoaded() const
{
  return m_cacheManager->IsCacheLoaded();
}

std::vector<std::string> cmState::GetCacheEntryKeys() const
{
  return m_cacheManager->GetCacheEntryKeys();
}

cmValue cmState::GetCacheEntryValue(std::string const& key) const
{
  return m_cacheManager->GetCacheEntryValue(key);
}

std::string cmState::GetSafeCacheEntryValue(std::string const& key) const
{
  if (cmValue val = GetCacheEntryValue(key)) {
    return *val;
  }
  return std::string();
}

cmValue cmState::GetInitializedCacheValue(std::string const& key) const
{
  return m_cacheManager->GetInitializedCacheValue(key);
}

cmStateEnums::CacheEntryType cmState::GetCacheEntryType(std::string const& key) const
{
  return m_cacheManager->GetCacheEntryType(key);
}

void cmState::SetCacheEntryValue(
  std::string const& key,
  std::string const& value)
{
  m_cacheManager->SetCacheEntryValue(key, value);
}

void cmState::SetCacheEntryProperty(
  std::string const& key,
  std::string const& propertyName,
  std::string const& value)
{
  m_cacheManager->SetCacheEntryProperty(key, propertyName, value);
}

void cmState::SetCacheEntryBoolProperty(
  std::string const& key,
  std::string const& propertyName,
  bool value)
{
  m_cacheManager->SetCacheEntryBoolProperty(key, propertyName, value);
}

std::vector<std::string> cmState::GetCacheEntryPropertyList(std::string const& key)
{
  return m_cacheManager->GetCacheEntryPropertyList(key);
}

cmValue cmState::GetCacheEntryProperty(
  std::string const& key,
  std::string const& propertyName)
{
  return m_cacheManager->GetCacheEntryProperty(key, propertyName);
}

bool cmState::GetCacheEntryPropertyAsBool(
  std::string const& key,
  std::string const& propertyName)
{
  return m_cacheManager->GetCacheEntryPropertyAsBool(key, propertyName);
}

void cmState::AddCacheEntry(
  std::string const& key,
  cmValue value,
  std::string const& helpString,
  cmStateEnums::CacheEntryType type)
{
  m_cacheManager->AddCacheEntry(key, value, helpString, type);
}

bool cmState::DoWriteGlobVerifyTarget() const
{
  return m_globVerificationManager->DoWriteVerifyTarget();
}

std::string const& cmState::GetGlobVerifyScript() const
{
  return m_globVerificationManager->GetVerifyScript();
}

std::string const& cmState::GetGlobVerifyStamp() const
{
  return m_globVerificationManager->GetVerifyStamp();
}

bool cmState::SaveVerificationScript(
  std::string const& path,
  cmMessenger* messenger)
{
  return m_globVerificationManager->SaveVerificationScript(path, messenger);
}

void cmState::AddGlobCacheEntry(
  cmGlobCacheEntry const& entry,
  std::string const& variable,
  cmListFileBacktrace const& backtrace,
  cmMessenger* messenger)
{
  m_globVerificationManager->AddCacheEntry(entry, variable, backtrace, messenger);
}

std::vector<cmGlobCacheEntry> cmState::GetGlobCacheEntries() const
{
  return m_globVerificationManager->GetCacheEntries();
}

void cmState::RemoveCacheEntry(std::string const& key)
{
  m_cacheManager->RemoveCacheEntry(key);
}

void cmState::AppendCacheEntryProperty(
  std::string const& key,
  std::string const& property,
  std::string const& value,
  bool asString)
{
  m_cacheManager->AppendCacheEntryProperty(key, property, value, asString);
}

void cmState::RemoveCacheEntryProperty(
  std::string const& key,
  std::string const& propertyName)
{
  m_cacheManager->RemoveCacheEntryProperty(key, propertyName);
}

cmStateSnapshot cmState::Reset()
{
  m_globalProperties.Clear();
  m_propertyDefinitions = {};
  m_globVerificationManager->Reset();

  cmStateDetail::PositionType pos = m_snapshotData.Truncate();
  m_executionListFiles.Truncate();

  {
    cmLinkedTree<cmStateDetail::BuildsystemDirectoryStateType>::iterator it = m_buildsystemDirectory.Truncate();

    cmStateDetail::BuildsystemDirectoryStateType newState;
    newState.Location = std::move(it->Location);
    newState.OutputLocation = std::move(it->OutputLocation);
    newState.CurrentScope = pos;
    *it = std::move(newState);
  }

  m_policyStack.Clear();
  pos->Policies = m_policyStack.Root();
  pos->PolicyRoot = m_policyStack.Root();
  pos->PolicyScope = m_policyStack.Root();
  assert(pos->Policies.IsValid());
  assert(pos->PolicyRoot.IsValid());

  {
    std::string srcDir = *cmDefinitions::Get("CMAKE_SOURCE_DIR", pos->Vars, pos->Root);
    std::string binDir = *cmDefinitions::Get("CMAKE_BINARY_DIR", pos->Vars, pos->Root);
    m_varTree.Clear();
    pos->Vars = m_varTree.Push(m_varTree.Root());
    pos->Parent = m_varTree.Root();
    pos->Root = m_varTree.Root();

    pos->Vars->m_set("CMAKE_SOURCE_DIR", srcDir);
    pos->Vars->m_set("CMAKE_BINARY_DIR", binDir);
  }

  DefineProperty("RULE_LAUNCH_COMPILE", cmProperty::DIRECTORY, "", "", true);
  DefineProperty("RULE_LAUNCH_LINK", cmProperty::DIRECTORY, "", "", true);
  DefineProperty("RULE_LAUNCH_CUSTOM", cmProperty::DIRECTORY, "", "", true);

  DefineProperty("RULE_LAUNCH_COMPILE", cmProperty::TARGET, "", "", true);
  DefineProperty("RULE_LAUNCH_LINK", cmProperty::TARGET, "", "", true);
  DefineProperty("RULE_LAUNCH_CUSTOM", cmProperty::TARGET, "", "", true);

  return { this, pos };
}

void cmState::DefineProperty(
  std::string const& name,
  cmProperty::ScopeType scope,
  std::string const& ShortDescription,
  std::string const& FullDescription,
  bool chained,
  std::string const& initializeFromVariable)
{
  m_propertyDefinitions.DefineProperty(name, scope, ShortDescription, FullDescription, chained, initializeFromVariable);
}

cmPropertyDefinition const* cmState::GetPropertyDefinition(
  std::string const& name,
  cmProperty::ScopeType scope) const
{
  return m_propertyDefinitions.GetPropertyDefinition(name, scope);
}

bool cmState::IsPropertyChained(
  std::string const& name,
  cmProperty::ScopeType scope) const
{
  if (auto const* def = GetPropertyDefinition(name, scope)) {
    return def->IsChained();
  }
  return false;
}

void cmState::SetLanguageEnabled(std::string const& l)
{
  auto it = std::lower_bound(m_enabledLanguages.begin(), m_enabledLanguages.end(), l);
  if (it == m_enabledLanguages.end() || *it != l) {
    m_enabledLanguages.insert(it, l);
  }
}

bool cmState::GetLanguageEnabled(std::string const& l) const
{
  return std::binary_search(m_enabledLanguages.begin(), m_enabledLanguages.end(), l);
}

std::vector<std::string> cmState::GetEnabledLanguages() const
{
  return m_enabledLanguages;
}

void cmState::SetEnabledLanguages(std::vector<std::string> const& langs)
{
  m_enabledLanguages = langs;
}

void cmState::ClearEnabledLanguages()
{
  m_enabledLanguages.clear();
}

bool cmState::GetIsGeneratorMultiConfig() const
{
  return m_isGeneratorMultiConfig;
}

void cmState::SetIsGeneratorMultiConfig(bool b)
{
  m_isGeneratorMultiConfig = b;
}

void cmState::AddBuiltinCommand(
  std::string const& name,
  m_command command)
{
  assert(name == cmSystemTools::LowerCase(name));
  assert(m_builtinCommands.find(name) == m_builtinCommands.end());
  m_builtinCommands.emplace(name, std::move(command));
}

static bool InvokeBuiltinCommand(
  cmState::BuiltinCommand command,
  std::vector<cmListFileArgument> const& args,
  cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();
  std::vector<std::string> expandedArguments;
  if (!mf.ExpandArguments(args, expandedArguments)) {
    // There was an error expanding arguments.  It was already
    // reported, so we can skip this command without error.
    return true;
  }
  return command(expandedArguments, status);
}

void cmState::AddBuiltinCommand(
  std::string const& name,
  BuiltinCommand command)
{
  AddBuiltinCommand(name, [command](std::vector<cmListFileArgument> const& args, cmExecutionStatus& status) -> bool {
    return InvokeBuiltinCommand(command, args, status);
  });
}

void cmState::AddFlowControlCommand(
  std::string const& name,
  m_command command)
{
  m_flowControlCommands.insert(name);
  AddBuiltinCommand(name, std::move(command));
}

void cmState::AddFlowControlCommand(
  std::string const& name,
  BuiltinCommand command)
{
  m_flowControlCommands.insert(name);
  AddBuiltinCommand(name, command);
}

void cmState::AddDisallowedCommand(
  std::string const& name,
  BuiltinCommand command,
  cmPolicies::PolicyID policy,
  char const* message,
  char const* additionalWarning)
{
  AddBuiltinCommand(
    name,
    [command, policy, message,
     additionalWarning](std::vector<cmListFileArgument> const& args, cmExecutionStatus& status) -> bool {
      cmMakefile& mf = status.GetMakefile();
      switch (mf.GetPolicyStatus(policy)) {
        case cmPolicies::WARN: {
          std::string warning = cmPolicies::GetPolicyWarning(policy);
          if (additionalWarning) {
            warning = cmStrCat(warning, '\n', additionalWarning);
          }
          mf.IssueMessage(MessageType::AUTHOR_WARNING, warning);
        }
          CM_FALLTHROUGH;
        case cmPolicies::OLD:
          break;
        case cmPolicies::NEW:
          mf.IssueMessage(MessageType::FATAL_ERROR, message);
          return true;
      }
      return InvokeBuiltinCommand(command, args, status);
    });
}

void cmState::AddRemovedCommand(
  std::string const& name,
  std::string const& message)
{
  AddBuiltinCommand(name, [message](std::vector<cmListFileArgument> const&, cmExecutionStatus& status) -> bool {
    status.GetMakefile().IssueMessage(MessageType::FATAL_ERROR, message);
    return true;
  });
}

void cmState::AddUnexpectedCommand(
  std::string const& name,
  char const* error)
{
  AddBuiltinCommand(name, [name, error](std::vector<cmListFileArgument> const&, cmExecutionStatus& status) -> bool {
    cmValue versionValue = status.GetMakefile().GetDefinition("CMAKE_MINIMUM_REQUIRED_VERSION");
    if (name == "endif" && (!versionValue || atof(versionValue->c_str()) <= 1.4)) {
      return true;
    }
    status.SetError(error);
    return false;
  });
}

void cmState::AddUnexpectedFlowControlCommand(
  std::string const& name,
  char const* error)
{
  m_flowControlCommands.insert(name);
  AddUnexpectedCommand(name, error);
}

bool cmState::AddScriptedCommand(
  std::string const& name,
  BT<m_command> command,
  cmMakefile& mf)
{
  std::string sName = cmSystemTools::LowerCase(name);

  if (m_flowControlCommands.count(sName)) {
    mf.GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR, cmStrCat("Built-in flow control command \"", sName, "\" cannot be overridden."),
      command.m_backtrace);
    cmSystemTools::SetFatalErrorOccurred();
    return false;
  }

  // if the command already exists, give a new name to the old command.
  if (m_command oldCmd = GetCommandByExactName(sName)) {
    m_scriptedCommands["_" + sName] = oldCmd;
  }

  m_scriptedCommands[sName] = std::move(command.Value);
  return true;
}

cmState::m_command cmState::GetCommand(std::string const& name) const
{
  return GetCommandByExactName(cmSystemTools::LowerCase(name));
}

cmState::m_command cmState::GetCommandByExactName(std::string const& name) const
{
  auto pos = m_scriptedCommands.find(name);
  if (pos != m_scriptedCommands.end()) {
    return pos->second;
  }
  pos = m_builtinCommands.find(name);
  if (pos != m_builtinCommands.end()) {
    return pos->second;
  }
  return nullptr;
}

std::vector<std::string> cmState::GetCommandNames() const
{
  std::vector<std::string> commandNames;
  commandNames.reserve(m_builtinCommands.size() + m_scriptedCommands.size());
  for (auto const& bc : m_builtinCommands) {
    commandNames.push_back(bc.first);
  }
  for (auto const& sc : m_scriptedCommands) {
    commandNames.push_back(sc.first);
  }
  std::sort(commandNames.begin(), commandNames.end());
  commandNames.erase(std::unique(commandNames.begin(), commandNames.end()), commandNames.end());
  return commandNames;
}

void cmState::RemoveBuiltinCommand(std::string const& name)
{
  assert(name == cmSystemTools::LowerCase(name));
  m_builtinCommands.erase(name);
}

void cmState::RemoveUserDefinedCommands()
{
  m_scriptedCommands.clear();
}

void cmState::SetGlobalProperty(
  std::string const& prop,
  std::string const& value)
{
  m_globalProperties.SetProperty(prop, value);
}
void cmState::SetGlobalProperty(
  std::string const& prop,
  cmValue value)
{
  m_globalProperties.SetProperty(prop, value);
}

void cmState::AppendGlobalProperty(
  std::string const& prop,
  std::string const& value,
  bool asString)
{
  m_globalProperties.AppendProperty(prop, value, asString);
}

cmValue cmState::GetGlobalProperty(std::string const& prop)
{
  if (prop == "CACHE_VARIABLES") {
    std::vector<std::string> cacheKeys = GetCacheEntryKeys();
    SetGlobalProperty("CACHE_VARIABLES", cmList::to_string(cacheKeys));
  } else if (prop == "COMMANDS") {
    std::vector<std::string> commands = GetCommandNames();
    SetGlobalProperty("COMMANDS", cmList::to_string(commands));
  } else if (prop == "IN_TRY_COMPILE") {
    SetGlobalProperty("IN_TRY_COMPILE", m_stateProjectKind == ProjectKind::TryCompile ? "1" : "0");
  } else if (prop == "GENERATOR_IS_MULTI_CONFIG") {
    SetGlobalProperty("GENERATOR_IS_MULTI_CONFIG", m_isGeneratorMultiConfig ? "1" : "0");
  } else if (prop == "ENABLED_LANGUAGES") {
    auto langs = cmList::to_string(m_enabledLanguages);
    SetGlobalProperty("ENABLED_LANGUAGES", langs);
  } else if (prop == "CMAKE_ROLE") {
    std::string mode = GetModeString();
    SetGlobalProperty("CMAKE_ROLE", mode);
  }
#define STRING_LIST_ELEMENT(F) ";" #F
  if (prop == "CMAKE_C_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_C_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_C90_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_C90_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_C99_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_C99_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_C11_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_C11_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_CXX_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_CXX_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_CXX98_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_CXX98_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_CXX11_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_CXX11_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_CXX14_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_CXX14_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_CUDA_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_CUDA_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }
  if (prop == "CMAKE_HIP_KNOWN_FEATURES") {
    static std::string const s_out(&FOR_EACH_HIP_FEATURE(STRING_LIST_ELEMENT)[1]);
    return cmValue(s_out);
  }

#undef STRING_LIST_ELEMENT
  return m_globalProperties.GetPropertyValue(prop);
}

bool cmState::GetGlobalPropertyAsBool(std::string const& prop)
{
  return GetGlobalProperty(prop).IsOn();
}

void cmState::SetSourceDirectory(std::string const& sourceDirectory)
{
  m_sourceDirectory = sourceDirectory;
  cmSystemTools::ConvertToUnixSlashes(m_sourceDirectory);
}

std::string const& cmState::GetSourceDirectory() const
{
  return m_sourceDirectory;
}

void cmState::SetBinaryDirectory(std::string const& binaryDirectory)
{
  m_binaryDirectory = binaryDirectory;
  cmSystemTools::ConvertToUnixSlashes(m_binaryDirectory);
}

void cmState::SetWindowsShell(bool windowsShell)
{
  m_windowsShell = windowsShell;
}

bool cmState::UseWindowsShell() const
{
  return m_windowsShell;
}

void cmState::SetWindowsVSIDE(bool windowsVSIDE)
{
  m_windowsVSIDE = windowsVSIDE;
}

bool cmState::UseWindowsVSIDE() const
{
  return m_windowsVSIDE;
}

void cmState::SetGhsMultiIDE(bool ghsMultiIDE)
{
  m_ghsMultiIDE = ghsMultiIDE;
}

bool cmState::UseGhsMultiIDE() const
{
  return m_ghsMultiIDE;
}

void cmState::SetBorlandMake(bool borlandMake)
{
  m_borlandMake = borlandMake;
}

bool cmState::UseBorlandMake() const
{
  return m_borlandMake;
}

void cmState::SetWatcomWMake(bool watcomWMake)
{
  m_watcomWMake = watcomWMake;
}

bool cmState::UseWatcomWMake() const
{
  return m_watcomWMake;
}

void cmState::SetMinGWMake(bool minGWMake)
{
  m_minGWMake = minGWMake;
}

bool cmState::UseMinGWMake() const
{
  return m_minGWMake;
}

void cmState::SetNMake(bool nMake)
{
  m_nmake = nMake;
}

bool cmState::UseNMake() const
{
  return m_nmake;
}

void cmState::SetMSYSShell(bool mSYSShell)
{
  m_msysShell = mSYSShell;
}

bool cmState::UseMSYSShell() const
{
  return m_msysShell;
}

void cmState::SetNinja(bool ninja)
{
  m_ninja = ninja;
}

bool cmState::UseNinja() const
{
  return m_ninja;
}

void cmState::SetNinjaMulti(bool ninjaMulti)
{
  m_ninjaMulti = ninjaMulti;
}

bool cmState::UseNinjaMulti() const
{
  return m_ninjaMulti;
}

unsigned int cmState::GetCacheMajorVersion() const
{
  return m_cacheManager->GetCacheMajorVersion();
}

unsigned int cmState::GetCacheMinorVersion() const
{
  return m_cacheManager->GetCacheMinorVersion();
}

cmState::Mode cmState::GetMode() const
{
  return m_stateMode;
}

std::string cmState::GetModeString() const
{
  return ModeToString(m_stateMode);
}

std::string cmState::ModeToString(cmState::Mode mode)
{
  switch (mode) {
    case Project:
      return "PROJECT";
    case Script:
      return "SCRIPT";
    case FindPackage:
      return "FIND_PACKAGE";
    case CTest:
      return "CTEST";
    case CPack:
      return "CPACK";
    case Help:
      return "HELP";
    case Unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

cmState::ProjectKind cmState::GetProjectKind() const
{
  return m_stateProjectKind;
}

std::string const& cmState::GetBinaryDirectory() const
{
  return m_binaryDirectory;
}

cmStateSnapshot cmState::CreateBaseSnapshot()
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(m_snapshotData.Root());
  pos->DirectoryParent = m_snapshotData.Root();
  pos->ScopeParent = m_snapshotData.Root();
  pos->SnapshotType = cmStateEnums::BaseType;
  pos->Keep = true;
  pos->BuildSystemDirectory = m_buildsystemDirectory.Push(m_buildsystemDirectory.Root());
  pos->ExecutionListFile = m_executionListFiles.Push(m_executionListFiles.Root());
  pos->IncludeDirectoryPosition = 0;
  pos->CompileDefinitionsPosition = 0;
  pos->CompileOptionsPosition = 0;
  pos->LinkOptionsPosition = 0;
  pos->LinkDirectoriesPosition = 0;
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->Policies = m_policyStack.Root();
  pos->PolicyRoot = m_policyStack.Root();
  pos->PolicyScope = m_policyStack.Root();
  assert(pos->Policies.IsValid());
  assert(pos->PolicyRoot.IsValid());
  pos->Vars = m_varTree.Push(m_varTree.Root());
  assert(pos->Vars.IsValid());
  pos->Parent = m_varTree.Root();
  pos->Root = m_varTree.Root();
  return { this, pos };
}

cmStateSnapshot cmState::CreateBuildsystemDirectorySnapshot(cmStateSnapshot const& originSnapshot)
{
  assert(originSnapshot.IsValid());
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position);
  pos->DirectoryParent = originSnapshot.Position;
  pos->ScopeParent = originSnapshot.Position;
  pos->SnapshotType = cmStateEnums::BuildsystemDirectoryType;
  pos->Keep = true;
  pos->BuildSystemDirectory = m_buildsystemDirectory.Push(originSnapshot.Position->BuildSystemDirectory);
  pos->ExecutionListFile = m_executionListFiles.Push(originSnapshot.Position->ExecutionListFile);
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->Policies = originSnapshot.Position->Policies;
  pos->PolicyRoot = originSnapshot.Position->Policies;
  pos->PolicyScope = originSnapshot.Position->Policies;
  assert(pos->Policies.IsValid());
  assert(pos->PolicyRoot.IsValid());

  cmLinkedTree<cmDefinitions>::iterator origin = originSnapshot.Position->Vars;
  pos->Parent = origin;
  pos->Root = origin;
  pos->Vars = m_varTree.Push(origin);

  cmStateSnapshot snapshot = cmStateSnapshot(this, pos);
  originSnapshot.Position->BuildSystemDirectory->Children.push_back(snapshot);
  snapshot.SetDefaultDefinitions();
  snapshot.InitializeFromParent();
  snapshot.SetDirectoryDefinitions();
  return snapshot;
}

cmStateSnapshot cmState::CreateDeferCallSnapshot(
  cmStateSnapshot const& originSnapshot,
  std::string const& fileName)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->SnapshotType = cmStateEnums::DeferCallType;
  pos->Keep = false;
  pos->ExecutionListFile = m_executionListFiles.Push(originSnapshot.Position->ExecutionListFile, fileName);
  assert(originSnapshot.Position->Vars.IsValid());
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  return { this, pos };
}

cmStateSnapshot cmState::CreateFunctionCallSnapshot(
  cmStateSnapshot const& originSnapshot,
  std::string const& fileName)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->ScopeParent = originSnapshot.Position;
  pos->SnapshotType = cmStateEnums::FunctionCallType;
  pos->Keep = false;
  pos->ExecutionListFile = m_executionListFiles.Push(originSnapshot.Position->ExecutionListFile, fileName);
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  assert(originSnapshot.Position->Vars.IsValid());
  cmLinkedTree<cmDefinitions>::iterator origin = originSnapshot.Position->Vars;
  pos->Parent = origin;
  pos->Vars = m_varTree.Push(origin);
  return { this, pos };
}

cmStateSnapshot cmState::CreateMacroCallSnapshot(
  cmStateSnapshot const& originSnapshot,
  std::string const& fileName)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->SnapshotType = cmStateEnums::MacroCallType;
  pos->Keep = false;
  pos->ExecutionListFile = m_executionListFiles.Push(originSnapshot.Position->ExecutionListFile, fileName);
  assert(originSnapshot.Position->Vars.IsValid());
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  return { this, pos };
}

cmStateSnapshot cmState::CreateIncludeFileSnapshot(
  cmStateSnapshot const& originSnapshot,
  std::string const& fileName)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->SnapshotType = cmStateEnums::IncludeFileType;
  pos->Keep = true;
  pos->ExecutionListFile = m_executionListFiles.Push(originSnapshot.Position->ExecutionListFile, fileName);
  assert(originSnapshot.Position->Vars.IsValid());
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  return { this, pos };
}

cmStateSnapshot cmState::CreateVariableScopeSnapshot(cmStateSnapshot const& originSnapshot)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->ScopeParent = originSnapshot.Position;
  pos->SnapshotType = cmStateEnums::VariableScopeType;
  pos->Keep = false;
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  assert(originSnapshot.Position->Vars.IsValid());

  cmLinkedTree<cmDefinitions>::iterator origin = originSnapshot.Position->Vars;
  pos->Parent = origin;
  pos->Vars = m_varTree.Push(origin);
  assert(pos->Vars.IsValid());
  return { this, pos };
}

cmStateSnapshot cmState::CreateInlineListFileSnapshot(
  cmStateSnapshot const& originSnapshot,
  std::string const& fileName)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->SnapshotType = cmStateEnums::InlineListFileType;
  pos->Keep = true;
  pos->ExecutionListFile = m_executionListFiles.Push(originSnapshot.Position->ExecutionListFile, fileName);
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  return { this, pos };
}

cmStateSnapshot cmState::CreatePolicyScopeSnapshot(cmStateSnapshot const& originSnapshot)
{
  cmStateDetail::PositionType pos = m_snapshotData.Push(originSnapshot.Position, *originSnapshot.Position);
  pos->SnapshotType = cmStateEnums::PolicyScopeType;
  pos->Keep = false;
  pos->BuildSystemDirectory->CurrentScope = pos;
  pos->PolicyScope = originSnapshot.Position->Policies;
  return { this, pos };
}

cmStateSnapshot cmState::Pop(cmStateSnapshot const& originSnapshot)
{
  cmStateDetail::PositionType pos = originSnapshot.Position;
  cmStateDetail::PositionType prevPos = pos;
  ++prevPos;
  prevPos->IncludeDirectoryPosition = prevPos->BuildSystemDirectory->IncludeDirectories.size();
  prevPos->CompileDefinitionsPosition = prevPos->BuildSystemDirectory->CompileDefinitions.size();
  prevPos->CompileOptionsPosition = prevPos->BuildSystemDirectory->CompileOptions.size();
  prevPos->LinkOptionsPosition = prevPos->BuildSystemDirectory->LinkOptions.size();
  prevPos->LinkDirectoriesPosition = prevPos->BuildSystemDirectory->LinkDirectories.size();
  prevPos->BuildSystemDirectory->CurrentScope = prevPos;

  if (!pos->Keep && m_snapshotData.IsLast(pos)) {
    if (pos->Vars != prevPos->Vars) {
      assert(m_varTree.IsLast(pos->Vars));
      m_varTree.Pop(pos->Vars);
    }
    if (pos->ExecutionListFile != prevPos->ExecutionListFile) {
      assert(m_executionListFiles.IsLast(pos->ExecutionListFile));
      m_executionListFiles.Pop(pos->ExecutionListFile);
    }
    m_snapshotData.Pop(pos);
  }

  return { this, prevPos };
}

static bool ParseEntryWithoutType(
  std::string const& entry,
  std::string& var,
  std::string& value)
{
  // input line is:         key=value
  static cmsys::RegularExpression reg("^([^=]*)=(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$");
  // input line is:         "key"=value
  static cmsys::RegularExpression regQuoted("^\"([^\"]*)\"=(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$");
  bool flag = false;
  if (regQuoted.find(entry)) {
    var = regQuoted.match(1);
    value = regQuoted.match(2);
    flag = true;
  } else if (reg.find(entry)) {
    var = reg.match(1);
    value = reg.match(2);
    flag = true;
  }

  // if value is enclosed in single quotes ('foo') then remove them
  // it is used to enclose trailing space or tab
  if (flag && value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    value = value.substr(1, value.size() - 2);
  }

  return flag;
}

bool cmState::ParseCacheEntry(
  std::string const& entry,
  std::string& var,
  std::string& value,
  cmStateEnums::CacheEntryType& type)
{
  // input line is:         key:type=value
  static cmsys::RegularExpression reg("^([^=:]*):([^=]*)=(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$");
  // input line is:         "key":type=value
  static cmsys::RegularExpression regQuoted("^\"([^\"]*)\":([^=]*)=(.*[^\r\t ]|[\r\t ]*)[\r\t ]*$");
  bool flag = false;
  if (regQuoted.find(entry)) {
    var = regQuoted.match(1);
    type = cmState::StringToCacheEntryType(regQuoted.match(2));
    value = regQuoted.match(3);
    flag = true;
  } else if (reg.find(entry)) {
    var = reg.match(1);
    type = cmState::StringToCacheEntryType(reg.match(2));
    value = reg.match(3);
    flag = true;
  }

  // if value is enclosed in single quotes ('foo') then remove them
  // it is used to enclose trailing space or tab
  if (flag && value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    value = value.substr(1, value.size() - 2);
  }

  if (!flag) {
    return ParseEntryWithoutType(entry, var, value);
  }

  return flag;
}

cmState::m_command cmState::GetDependencyProviderCommand(cmDependencyProvider::Method method) const
{
  return (m_dependencyProvider && m_dependencyProvider->SupportsMethod(method))
    ? GetCommand(m_dependencyProvider->GetCommand())
    : m_command{};
}
