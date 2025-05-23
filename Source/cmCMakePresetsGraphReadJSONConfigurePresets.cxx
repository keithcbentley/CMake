/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cm/optional>
#include <cmext/string_view>

#include <cm3p/json/value.h>

#include "cmCMakePresetsErrors.h"
#include "cmCMakePresetsGraph.h"
#include "cmCMakePresetsGraphInternal.h"
#include "cmJSONHelpers.h"
#include "cmStateTypes.h"

class cmJSONState;

namespace {
using CacheVariable = cmCMakePresetsGraph::CacheVariable;
using ConfigurePreset = cmCMakePresetsGraph::ConfigurePreset;
using ArchToolsetStrategy = cmCMakePresetsGraph::ArchToolsetStrategy;
using JSONHelperBuilder = cmJSONHelperBuilder;
using TraceEnableMode = cmCMakePresetsGraph::TraceEnableMode;
using TraceOutputFormat = cmTraceEnums::TraceOutputFormat;

bool ArchToolsetStrategyHelper(cm::optional<ArchToolsetStrategy>& out,
                               Json::Value const* value, cmJSONState* state)
{
  if (!value) {
    out = cm::nullopt;
    return true;
  }

  if (!value->isString()) {
    cmCMakePresetsErrors::INVALID_PRESET(value, state);
    return false;
  }

  if (value->asString() == "set") {
    out = ArchToolsetStrategy::m_set;
    return true;
  }

  if (value->asString() == "external") {
    out = ArchToolsetStrategy::External;
    return true;
  }

  cmCMakePresetsErrors::INVALID_PRESET(value, state);
  return false;
}

std::function<bool(ConfigurePreset&, Json::Value const*, cmJSONState*)>
ArchToolsetHelper(
  std::string ConfigurePreset::*valueField,
  cm::optional<ArchToolsetStrategy> ConfigurePreset::*strategyField)
{
  auto const objectHelper =
    JSONHelperBuilder::Object<ConfigurePreset>(JsonErrors::INVALID_OBJECT,
                                               false)
      .Bind("value", valueField,
            cmCMakePresetsGraphInternal::PresetStringHelper, false)
      .Bind("strategy", strategyField, ArchToolsetStrategyHelper, false);
  return [valueField, strategyField,
          objectHelper](ConfigurePreset& out, Json::Value const* value,
                        cmJSONState* state) -> bool {
    if (!value) {
      (out.*valueField).clear();
      out.*strategyField = cm::nullopt;
      return true;
    }

    if (value->isString()) {
      out.*valueField = value->asString();
      out.*strategyField = cm::nullopt;
      return true;
    }

    if (value->isObject()) {
      return objectHelper(out, value, state);
    }

    cmCMakePresetsErrors::INVALID_PRESET(value, state);
    return false;
  };
}

auto const ArchitectureHelper = ArchToolsetHelper(
  &ConfigurePreset::Architecture, &ConfigurePreset::ArchitectureStrategy);
auto const ToolsetHelper = ArchToolsetHelper(
  &ConfigurePreset::Toolset, &ConfigurePreset::ToolsetStrategy);

bool TraceEnableModeHelper(cm::optional<TraceEnableMode>& out,
                           Json::Value const* value, cmJSONState* state)
{
  if (!value) {
    out = cm::nullopt;
    return true;
  }

  if (!value->isString()) {
    cmCMakePresetsErrors::INVALID_PRESET(value, state);
    return false;
  }

  if (value->asString() == "on") {
    out = TraceEnableMode::Default;
  } else if (value->asString() == "off") {
    out = TraceEnableMode::Disable;
  } else if (value->asString() == "expand") {
    out = TraceEnableMode::Expand;
  } else {
    cmCMakePresetsErrors::INVALID_PRESET(value, state);
    return false;
  }

  return true;
}

bool TraceOutputFormatHelper(cm::optional<TraceOutputFormat>& out,
                             Json::Value const* value, cmJSONState* state)
{
  if (!value) {
    out = cm::nullopt;
    return true;
  }

  if (!value->isString()) {
    cmCMakePresetsErrors::INVALID_PRESET(value, state);
    return false;
  }

  if (value->asString() == "human") {
    out = TraceOutputFormat::Human;
  } else if (value->asString() == "json-v1") {
    out = TraceOutputFormat::JSONv1;
  } else {
    cmCMakePresetsErrors::INVALID_PRESET(value, state);
    return false;
  }

  return true;
}

auto const VariableStringHelper = JSONHelperBuilder::String();

bool VariableValueHelper(std::string& out, Json::Value const* value,
                         cmJSONState* state)
{
  if (!value) {
    out.clear();
    return true;
  }

  if (value->isBool()) {
    out = value->asBool() ? "TRUE" : "FALSE";
    return true;
  }

  return VariableStringHelper(out, value, state);
}

auto const VariableObjectHelper =
  JSONHelperBuilder::Object<CacheVariable>(
    cmCMakePresetsErrors::INVALID_VARIABLE_OBJECT, false)
    .Bind("type"_s, &CacheVariable::Type, VariableStringHelper, false)
    .Bind("value"_s, &CacheVariable::Value, VariableValueHelper);

bool VariableHelper(cm::optional<CacheVariable>& out, Json::Value const* value,
                    cmJSONState* state)
{
  if (value->isBool()) {
    out = CacheVariable{
      /*Type=*/"BOOL",
      /*Value=*/value->asBool() ? "TRUE" : "FALSE",
    };
    return true;
  }
  if (value->isString()) {
    out = CacheVariable{
      /*Type=*/"",
      /*Value=*/value->asString(),
    };
    return true;
  }
  if (value->isObject()) {
    out.emplace();
    return VariableObjectHelper(*out, value, state);
  }
  if (value->isNull()) {
    out = cm::nullopt;
    return true;
  }
  cmCMakePresetsErrors::INVALID_VARIABLE(value, state);
  return false;
}

auto const VariablesHelper =
  JSONHelperBuilder::Map<cm::optional<CacheVariable>>(
    cmCMakePresetsErrors::INVALID_PRESET, VariableHelper);

auto const PresetWarningsHelper =
  JSONHelperBuilder::Object<ConfigurePreset>(
    JsonErrors::INVALID_NAMED_OBJECT_KEY, false)
    .Bind("dev"_s, &ConfigurePreset::WarnDev,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("deprecated"_s, &ConfigurePreset::WarnDeprecated,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("uninitialized"_s, &ConfigurePreset::m_warnUninitialized,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("unusedCli"_s, &ConfigurePreset::m_warnUnusedCli,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("systemVars"_s, &ConfigurePreset::WarnSystemVars,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false);

auto const PresetErrorsHelper =
  JSONHelperBuilder::Object<ConfigurePreset>(
    JsonErrors::INVALID_NAMED_OBJECT_KEY, false)
    .Bind("dev"_s, &ConfigurePreset::ErrorDev,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("deprecated"_s, &ConfigurePreset::ErrorDeprecated,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false);

auto const PresetDebugHelper =
  JSONHelperBuilder::Object<ConfigurePreset>(
    JsonErrors::INVALID_NAMED_OBJECT_KEY, false)
    .Bind("output"_s, &ConfigurePreset::m_debugOutput,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("tryCompile"_s, &ConfigurePreset::m_debugTryCompile,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false)
    .Bind("find"_s, &ConfigurePreset::DebugFind,
          cmCMakePresetsGraphInternal::PresetOptionalBoolHelper, false);

auto const PresetTraceHelper =
  JSONHelperBuilder::Object<ConfigurePreset>(
    cmCMakePresetsErrors::INVALID_PRESET_OBJECT, false)
    .Bind("mode"_s, &ConfigurePreset::TraceMode, TraceEnableModeHelper, false)
    .Bind("format"_s, &ConfigurePreset::TraceFormat, TraceOutputFormatHelper,
          false)
    .Bind("source"_s, &ConfigurePreset::TraceSource,
          cmCMakePresetsGraphInternal::PresetVectorOneOrMoreStringHelper,
          false)
    .Bind("redirect"_s, &ConfigurePreset::m_traceRedirect,
          cmCMakePresetsGraphInternal::PresetStringHelper, false);

auto const ConfigurePresetHelper =
  JSONHelperBuilder::Object<ConfigurePreset>(
    cmCMakePresetsErrors::INVALID_PRESET_OBJECT, false)
    .Bind("name"_s, &ConfigurePreset::m_name,
          cmCMakePresetsGraphInternal::PresetNameHelper)
    .Bind("inherits"_s, &ConfigurePreset::Inherits,
          cmCMakePresetsGraphInternal::PresetVectorOneOrMoreStringHelper,
          false)
    .Bind("hidden"_s, &ConfigurePreset::Hidden,
          cmCMakePresetsGraphInternal::PresetBoolHelper, false)
    .Bind<std::nullptr_t>("vendor"_s, nullptr,
                          cmCMakePresetsGraphInternal::VendorHelper(
                            cmCMakePresetsErrors::INVALID_PRESET),
                          false)
    .Bind("displayName"_s, &ConfigurePreset::DisplayName,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("description"_s, &ConfigurePreset::Description,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("generator"_s, &ConfigurePreset::Generator,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("architecture"_s, ArchitectureHelper, false)
    .Bind("toolset"_s, ToolsetHelper, false)
    .Bind("toolchainFile"_s, &ConfigurePreset::ToolchainFile,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("graphviz"_s, &ConfigurePreset::m_graphVizFile,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("binaryDir"_s, &ConfigurePreset::BinaryDir,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("installDir"_s, &ConfigurePreset::InstallDir,
          cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind<std::string>("cmakeExecutable"_s, nullptr,
                       cmCMakePresetsGraphInternal::PresetStringHelper, false)
    .Bind("cacheVariables"_s, &ConfigurePreset::CacheVariables,
          VariablesHelper, false)
    .Bind("environment"_s, &ConfigurePreset::Environment,
          cmCMakePresetsGraphInternal::EnvironmentMapHelper, false)
    .Bind("warnings"_s, PresetWarningsHelper, false)
    .Bind("errors"_s, PresetErrorsHelper, false)
    .Bind("debug"_s, PresetDebugHelper, false)
    .Bind("trace"_s, PresetTraceHelper, false)
    .Bind("condition"_s, &ConfigurePreset::ConditionEvaluator,
          cmCMakePresetsGraphInternal::PresetConditionHelper, false);
}

namespace cmCMakePresetsGraphInternal {
bool ConfigurePresetsHelper(std::vector<ConfigurePreset>& out,
                            Json::Value const* value, cmJSONState* state)
{
  static auto const helper = JSONHelperBuilder::Vector<ConfigurePreset>(
    cmCMakePresetsErrors::INVALID_PRESETS, ConfigurePresetHelper);

  return helper(out, value, state);
}
}
