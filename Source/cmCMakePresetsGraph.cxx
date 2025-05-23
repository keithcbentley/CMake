/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmCMakePresetsGraph.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <utility>

#include <cm/memory>
#include <cm/string_view>

#include "cmsys/RegularExpression.hxx"

#include "cmCMakePresetsErrors.h"
#include "cmCMakePresetsGraphInternal.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"

#define CHECK_EXPAND(out, field, expanders, version)                          \
  do {                                                                        \
    switch (ExpandMacros(field, expanders, version)) {                        \
      case ExpandMacroResult::Error:                                          \
        return false;                                                         \
      case ExpandMacroResult::Ignore:                                         \
        out.reset();                                                          \
        return true;                                                          \
      case ExpandMacroResult::Ok:                                             \
        break;                                                                \
    }                                                                         \
  } while (false)

namespace {
enum class CycleStatus
{
  Unvisited,
  InProgress,
  Verified,
};

using ConfigurePreset = cmCMakePresetsGraph::ConfigurePreset;
using BuildPreset = cmCMakePresetsGraph::BuildPreset;
using TestPreset = cmCMakePresetsGraph::TestPreset;
using PackagePreset = cmCMakePresetsGraph::PackagePreset;
using WorkflowPreset = cmCMakePresetsGraph::WorkflowPreset;
template <typename T>
using PresetPair = cmCMakePresetsGraph::PresetPair<T>;
using ExpandMacroResult = cmCMakePresetsGraphInternal::ExpandMacroResult;
using MacroExpander = cmCMakePresetsGraphInternal::MacroExpander;
using MacroExpanderVector = cmCMakePresetsGraphInternal::MacroExpanderVector;
using BaseMacroExpander = cmCMakePresetsGraphInternal::BaseMacroExpander;
template <typename T>
using PresetMacroExpander =
  cmCMakePresetsGraphInternal::PresetMacroExpander<T>;
using cmCMakePresetsGraphInternal::ExpandMacros;

void InheritString(std::string& child, std::string const& parent)
{
  if (child.empty()) {
    child = parent;
  }
}

template <typename T>
void InheritOptionalValue(cm::optional<T>& child,
                          cm::optional<T> const& parent)
{
  if (!child) {
    child = parent;
  }
}

template <typename T>
void InheritVector(std::vector<T>& child, std::vector<T> const& parent)
{
  if (child.empty()) {
    child = parent;
  }
}

/**
 * Check preset inheritance for cycles (using a DAG check algorithm) while
 * also bubbling up fields through the inheritance hierarchy, then verify
 * that each preset has the required fields, either directly or through
 * inheritance.
 */
template <class T>
bool VisitPreset(
  T& preset,
  std::map<std::string, cmCMakePresetsGraph::PresetPair<T>>& presets,
  std::map<std::string, CycleStatus> cycleStatus, cmCMakePresetsGraph& graph)
{
  switch (cycleStatus[preset.m_name]) {
    case CycleStatus::InProgress:
      cmCMakePresetsErrors::CYCLIC_PRESET_INHERITANCE(preset.m_name,
                                                      &graph.parseState);
      return false;
    case CycleStatus::Verified:
      return true;
    default:
      break;
  }

  cycleStatus[preset.m_name] = CycleStatus::InProgress;

  if (preset.Environment.count("") != 0) {
    cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name, &graph.parseState);
    return false;
  }

  bool result = preset.VisitPresetBeforeInherit();
  if (!result) {
    cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name, &graph.parseState);
    return false;
  }

  for (auto const& i : preset.Inherits) {
    auto parent = presets.find(i);
    if (parent == presets.end()) {
      cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name,
                                                 &graph.parseState);
      return false;
    }

    auto& parentPreset = parent->second.Unexpanded;
    if (!preset.OriginFile->ReachableFiles.count(parentPreset.OriginFile)) {
      cmCMakePresetsErrors::INHERITED_PRESET_UNREACHABLE_FROM_FILE(
        preset.m_name, &graph.parseState);
      return false;
    }

    if (!VisitPreset(parentPreset, presets, cycleStatus, graph)) {
      return false;
    }

    result = preset.VisitPresetInherit(parentPreset);
    if (!result) {
      cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name,
                                                 &graph.parseState);
      return false;
    }

    for (auto const& v : parentPreset.Environment) {
      preset.Environment.insert(v);
    }

    if (!preset.ConditionEvaluator) {
      preset.ConditionEvaluator = parentPreset.ConditionEvaluator;
    }
  }

  if (preset.ConditionEvaluator && preset.ConditionEvaluator->IsNull()) {
    preset.ConditionEvaluator.reset();
  }

  result = preset.VisitPresetAfterInherit(graph.GetVersion(preset),
                                          &graph.parseState);
  if (!result) {
    cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name, &graph.parseState);
    return false;
  }

  cycleStatus[preset.m_name] = CycleStatus::Verified;
  return true;
}

template <class T>
bool ComputePresetInheritance(
  std::map<std::string, cmCMakePresetsGraph::PresetPair<T>>& presets,
  cmCMakePresetsGraph& graph)
{
  std::map<std::string, CycleStatus> cycleStatus;
  for (auto const& it : presets) {
    cycleStatus[it.first] = CycleStatus::Unvisited;
  }

  for (auto& it : presets) {
    auto& preset = it.second.Unexpanded;
    if (!VisitPreset<T>(preset, presets, cycleStatus, graph)) {
      return false;
    }
  }

  return true;
}

constexpr char const* ValidPrefixes[] = {
  "",
  "env",
  "penv",
  "vendor",
};

bool PrefixesValidMacroNamespace(std::string const& str)
{
  return std::any_of(
    std::begin(ValidPrefixes), std::end(ValidPrefixes),
    [&str](char const* prefix) -> bool { return cmHasPrefix(prefix, str); });
}

bool IsValidMacroNamespace(std::string const& str)
{
  return std::any_of(
    std::begin(ValidPrefixes), std::end(ValidPrefixes),
    [&str](char const* prefix) -> bool { return str == prefix; });
}

ExpandMacroResult VisitEnv(std::string& value, CycleStatus& status,
                           MacroExpanderVector const& macroExpanders,
                           int version);
template <class T>
class EnvironmentMacroExpander : public MacroExpander
{
  std::map<std::string, CycleStatus>& EnvCycles;
  cm::optional<T>& Out;
  MacroExpanderVector& MacroExpanders;

public:
  EnvironmentMacroExpander(MacroExpanderVector& macroExpanders,
                           cm::optional<T>& out,
                           std::map<std::string, CycleStatus>& envCycles)
    : EnvCycles(envCycles)
    , Out(out)
    , MacroExpanders(macroExpanders)
  {
  }
  ExpandMacroResult operator()(std::string const& macroNamespace,
                               std::string const& macroName,
                               std::string& macroOut,
                               int version) const override
  {
    if (macroNamespace == "env" && !macroName.empty() && Out) {
      auto v = Out->Environment.find(macroName);
      if (v != Out->Environment.end() && v->second) {
        auto e =
          VisitEnv(*v->second, EnvCycles[macroName], MacroExpanders, version);
        if (e != ExpandMacroResult::Ok) {
          return e;
        }
        macroOut += *v->second;
        return ExpandMacroResult::Ok;
      }
    }

    if (macroNamespace == "env" || macroNamespace == "penv") {
      if (macroName.empty()) {
        return ExpandMacroResult::Error;
      }
      if (cm::optional<std::string> value =
            cmSystemTools::GetEnvVar(macroName)) {
        macroOut += *value;
      }
      return ExpandMacroResult::Ok;
    }

    return ExpandMacroResult::Ignore;
  }
};

bool ExpandMacros(cmCMakePresetsGraph const& graph,
                  ConfigurePreset const& preset,
                  cm::optional<ConfigurePreset>& out,
                  MacroExpanderVector const& macroExpanders)
{
  std::string binaryDir = preset.BinaryDir;
  CHECK_EXPAND(out, binaryDir, macroExpanders, graph.GetVersion(preset));

  if (!binaryDir.empty()) {
    if (!cmSystemTools::FileIsFullPath(binaryDir)) {
      binaryDir = cmStrCat(graph.SourceDir, '/', binaryDir);
    }
    out->BinaryDir = cmSystemTools::CollapseFullPath(binaryDir);
    cmSystemTools::ConvertToUnixSlashes(out->BinaryDir);
  }

  if (!preset.InstallDir.empty()) {
    std::string installDir = preset.InstallDir;
    CHECK_EXPAND(out, installDir, macroExpanders, graph.GetVersion(preset));

    if (!cmSystemTools::FileIsFullPath(installDir)) {
      installDir = cmStrCat(graph.SourceDir, '/', installDir);
    }
    out->InstallDir = cmSystemTools::CollapseFullPath(installDir);
    cmSystemTools::ConvertToUnixSlashes(out->InstallDir);
  }

  if (!preset.ToolchainFile.empty()) {
    std::string toolchain = preset.ToolchainFile;
    CHECK_EXPAND(out, toolchain, macroExpanders, graph.GetVersion(preset));
    out->ToolchainFile = toolchain;
  }

  if (!preset.m_graphVizFile.empty()) {
    std::string graphVizFile = preset.m_graphVizFile;
    CHECK_EXPAND(out, graphVizFile, macroExpanders, graph.GetVersion(preset));
    out->m_graphVizFile = graphVizFile;
  }

  for (auto& variable : out->CacheVariables) {
    if (variable.second) {
      CHECK_EXPAND(out, variable.second->Value, macroExpanders,
                   graph.GetVersion(preset));
    }
  }

  return true;
}

bool ExpandMacros(cmCMakePresetsGraph const& graph, BuildPreset const& preset,
                  cm::optional<BuildPreset>& out,
                  MacroExpanderVector const& macroExpanders)
{
  for (auto& target : out->m_targets) {
    CHECK_EXPAND(out, target, macroExpanders, graph.GetVersion(preset));
  }

  for (auto& nativeToolOption : out->NativeToolOptions) {
    CHECK_EXPAND(out, nativeToolOption, macroExpanders,
                 graph.GetVersion(preset));
  }

  return true;
}

bool ExpandMacros(cmCMakePresetsGraph const& graph, TestPreset const& preset,
                  cm::optional<TestPreset>& out,
                  MacroExpanderVector const& macroExpanders)
{
  for (auto& overwrite : out->OverwriteConfigurationFile) {
    CHECK_EXPAND(out, overwrite, macroExpanders, graph.GetVersion(preset));
  }

  if (out->Output) {
    CHECK_EXPAND(out, out->Output->OutputLogFile, macroExpanders,
                 graph.GetVersion(preset));
    CHECK_EXPAND(out, out->Output->OutputJUnitFile, macroExpanders,
                 graph.GetVersion(preset));
  }

  if (out->Filter) {
    if (out->Filter->Include) {
      CHECK_EXPAND(out, out->Filter->Include->m_name, macroExpanders,
                   graph.GetVersion(preset));
      CHECK_EXPAND(out, out->Filter->Include->Label, macroExpanders,
                   graph.GetVersion(preset));

      if (out->Filter->Include->Index) {
        CHECK_EXPAND(out, out->Filter->Include->Index->IndexFile,
                     macroExpanders, graph.GetVersion(preset));
      }
    }

    if (out->Filter->Exclude) {
      CHECK_EXPAND(out, out->Filter->Exclude->m_name, macroExpanders,
                   graph.GetVersion(preset));
      CHECK_EXPAND(out, out->Filter->Exclude->Label, macroExpanders,
                   graph.GetVersion(preset));

      if (out->Filter->Exclude->Fixtures) {
        CHECK_EXPAND(out, out->Filter->Exclude->Fixtures->Any, macroExpanders,
                     graph.GetVersion(preset));
        CHECK_EXPAND(out, out->Filter->Exclude->Fixtures->Setup,
                     macroExpanders, graph.GetVersion(preset));
        CHECK_EXPAND(out, out->Filter->Exclude->Fixtures->Cleanup,
                     macroExpanders, graph.GetVersion(preset));
      }
    }
  }

  if (out->Execution) {
    CHECK_EXPAND(out, out->Execution->ResourceSpecFile, macroExpanders,
                 graph.GetVersion(preset));
  }

  return true;
}

bool ExpandMacros(cmCMakePresetsGraph const& graph,
                  PackagePreset const& preset,
                  cm::optional<PackagePreset>& out,
                  MacroExpanderVector const& macroExpanders)
{
  for (auto& variable : out->Variables) {
    CHECK_EXPAND(out, variable.second, macroExpanders,
                 graph.GetVersion(preset));
  }

  CHECK_EXPAND(out, out->ConfigFile, macroExpanders, graph.GetVersion(preset));
  CHECK_EXPAND(out, out->PackageName, macroExpanders,
               graph.GetVersion(preset));
  CHECK_EXPAND(out, out->PackageVersion, macroExpanders,
               graph.GetVersion(preset));
  CHECK_EXPAND(out, out->PackageDirectory, macroExpanders,
               graph.GetVersion(preset));
  CHECK_EXPAND(out, out->VendorName, macroExpanders, graph.GetVersion(preset));

  return true;
}

bool ExpandMacros(cmCMakePresetsGraph const& /*graph*/,
                  WorkflowPreset const& /*preset*/,
                  cm::optional<WorkflowPreset>& /*out*/,
                  MacroExpanderVector const& /*macroExpanders*/)
{
  return true;
}

template <class T>
bool ExpandMacros(cmCMakePresetsGraph& graph, T const& preset,
                  cm::optional<T>& out)
{
  out.emplace(preset);

  std::map<std::string, CycleStatus> envCycles;
  for (auto const& v : out->Environment) {
    envCycles[v.first] = CycleStatus::Unvisited;
  }

  MacroExpanderVector macroExpanders{};

  macroExpanders.push_back(cm::make_unique<BaseMacroExpander>(graph));
  macroExpanders.push_back(
    cm::make_unique<PresetMacroExpander<T>>(graph, preset));
  macroExpanders.push_back(cm::make_unique<EnvironmentMacroExpander<T>>(
    macroExpanders, out, envCycles));

  for (auto& v : out->Environment) {
    if (v.second) {
      switch (VisitEnv(*v.second, envCycles[v.first], macroExpanders,
                       graph.GetVersion(preset))) {
        case ExpandMacroResult::Error:
          cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name,
                                                     &graph.parseState);
          return false;
        case ExpandMacroResult::Ignore:
          out.reset();
          return true;
        case ExpandMacroResult::Ok:
          break;
      }
    }
  }

  if (preset.ConditionEvaluator) {
    cm::optional<bool> result;
    if (!preset.ConditionEvaluator->Evaluate(
          macroExpanders, graph.GetVersion(preset), result)) {
      cmCMakePresetsErrors::INVALID_PRESET_NAMED(preset.m_name,
                                                 &graph.parseState);
      return false;
    }
    if (!result) {
      out.reset();
      return true;
    }
    out->ConditionResult = *result;
  }

  return ExpandMacros(graph, preset, out, macroExpanders);
}

ExpandMacroResult VisitEnv(std::string& value, CycleStatus& status,
                           MacroExpanderVector const& macroExpanders,
                           int version)
{
  if (status == CycleStatus::Verified) {
    return ExpandMacroResult::Ok;
  }
  if (status == CycleStatus::InProgress) {
    return ExpandMacroResult::Error;
  }

  status = CycleStatus::InProgress;
  auto e = ExpandMacros(value, macroExpanders, version);
  if (e != ExpandMacroResult::Ok) {
    return e;
  }
  status = CycleStatus::Verified;
  return ExpandMacroResult::Ok;
}
}

ExpandMacroResult cmCMakePresetsGraphInternal::ExpandMacros(
  std::string& out, MacroExpanderVector const& macroExpanders, int version)
{
  std::string result;
  std::string macroNamespace;
  std::string macroName;

  enum class m_pState
  {
    Default,
    MacroNamespace,
    MacroName,
  } state = m_pState::Default;

  for (auto c : out) {
    switch (state) {
      case m_pState::Default:
        if (c == '$') {
          state = m_pState::MacroNamespace;
        } else {
          result += c;
        }
        break;

      case m_pState::MacroNamespace:
        if (c == '{') {
          if (IsValidMacroNamespace(macroNamespace)) {
            state = m_pState::MacroName;
          } else {
            result += '$';
            result += macroNamespace;
            result += '{';
            macroNamespace.clear();
            state = m_pState::Default;
          }
        } else {
          macroNamespace += c;
          if (!PrefixesValidMacroNamespace(macroNamespace)) {
            result += '$';
            result += macroNamespace;
            macroNamespace.clear();
            state = m_pState::Default;
          }
        }
        break;

      case m_pState::MacroName:
        if (c == '}') {
          auto e = ExpandMacro(result, macroNamespace, macroName,
                               macroExpanders, version);
          if (e != ExpandMacroResult::Ok) {
            return e;
          }
          macroNamespace.clear();
          macroName.clear();
          state = m_pState::Default;
        } else {
          macroName += c;
        }
        break;
    }
  }

  switch (state) {
    case m_pState::Default:
      break;
    case m_pState::MacroNamespace:
      result += '$';
      result += macroNamespace;
      break;
    case m_pState::MacroName:
      return ExpandMacroResult::Error;
  }

  out = std::move(result);
  return ExpandMacroResult::Ok;
}

ExpandMacroResult cmCMakePresetsGraphInternal::ExpandMacro(
  std::string& out, std::string const& macroNamespace,
  std::string const& macroName, MacroExpanderVector const& macroExpanders,
  int version)
{
  for (auto const& macroExpander : macroExpanders) {
    auto result = (*macroExpander)(macroNamespace, macroName, out, version);
    if (result != ExpandMacroResult::Ignore) {
      return result;
    }
  }

  if (macroNamespace == "vendor") {
    return ExpandMacroResult::Ignore;
  }

  return ExpandMacroResult::Error;
}

namespace {
template <typename T>
bool SetupWorkflowConfigurePreset(T const& preset,
                                  ConfigurePreset const*& configurePreset,
                                  cmJSONState* state)
{
  if (preset.ConfigurePreset != configurePreset->m_name) {
    cmCMakePresetsErrors::INVALID_WORKFLOW_STEPS(configurePreset->m_name, state);
    return false;
  }
  return true;
}

template <>
bool SetupWorkflowConfigurePreset<ConfigurePreset>(
  ConfigurePreset const& preset, ConfigurePreset const*& configurePreset,
  cmJSONState*)
{
  configurePreset = &preset;
  return true;
}

template <typename T>
bool TryReachPresetFromWorkflow(
  WorkflowPreset const& origin,
  std::map<std::string, PresetPair<T>> const& presets, std::string const& name,
  ConfigurePreset const*& configurePreset, cmJSONState* state)
{
  auto it = presets.find(name);
  if (it == presets.end()) {
    cmCMakePresetsErrors::INVALID_WORKFLOW_STEPS(name, state);
    return false;
  }
  if (!origin.OriginFile->ReachableFiles.count(
        it->second.Unexpanded.OriginFile)) {
    cmCMakePresetsErrors::WORKFLOW_STEP_UNREACHABLE_FROM_FILE(name, state);
    return false;
  }
  return SetupWorkflowConfigurePreset<T>(it->second.Unexpanded,
                                         configurePreset, state);
}
}

ExpandMacroResult BaseMacroExpander::operator()(
  std::string const& macroNamespace, std::string const& macroName,
  std::string& macroOut, int version) const
{
  if (macroNamespace.empty()) {
    if (macroName == "sourceDir") {
      macroOut += Graph.SourceDir;
      return ExpandMacroResult::Ok;
    }
    if (macroName == "sourceParentDir") {
      macroOut += cmSystemTools::GetParentDirectory(Graph.SourceDir);
      return ExpandMacroResult::Ok;
    }
    if (macroName == "sourceDirName") {
      macroOut += cmSystemTools::GetFilenameName(Graph.SourceDir);
      return ExpandMacroResult::Ok;
    }
    if (macroName == "dollar") {
      macroOut += '$';
      return ExpandMacroResult::Ok;
    }
    if (macroName == "hostSystemName") {
      if (version < 3) {
        return ExpandMacroResult::Error;
      }
      macroOut += cmSystemTools::GetSystemName();
      return ExpandMacroResult::Ok;
    }
    // Enable fileDir macro expansion for non-preset expanders
    if (macroName == "fileDir" && File) {
      if (version < 4) {
        return ExpandMacroResult::Error;
      }
      macroOut += cmSystemTools::GetParentDirectory(File.value());
      return ExpandMacroResult::Ok;
    }
    if (macroName == "pathListSep") {
      if (version < 5) {
        return ExpandMacroResult::Error;
      }
      macroOut += cmSystemTools::GetSystemPathlistSeparator();
      return ExpandMacroResult::Ok;
    }
  }

  return ExpandMacroResult::Ignore;
}

bool cmCMakePresetsGraphInternal::EqualsCondition::Evaluate(
  MacroExpanderVector const& expanders, int version,
  cm::optional<bool>& out) const
{
  std::string lhs = this->Lhs;
  CHECK_EXPAND(out, lhs, expanders, version);

  std::string rhs = this->Rhs;
  CHECK_EXPAND(out, rhs, expanders, version);

  out = (lhs == rhs);
  return true;
}

bool cmCMakePresetsGraphInternal::InListCondition::Evaluate(
  MacroExpanderVector const& expanders, int version,
  cm::optional<bool>& out) const
{
  std::string str = this->String;
  CHECK_EXPAND(out, str, expanders, version);

  for (auto item : this->List) {
    CHECK_EXPAND(out, item, expanders, version);
    if (str == item) {
      out = true;
      return true;
    }
  }

  out = false;
  return true;
}

bool cmCMakePresetsGraphInternal::MatchesCondition::Evaluate(
  MacroExpanderVector const& expanders, int version,
  cm::optional<bool>& out) const
{
  std::string str = this->String;
  CHECK_EXPAND(out, str, expanders, version);
  std::string regexStr = this->Regex;
  CHECK_EXPAND(out, regexStr, expanders, version);

  cmsys::RegularExpression regex;
  if (!regex.compile(regexStr)) {
    return false;
  }

  out = regex.find(str);
  return true;
}

bool cmCMakePresetsGraphInternal::AnyAllOfCondition::Evaluate(
  MacroExpanderVector const& expanders, int version,
  cm::optional<bool>& out) const
{
  for (auto const& condition : this->Conditions) {
    cm::optional<bool> result;
    if (!condition->Evaluate(expanders, version, result)) {
      out.reset();
      return false;
    }

    if (!result) {
      out.reset();
      return true;
    }

    if (result == this->StopValue) {
      out = result;
      return true;
    }
  }

  out = !this->StopValue;
  return true;
}

bool cmCMakePresetsGraphInternal::NotCondition::Evaluate(
  MacroExpanderVector const& expanders, int version,
  cm::optional<bool>& out) const
{
  out.reset();
  if (!this->SubCondition->Evaluate(expanders, version, out)) {
    out.reset();
    return false;
  }
  if (out) {
    *out = !*out;
  }
  return true;
}

bool cmCMakePresetsGraph::ConfigurePreset::VisitPresetInherit(
  cmCMakePresetsGraph::Preset const& parentPreset)
{
  auto& preset = *this;
  ConfigurePreset const& parent =
    static_cast<ConfigurePreset const&>(parentPreset);
  InheritString(preset.Generator, parent.Generator);
  InheritString(preset.Architecture, parent.Architecture);
  InheritString(preset.Toolset, parent.Toolset);
  if (!preset.ArchitectureStrategy) {
    preset.ArchitectureStrategy = parent.ArchitectureStrategy;
  }
  if (!preset.ToolsetStrategy) {
    preset.ToolsetStrategy = parent.ToolsetStrategy;
  }
  InheritString(preset.BinaryDir, parent.BinaryDir);
  InheritString(preset.InstallDir, parent.InstallDir);
  InheritString(preset.ToolchainFile, parent.ToolchainFile);
  InheritString(preset.m_graphVizFile, parent.m_graphVizFile);
  InheritOptionalValue(preset.WarnDev, parent.WarnDev);
  InheritOptionalValue(preset.ErrorDev, parent.ErrorDev);
  InheritOptionalValue(preset.WarnDeprecated, parent.WarnDeprecated);
  InheritOptionalValue(preset.ErrorDeprecated, parent.ErrorDeprecated);
  InheritOptionalValue(preset.m_warnUninitialized, parent.m_warnUninitialized);
  InheritOptionalValue(preset.m_warnUnusedCli, parent.m_warnUnusedCli);
  InheritOptionalValue(preset.WarnSystemVars, parent.WarnSystemVars);

  for (auto const& v : parent.CacheVariables) {
    preset.CacheVariables.insert(v);
  }

  return true;
}

bool cmCMakePresetsGraph::ConfigurePreset::VisitPresetBeforeInherit()
{
  auto& preset = *this;
  if (preset.Environment.count("") != 0) {
    return false;
  }

  return true;
}

bool cmCMakePresetsGraph::ConfigurePreset::VisitPresetAfterInherit(
  int version, cmJSONState* state)
{
  auto& preset = *this;
  if (!preset.Hidden) {
    if (version < 3) {
      if (preset.Generator.empty()) {
        cmCMakePresetsErrors::PRESET_MISSING_FIELD(preset.m_name, "generator",
                                                   state);
        return false;
      }
      if (preset.BinaryDir.empty()) {
        cmCMakePresetsErrors::PRESET_MISSING_FIELD(preset.m_name, "binaryDir",
                                                   state);
        return false;
      }
    }

    if (preset.WarnDev == false && preset.ErrorDev == true) {
      return false;
    }
    if (preset.WarnDeprecated == false && preset.ErrorDeprecated == true) {
      return false;
    }
    if (preset.CacheVariables.count("") != 0) {
      return false;
    }
  }

  return true;
}

bool cmCMakePresetsGraph::BuildPreset::VisitPresetInherit(
  cmCMakePresetsGraph::Preset const& parentPreset)
{
  auto& preset = *this;
  BuildPreset const& parent = static_cast<BuildPreset const&>(parentPreset);

  InheritString(preset.ConfigurePreset, parent.ConfigurePreset);
  InheritOptionalValue(preset.InheritConfigureEnvironment,
                       parent.InheritConfigureEnvironment);
  InheritOptionalValue(preset.Jobs, parent.Jobs);
  InheritVector(preset.m_targets, parent.m_targets);
  InheritString(preset.Configuration, parent.Configuration);
  InheritOptionalValue(preset.CleanFirst, parent.CleanFirst);
  InheritOptionalValue(preset.Verbose, parent.Verbose);
  InheritVector(preset.NativeToolOptions, parent.NativeToolOptions);
  if (!preset.ResolvePackageReferences) {
    preset.ResolvePackageReferences = parent.ResolvePackageReferences;
  }

  return true;
}

bool cmCMakePresetsGraph::BuildPreset::VisitPresetAfterInherit(
  int /* version */, cmJSONState* /*stat*/)
{
  auto& preset = *this;
  if (!preset.Hidden && preset.ConfigurePreset.empty()) {
    return false;
  }
  return true;
}

bool cmCMakePresetsGraph::TestPreset::VisitPresetInherit(
  cmCMakePresetsGraph::Preset const& parentPreset)
{
  auto& preset = *this;
  TestPreset const& parent = static_cast<TestPreset const&>(parentPreset);

  InheritString(preset.ConfigurePreset, parent.ConfigurePreset);
  InheritOptionalValue(preset.InheritConfigureEnvironment,
                       parent.InheritConfigureEnvironment);
  InheritString(preset.Configuration, parent.Configuration);
  InheritVector(preset.OverwriteConfigurationFile,
                parent.OverwriteConfigurationFile);

  if (parent.Output) {
    if (preset.Output) {
      auto& output = preset.Output.value();
      auto const& parentOutput = parent.Output.value();
      InheritOptionalValue(output.ShortProgress, parentOutput.ShortProgress);
      InheritOptionalValue(output.Verbosity, parentOutput.Verbosity);
      InheritOptionalValue(output.Debug, parentOutput.Debug);
      InheritOptionalValue(output.OutputOnFailure,
                           parentOutput.OutputOnFailure);
      InheritOptionalValue(output.Quiet, parentOutput.Quiet);
      InheritString(output.OutputLogFile, parentOutput.OutputLogFile);
      InheritString(output.OutputJUnitFile, parentOutput.OutputJUnitFile);
      InheritOptionalValue(output.LabelSummary, parentOutput.LabelSummary);
      InheritOptionalValue(output.SubprojectSummary,
                           parentOutput.SubprojectSummary);
      InheritOptionalValue(output.MaxPassedTestOutputSize,
                           parentOutput.MaxPassedTestOutputSize);
      InheritOptionalValue(output.MaxFailedTestOutputSize,
                           parentOutput.MaxFailedTestOutputSize);
      InheritOptionalValue(output.TestOutputTruncation,
                           parentOutput.TestOutputTruncation);
      InheritOptionalValue(output.MaxTestNameWidth,
                           parentOutput.MaxTestNameWidth);
    } else {
      preset.Output = parent.Output;
    }
  }

  if (parent.Filter) {
    if (parent.Filter->Include) {
      if (preset.Filter && preset.Filter->Include) {
        auto& include = *preset.Filter->Include;
        auto const& parentInclude = *parent.Filter->Include;
        InheritString(include.m_name, parentInclude.m_name);
        InheritString(include.Label, parentInclude.Label);
        InheritOptionalValue(include.Index, parentInclude.Index);
      } else {
        if (!preset.Filter) {
          preset.Filter.emplace();
        }
        preset.Filter->Include = parent.Filter->Include;
      }
    }

    if (parent.Filter->Exclude) {
      if (preset.Filter && preset.Filter->Exclude) {
        auto& exclude = *preset.Filter->Exclude;
        auto const& parentExclude = *parent.Filter->Exclude;
        InheritString(exclude.m_name, parentExclude.m_name);
        InheritString(exclude.Label, parentExclude.Label);
        InheritOptionalValue(exclude.Fixtures, parentExclude.Fixtures);
      } else {
        if (!preset.Filter) {
          preset.Filter.emplace();
        }
        preset.Filter->Exclude = parent.Filter->Exclude;
      }
    }
  }

  if (parent.Execution) {
    if (preset.Execution) {
      auto& execution = *preset.Execution;
      auto const& parentExecution = *parent.Execution;
      InheritOptionalValue(execution.StopOnFailure,
                           parentExecution.StopOnFailure);
      InheritOptionalValue(execution.EnableFailover,
                           parentExecution.EnableFailover);
      InheritOptionalValue(execution.Jobs, parentExecution.Jobs);
      InheritString(execution.ResourceSpecFile,
                    parentExecution.ResourceSpecFile);
      InheritOptionalValue(execution.TestLoad, parentExecution.TestLoad);
      InheritOptionalValue(execution.ShowOnly, parentExecution.ShowOnly);
      InheritOptionalValue(execution.Repeat, parentExecution.Repeat);
      InheritOptionalValue(execution.InteractiveDebugging,
                           parentExecution.InteractiveDebugging);
      InheritOptionalValue(execution.ScheduleRandom,
                           parentExecution.ScheduleRandom);
      InheritOptionalValue(execution.Timeout, parentExecution.Timeout);
      InheritOptionalValue(execution.NoTestsAction,
                           parentExecution.NoTestsAction);
    } else {
      preset.Execution = parent.Execution;
    }
  }

  return true;
}

bool cmCMakePresetsGraph::TestPreset::VisitPresetAfterInherit(
  int /* version */, cmJSONState* /*state*/)
{
  auto& preset = *this;
  if (!preset.Hidden && preset.ConfigurePreset.empty()) {
    return false;
  }
  return true;
}

bool cmCMakePresetsGraph::PackagePreset::VisitPresetInherit(
  cmCMakePresetsGraph::Preset const& parentPreset)
{
  auto& preset = *this;
  PackagePreset const& parent =
    static_cast<PackagePreset const&>(parentPreset);

  InheritString(preset.ConfigurePreset, parent.ConfigurePreset);
  InheritOptionalValue(preset.InheritConfigureEnvironment,
                       parent.InheritConfigureEnvironment);
  InheritVector(preset.Generators, parent.Generators);
  InheritVector(preset.Configurations, parent.Configurations);

  for (auto const& v : parent.Variables) {
    preset.Variables.insert(v);
  }

  InheritOptionalValue(preset.m_debugOutput, parent.m_debugOutput);
  InheritOptionalValue(preset.VerboseOutput, parent.VerboseOutput);
  InheritString(preset.PackageName, parent.PackageName);
  InheritString(preset.PackageVersion, parent.PackageVersion);
  InheritString(preset.PackageDirectory, parent.PackageDirectory);
  InheritString(preset.VendorName, parent.VendorName);

  return true;
}

bool cmCMakePresetsGraph::PackagePreset::VisitPresetAfterInherit(
  int /* version */, cmJSONState* /*state*/)
{
  auto& preset = *this;
  if (!preset.Hidden && preset.ConfigurePreset.empty()) {
    return false;
  }
  return true;
}

bool cmCMakePresetsGraph::WorkflowPreset::VisitPresetInherit(
  cmCMakePresetsGraph::Preset const& /*parentPreset*/)
{
  return true;
}

bool cmCMakePresetsGraph::WorkflowPreset::VisitPresetAfterInherit(
  int /* version */, cmJSONState* /*state*/)
{
  return true;
}

std::string cmCMakePresetsGraph::GetFilename(std::string const& sourceDir)
{
  return cmStrCat(sourceDir, "/CMakePresets.json");
}

std::string cmCMakePresetsGraph::GetUserFilename(std::string const& sourceDir)
{
  return cmStrCat(sourceDir, "/CMakeUserPresets.json");
}

bool cmCMakePresetsGraph::ReadProjectPresets(std::string const& sourceDir,
                                             bool allowNoFiles)
{
  this->SourceDir = sourceDir;
  this->ClearPresets();

  if (!this->ReadProjectPresetsInternal(allowNoFiles)) {
    this->ClearPresets();
    return false;
  }

  return true;
}

bool cmCMakePresetsGraph::ReadProjectPresetsInternal(bool allowNoFiles)
{
  bool haveOneFile = false;

  File* file;
  std::string filename = GetUserFilename(this->SourceDir);
  std::vector<File*> inProgressFiles;
  if (cmSystemTools::FileExists(filename)) {
    if (!this->ReadJSONFile(filename, RootType::User, ReadReason::Root,
                            inProgressFiles, file, this->errors)) {
      return false;
    }
    haveOneFile = true;
  } else {
    filename = GetFilename(this->SourceDir);
    if (cmSystemTools::FileExists(filename)) {
      if (!this->ReadJSONFile(filename, RootType::Project, ReadReason::Root,
                              inProgressFiles, file, this->errors)) {
        return false;
      }
      haveOneFile = true;
    }
  }
  assert(inProgressFiles.empty());

  if (!haveOneFile) {
    if (allowNoFiles) {
      return true;
    }
    cmCMakePresetsErrors::FILE_NOT_FOUND(filename, &this->parseState);
    return false;
  }

  bool result = ComputePresetInheritance(this->ConfigurePresets, *this) &&
    ComputePresetInheritance(this->BuildPresets, *this) &&
    ComputePresetInheritance(this->TestPresets, *this) &&
    ComputePresetInheritance(this->PackagePresets, *this) &&
    ComputePresetInheritance(this->WorkflowPresets, *this);
  if (!result) {
    return false;
  }

  for (auto& it : this->ConfigurePresets) {
    if (!ExpandMacros(*this, it.second.Unexpanded, it.second.Expanded)) {
      cmCMakePresetsErrors::INVALID_MACRO_EXPANSION(it.first,
                                                    &this->parseState);
      return false;
    }
  }

  for (auto& it : this->BuildPresets) {
    if (!it.second.Unexpanded.Hidden) {
      auto const configurePreset =
        this->ConfigurePresets.find(it.second.Unexpanded.ConfigurePreset);
      if (configurePreset == this->ConfigurePresets.end()) {
        cmCMakePresetsErrors::INVALID_CONFIGURE_PRESET(it.first,
                                                       &this->parseState);
        return false;
      }
      if (!it.second.Unexpanded.OriginFile->ReachableFiles.count(
            configurePreset->second.Unexpanded.OriginFile)) {
        cmCMakePresetsErrors::CONFIGURE_PRESET_UNREACHABLE_FROM_FILE(
          it.first, &this->parseState);
        return false;
      }

      if (it.second.Unexpanded.InheritConfigureEnvironment.value_or(true)) {
        it.second.Unexpanded.Environment.insert(
          configurePreset->second.Unexpanded.Environment.begin(),
          configurePreset->second.Unexpanded.Environment.end());
      }
    }

    if (!ExpandMacros(*this, it.second.Unexpanded, it.second.Expanded)) {
      cmCMakePresetsErrors::INVALID_MACRO_EXPANSION(it.first,
                                                    &this->parseState);
      return false;
    }
  }

  for (auto& it : this->TestPresets) {
    if (!it.second.Unexpanded.Hidden) {
      auto const configurePreset =
        this->ConfigurePresets.find(it.second.Unexpanded.ConfigurePreset);
      if (configurePreset == this->ConfigurePresets.end()) {
        cmCMakePresetsErrors::INVALID_CONFIGURE_PRESET(it.first,
                                                       &this->parseState);
        return false;
      }
      if (!it.second.Unexpanded.OriginFile->ReachableFiles.count(
            configurePreset->second.Unexpanded.OriginFile)) {
        cmCMakePresetsErrors::CONFIGURE_PRESET_UNREACHABLE_FROM_FILE(
          it.first, &this->parseState);
        return false;
      }

      if (it.second.Unexpanded.InheritConfigureEnvironment.value_or(true)) {
        it.second.Unexpanded.Environment.insert(
          configurePreset->second.Unexpanded.Environment.begin(),
          configurePreset->second.Unexpanded.Environment.end());
      }
    }

    if (!ExpandMacros(*this, it.second.Unexpanded, it.second.Expanded)) {
      cmCMakePresetsErrors::INVALID_MACRO_EXPANSION(it.first,
                                                    &this->parseState);
      return false;
    }
  }

  for (auto& it : this->PackagePresets) {
    if (!it.second.Unexpanded.Hidden) {
      auto const configurePreset =
        this->ConfigurePresets.find(it.second.Unexpanded.ConfigurePreset);
      if (configurePreset == this->ConfigurePresets.end()) {
        cmCMakePresetsErrors::INVALID_CONFIGURE_PRESET(it.first,
                                                       &this->parseState);
        return false;
      }
      if (!it.second.Unexpanded.OriginFile->ReachableFiles.count(
            configurePreset->second.Unexpanded.OriginFile)) {
        cmCMakePresetsErrors::CONFIGURE_PRESET_UNREACHABLE_FROM_FILE(
          it.first, &this->parseState);
        return false;
      }

      if (it.second.Unexpanded.InheritConfigureEnvironment.value_or(true)) {
        it.second.Unexpanded.Environment.insert(
          configurePreset->second.Unexpanded.Environment.begin(),
          configurePreset->second.Unexpanded.Environment.end());
      }
    }

    if (!ExpandMacros(*this, it.second.Unexpanded, it.second.Expanded)) {
      cmCMakePresetsErrors::INVALID_MACRO_EXPANSION(it.first,
                                                    &this->parseState);
      return false;
    }
  }

  for (auto& it : this->WorkflowPresets) {
    using Type = WorkflowPreset::WorkflowStep::Type;

    ConfigurePreset const* configurePreset = nullptr;
    for (auto const& step : it.second.Unexpanded.Steps) {
      if (!configurePreset && step.PresetType != Type::Configure) {
        cmCMakePresetsErrors::FIRST_WORKFLOW_STEP_NOT_CONFIGURE(
          step.PresetName, &this->parseState);
        return false;
      }
      if (configurePreset && step.PresetType == Type::Configure) {
        cmCMakePresetsErrors::CONFIGURE_WORKFLOW_STEP_NOT_FIRST(
          step.PresetName, &this->parseState);
        return false;
      }

      switch (step.PresetType) {
        case Type::Configure:
          result = TryReachPresetFromWorkflow(
            it.second.Unexpanded, this->ConfigurePresets, step.PresetName,
            configurePreset, &this->parseState);
          break;
        case Type::Build:
          result = TryReachPresetFromWorkflow(
            it.second.Unexpanded, this->BuildPresets, step.PresetName,
            configurePreset, &this->parseState);
          break;
        case Type::Test:
          result = TryReachPresetFromWorkflow(
            it.second.Unexpanded, this->TestPresets, step.PresetName,
            configurePreset, &this->parseState);
          break;
        case Type::Package:
          result = TryReachPresetFromWorkflow(
            it.second.Unexpanded, this->PackagePresets, step.PresetName,
            configurePreset, &this->parseState);
          break;
      }
      if (!result) {
        return false;
      }
    }

    if (!configurePreset) {
      cmCMakePresetsErrors::NO_WORKFLOW_STEPS(it.first, &this->parseState);
      return false;
    }

    if (!ExpandMacros(*this, it.second.Unexpanded, it.second.Expanded)) {
      cmCMakePresetsErrors::INVALID_MACRO_EXPANSION(it.first,
                                                    &this->parseState);
      return false;
    }
  }

  return true;
}

void cmCMakePresetsGraph::ClearPresets()
{
  this->ConfigurePresets.clear();
  this->BuildPresets.clear();
  this->TestPresets.clear();
  this->PackagePresets.clear();
  this->WorkflowPresets.clear();

  this->ConfigurePresetOrder.clear();
  this->BuildPresetOrder.clear();
  this->TestPresetOrder.clear();
  this->PackagePresetOrder.clear();
  this->WorkflowPresetOrder.clear();

  this->Files.clear();
}

void cmCMakePresetsGraph::printPrecedingNewline(PrintPrecedingNewline* newline)
{
  if (newline) {
    if (*newline == PrintPrecedingNewline::True) {
      std::cout << std::endl;
    }
    *newline = PrintPrecedingNewline::True;
  }
}

void cmCMakePresetsGraph::PrintPresets(
  std::vector<cmCMakePresetsGraph::Preset const*> const& presets)
{
  if (presets.empty()) {
    return;
  }

  auto longestPresetName =
    std::max_element(presets.begin(), presets.end(),
                     [](cmCMakePresetsGraph::Preset const* a,
                        cmCMakePresetsGraph::Preset const* b) {
                       return a->m_name.length() < b->m_name.length();
                     });
  auto longestLength = (*longestPresetName)->m_name.length();

  for (auto const* preset : presets) {
    std::cout << "  \"" << preset->m_name << '"';
    auto const& description = preset->DisplayName;
    if (!description.empty()) {
      for (std::size_t i = 0; i < longestLength - preset->m_name.length(); ++i) {
        std::cout << ' ';
      }
      std::cout << " - " << description;
    }
    std::cout << '\n';
  }
}

void cmCMakePresetsGraph::PrintConfigurePresetList(
  PrintPrecedingNewline* newline) const
{
  PrintConfigurePresetList([](ConfigurePreset const&) { return true; },
                           newline);
}

void cmCMakePresetsGraph::PrintConfigurePresetList(
  std::function<bool(ConfigurePreset const&)> const& filter,
  PrintPrecedingNewline* newline) const
{
  std::vector<cmCMakePresetsGraph::Preset const*> presets;
  for (auto const& p : this->ConfigurePresetOrder) {
    auto const& preset = this->ConfigurePresets.at(p);
    if (!preset.Unexpanded.Hidden && preset.Expanded &&
        preset.Expanded->ConditionResult && filter(preset.Unexpanded)) {
      presets.push_back(
        static_cast<cmCMakePresetsGraph::Preset const*>(&preset.Unexpanded));
    }
  }

  if (!presets.empty()) {
    printPrecedingNewline(newline);
    std::cout << "Available configure presets:\n\n";
    cmCMakePresetsGraph::PrintPresets(presets);
  }
}

void cmCMakePresetsGraph::PrintBuildPresetList(
  PrintPrecedingNewline* newline) const
{
  std::vector<cmCMakePresetsGraph::Preset const*> presets;
  for (auto const& p : this->BuildPresetOrder) {
    auto const& preset = this->BuildPresets.at(p);
    if (!preset.Unexpanded.Hidden && preset.Expanded &&
        preset.Expanded->ConditionResult) {
      presets.push_back(
        static_cast<cmCMakePresetsGraph::Preset const*>(&preset.Unexpanded));
    }
  }

  if (!presets.empty()) {
    printPrecedingNewline(newline);
    std::cout << "Available build presets:\n\n";
    cmCMakePresetsGraph::PrintPresets(presets);
  }
}

void cmCMakePresetsGraph::PrintTestPresetList(
  PrintPrecedingNewline* newline) const
{
  std::vector<cmCMakePresetsGraph::Preset const*> presets;
  for (auto const& p : this->TestPresetOrder) {
    auto const& preset = this->TestPresets.at(p);
    if (!preset.Unexpanded.Hidden && preset.Expanded &&
        preset.Expanded->ConditionResult) {
      presets.push_back(
        static_cast<cmCMakePresetsGraph::Preset const*>(&preset.Unexpanded));
    }
  }

  if (!presets.empty()) {
    printPrecedingNewline(newline);
    std::cout << "Available test presets:\n\n";
    cmCMakePresetsGraph::PrintPresets(presets);
  }
}

void cmCMakePresetsGraph::PrintPackagePresetList(
  PrintPrecedingNewline* newline) const
{
  this->PrintPackagePresetList([](PackagePreset const&) { return true; },
                               newline);
}

void cmCMakePresetsGraph::PrintPackagePresetList(
  std::function<bool(PackagePreset const&)> const& filter,
  PrintPrecedingNewline* newline) const
{
  std::vector<cmCMakePresetsGraph::Preset const*> presets;
  for (auto const& p : this->PackagePresetOrder) {
    auto const& preset = this->PackagePresets.at(p);
    if (!preset.Unexpanded.Hidden && preset.Expanded &&
        preset.Expanded->ConditionResult && filter(preset.Unexpanded)) {
      presets.push_back(
        static_cast<cmCMakePresetsGraph::Preset const*>(&preset.Unexpanded));
    }
  }

  if (!presets.empty()) {
    printPrecedingNewline(newline);
    std::cout << "Available package presets:\n\n";
    cmCMakePresetsGraph::PrintPresets(presets);
  }
}

void cmCMakePresetsGraph::PrintWorkflowPresetList(
  PrintPrecedingNewline* newline) const
{
  std::vector<cmCMakePresetsGraph::Preset const*> presets;
  for (auto const& p : this->WorkflowPresetOrder) {
    auto const& preset = this->WorkflowPresets.at(p);
    if (!preset.Unexpanded.Hidden && preset.Expanded &&
        preset.Expanded->ConditionResult) {
      presets.push_back(
        static_cast<cmCMakePresetsGraph::Preset const*>(&preset.Unexpanded));
    }
  }

  if (!presets.empty()) {
    printPrecedingNewline(newline);
    std::cout << "Available workflow presets:\n\n";
    cmCMakePresetsGraph::PrintPresets(presets);
  }
}

void cmCMakePresetsGraph::PrintAllPresets() const
{
  PrintPrecedingNewline newline = PrintPrecedingNewline::False;
  this->PrintConfigurePresetList(&newline);
  this->PrintBuildPresetList(&newline);
  this->PrintTestPresetList(&newline);
  this->PrintPackagePresetList(&newline);
  this->PrintWorkflowPresetList(&newline);
}
