/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
/* clang-format off */
#include "cmGeneratorTarget.h"
/* clang-format on */

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cm/string_view>
#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmEvaluatedTargetProperty.h"
#include "cmGeneratorExpressionDAGChecker.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmRange.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"
#include "cmake.h"

enum class OptionsParse
{
  None,
  Shell
};

namespace {
auto const DL_BEGIN = "<DEVICE_LINK>"_s;
auto const DL_END = "</DEVICE_LINK>"_s;

void processOptions(cmGeneratorTarget const* tgt,
                    EvaluatedTargetPropertyEntries const& entries,
                    std::vector<BT<std::string>>& options,
                    std::unordered_set<std::string>& uniqueOptions,
                    bool debugOptions, char const* logName, OptionsParse parse,
                    bool processDeviceOptions = false)
{
  bool splitOption = !processDeviceOptions;
  for (EvaluatedTargetPropertyEntry const& entry : entries.Entries) {
    std::string usedOptions;
    for (std::string const& opt : entry.Values) {
      if (processDeviceOptions && (opt == DL_BEGIN || opt == DL_END)) {
        options.emplace_back(opt, entry.m_backtrace);
        splitOption = opt == DL_BEGIN;
        continue;
      }

      if (uniqueOptions.insert(opt).second) {
        if (parse == OptionsParse::Shell &&
            cmHasLiteralPrefix(opt, "SHELL:")) {
          if (splitOption) {
            std::vector<std::string> tmp;
            cmSystemTools::ParseUnixCommandLine(opt.c_str() + 6, tmp);
            for (std::string& o : tmp) {
              options.emplace_back(std::move(o), entry.m_backtrace);
            }
          } else {
            options.emplace_back(std::string(opt.c_str() + 6),
                                 entry.m_backtrace);
          }
        } else {
          options.emplace_back(opt, entry.m_backtrace);
        }
        if (debugOptions) {
          usedOptions += " * " + opt + "\n";
        }
      }
    }
    if (!usedOptions.empty()) {
      tgt->GetLocalGenerator()->GetCMakeInstance()->IssueMessage(
        MessageType::LOG,
        std::string("Used ") + logName + std::string(" for target ") +
          tgt->GetName() + ":\n" + usedOptions,
        entry.m_backtrace);
    }
  }
}

enum class NestedLinkerFlags
{
  PreserveAsSpelled,
  Normalize
};

std::vector<BT<std::string>> wrapOptions(
  std::vector<std::string>& options, cmListFileBacktrace const& bt,
  std::vector<std::string> const& wrapperFlag, std::string const& wrapperSep,
  bool concatFlagAndArgs, NestedLinkerFlags nestedLinkerFlags)
{
  std::vector<BT<std::string>> result;

  if (options.empty()) {
    return result;
  }

  if (wrapperFlag.empty()) {
    // nothing specified, insert elements as is
    result.reserve(options.size());
    for (std::string& o : options) {
      result.emplace_back(std::move(o), bt);
    }
    return result;
  }

  auto insertWrapped = [&](std::vector<std::string>& opts) {
    if (!wrapperSep.empty()) {
      if (concatFlagAndArgs) {
        // insert flag elements except last one
        for (auto i = wrapperFlag.begin(); i != wrapperFlag.end() - 1; ++i) {
          result.emplace_back(*i, bt);
        }
        // concatenate last flag element and all list values
        // in one option
        result.emplace_back(wrapperFlag.back() + cmJoin(opts, wrapperSep), bt);
      } else {
        for (std::string const& i : wrapperFlag) {
          result.emplace_back(i, bt);
        }
        // concatenate all list values in one option
        result.emplace_back(cmJoin(opts, wrapperSep), bt);
      }
    } else {
      // prefix each element of list with wrapper
      if (concatFlagAndArgs) {
        std::transform(opts.begin(), opts.end(), opts.begin(),
                       [&wrapperFlag](std::string const& o) -> std::string {
                         return wrapperFlag.back() + o;
                       });
      }
      for (std::string& o : opts) {
        for (auto i = wrapperFlag.begin(),
                  e = concatFlagAndArgs ? wrapperFlag.end() - 1
                                        : wrapperFlag.end();
             i != e; ++i) {
          result.emplace_back(*i, bt);
        }
        result.emplace_back(std::move(o), bt);
      }
    }
  };

  if (nestedLinkerFlags == NestedLinkerFlags::PreserveAsSpelled) {
    insertWrapped(options);
    return result;
  }

  for (std::vector<std::string>::size_type index = 0; index < options.size();
       index++) {
    if (cmHasLiteralPrefix(options[index], "LINKER:")) {
      // LINKER wrapper specified, insert elements as is
      result.emplace_back(std::move(options[index]), bt);
      continue;
    }
    if (cmHasLiteralPrefix(options[index], "-Wl,")) {
      // replace option by LINKER wrapper
      result.emplace_back(options[index].replace(0, 4, "LINKER:"), bt);
      continue;
    }
    if (cmHasLiteralPrefix(options[index], "-Xlinker=")) {
      // replace option by LINKER wrapper
      result.emplace_back(options[index].replace(0, 9, "LINKER:"), bt);
      continue;
    }
    if (options[index] == "-Xlinker") {
      // replace option by LINKER wrapper
      if (index + 1 < options.size()) {
        result.emplace_back("LINKER:" + options[++index], bt);
      } else {
        result.emplace_back(std::move(options[index]), bt);
      }
      continue;
    }

    // collect all options which must be transformed
    std::vector<std::string> opts;
    while (index < options.size()) {
      if (!cmHasLiteralPrefix(options[index], "LINKER:") &&
          !cmHasLiteralPrefix(options[index], "-Wl,") &&
          !cmHasLiteralPrefix(options[index], "-Xlinker")) {
        opts.emplace_back(std::move(options[index++]));
      } else {
        --index;
        break;
      }
    }
    if (opts.empty()) {
      continue;
    }

    insertWrapped(opts);
  }
  return result;
}
}

void cmGeneratorTarget::GetCompileOptions(std::vector<std::string>& result,
                                          std::string const& config,
                                          std::string const& language) const
{
  std::vector<BT<std::string>> tmp = this->GetCompileOptions(config, language);
  result.reserve(tmp.size());
  for (BT<std::string>& v : tmp) {
    result.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmGeneratorTarget::GetCompileOptions(
  std::string const& config, std::string const& language) const
{
  ConfigAndLanguage cacheKey(config, language);
  {
    auto it = this->CompileOptionsCache.find(cacheKey);
    if (it != this->CompileOptionsCache.end()) {
      return it->second;
    }
  }
  std::vector<BT<std::string>> result;
  std::unordered_set<std::string> uniqueOptions;

  cmGeneratorExpressionDAGChecker dagChecker{
    this, "COMPILE_OPTIONS", nullptr, nullptr, this->LocalGenerator, config,
  };

  cmList debugProperties{ this->m_pMakefile->GetDefinition(
    "CMAKE_DEBUG_TARGET_PROPERTIES") };
  bool debugOptions = !this->DebugCompileOptionsDone &&
    cm::contains(debugProperties, "COMPILE_OPTIONS");

  this->DebugCompileOptionsDone = true;

  EvaluatedTargetPropertyEntries entries = EvaluateTargetPropertyEntries(
    this, config, language, &dagChecker, this->CompileOptionsEntries);

  AddInterfaceEntries(this, config, "INTERFACE_COMPILE_OPTIONS", language,
                      &dagChecker, entries, IncludeRuntimeInterface::Yes);

  processOptions(this, entries, result, uniqueOptions, debugOptions,
                 "compile options", OptionsParse::Shell);

  CompileOptionsCache.emplace(cacheKey, result);
  return result;
}

void cmGeneratorTarget::GetCompileFeatures(std::vector<std::string>& result,
                                           std::string const& config) const
{
  std::vector<BT<std::string>> tmp = this->GetCompileFeatures(config);
  result.reserve(tmp.size());
  for (BT<std::string>& v : tmp) {
    result.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmGeneratorTarget::GetCompileFeatures(
  std::string const& config) const
{
  std::vector<BT<std::string>> result;
  std::unordered_set<std::string> uniqueFeatures;

  cmGeneratorExpressionDAGChecker dagChecker{
    this, "COMPILE_FEATURES", nullptr, nullptr, this->LocalGenerator, config,
  };

  cmList debugProperties{ this->m_pMakefile->GetDefinition(
    "CMAKE_DEBUG_TARGET_PROPERTIES") };
  bool debugFeatures = !this->DebugCompileFeaturesDone &&
    cm::contains(debugProperties, "COMPILE_FEATURES");

  this->DebugCompileFeaturesDone = true;

  EvaluatedTargetPropertyEntries entries = EvaluateTargetPropertyEntries(
    this, config, std::string(), &dagChecker, this->CompileFeaturesEntries);

  AddInterfaceEntries(this, config, "INTERFACE_COMPILE_FEATURES",
                      std::string(), &dagChecker, entries,
                      IncludeRuntimeInterface::Yes);

  processOptions(this, entries, result, uniqueFeatures, debugFeatures,
                 "compile features", OptionsParse::None);

  return result;
}

void cmGeneratorTarget::GetCompileDefinitions(
  std::vector<std::string>& result, std::string const& config,
  std::string const& language) const
{
  std::vector<BT<std::string>> tmp =
    this->GetCompileDefinitions(config, language);
  result.reserve(tmp.size());
  for (BT<std::string>& v : tmp) {
    result.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmGeneratorTarget::GetCompileDefinitions(
  std::string const& config, std::string const& language) const
{
  ConfigAndLanguage cacheKey(config, language);
  {
    auto it = this->CompileDefinitionsCache.find(cacheKey);
    if (it != this->CompileDefinitionsCache.end()) {
      return it->second;
    }
  }
  std::vector<BT<std::string>> list;
  std::unordered_set<std::string> uniqueOptions;

  cmGeneratorExpressionDAGChecker dagChecker{
    this,    "COMPILE_DEFINITIONS", nullptr,
    nullptr, this->LocalGenerator,  config,
  };

  cmList debugProperties{ this->m_pMakefile->GetDefinition(
    "CMAKE_DEBUG_TARGET_PROPERTIES") };
  bool debugDefines = !this->DebugCompileDefinitionsDone &&
    cm::contains(debugProperties, "COMPILE_DEFINITIONS");

  this->DebugCompileDefinitionsDone = true;

  EvaluatedTargetPropertyEntries entries = EvaluateTargetPropertyEntries(
    this, config, language, &dagChecker, this->CompileDefinitionsEntries);

  AddInterfaceEntries(this, config, "INTERFACE_COMPILE_DEFINITIONS", language,
                      &dagChecker, entries, IncludeRuntimeInterface::Yes);

  processOptions(this, entries, list, uniqueOptions, debugDefines,
                 "compile definitions", OptionsParse::None);

  this->CompileDefinitionsCache.emplace(cacheKey, list);
  return list;
}

std::vector<BT<std::string>> cmGeneratorTarget::GetPrecompileHeaders(
  std::string const& config, std::string const& language) const
{
  ConfigAndLanguage cacheKey(config, language);
  {
    auto it = this->PrecompileHeadersCache.find(cacheKey);
    if (it != this->PrecompileHeadersCache.end()) {
      return it->second;
    }
  }
  std::unordered_set<std::string> uniqueOptions;

  cmGeneratorExpressionDAGChecker dagChecker{
    this, "PRECOMPILE_HEADERS", nullptr, nullptr, this->LocalGenerator, config,
  };

  cmList debugProperties{ this->m_pMakefile->GetDefinition(
    "CMAKE_DEBUG_TARGET_PROPERTIES") };
  bool debugDefines = !this->DebugPrecompileHeadersDone &&
    std::find(debugProperties.begin(), debugProperties.end(),
              "PRECOMPILE_HEADERS") != debugProperties.end();

  this->DebugPrecompileHeadersDone = true;

  EvaluatedTargetPropertyEntries entries = EvaluateTargetPropertyEntries(
    this, config, language, &dagChecker, this->PrecompileHeadersEntries);

  AddInterfaceEntries(this, config, "INTERFACE_PRECOMPILE_HEADERS", language,
                      &dagChecker, entries, IncludeRuntimeInterface::Yes);

  std::vector<BT<std::string>> list;
  processOptions(this, entries, list, uniqueOptions, debugDefines,
                 "precompile headers", OptionsParse::None);

  this->PrecompileHeadersCache.emplace(cacheKey, list);
  return list;
}

void cmGeneratorTarget::GetLinkOptions(std::vector<std::string>& result,
                                       std::string const& config,
                                       std::string const& language) const
{
  if (this->IsDeviceLink() &&
      this->GetPolicyStatusCMP0105() != cmPolicies::NEW) {
    // link options are not propagated to the device link step
    return;
  }

  std::vector<BT<std::string>> tmp = this->GetLinkOptions(config, language);
  result.reserve(tmp.size());
  for (BT<std::string>& v : tmp) {
    result.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmGeneratorTarget::GetLinkOptions(
  std::string const& config, std::string const& language) const
{
  ConfigAndLanguage cacheKey(
    config, cmStrCat(language, this->IsDeviceLink() ? "-device" : ""));
  {
    auto it = this->LinkOptionsCache.find(cacheKey);
    if (it != this->LinkOptionsCache.end()) {
      return it->second;
    }
  }
  std::vector<BT<std::string>> result;
  std::unordered_set<std::string> uniqueOptions;

  cmGeneratorExpressionDAGChecker dagChecker{
    this, "LINK_OPTIONS", nullptr, nullptr, this->LocalGenerator, config,
  };

  cmList debugProperties{ this->m_pMakefile->GetDefinition(
    "CMAKE_DEBUG_TARGET_PROPERTIES") };
  bool debugOptions = !this->DebugLinkOptionsDone &&
    cm::contains(debugProperties, "LINK_OPTIONS");

  this->DebugLinkOptionsDone = true;

  EvaluatedTargetPropertyEntries entries = EvaluateTargetPropertyEntries(
    this, config, language, &dagChecker, this->LinkOptionsEntries);

  AddInterfaceEntries(this, config, "INTERFACE_LINK_OPTIONS", language,
                      &dagChecker, entries, IncludeRuntimeInterface::Yes,
                      this->GetPolicyStatusCMP0099() == cmPolicies::NEW
                        ? UseTo::Link
                        : UseTo::Compile);

  processOptions(this, entries, result, uniqueOptions, debugOptions,
                 "link options", OptionsParse::Shell, this->IsDeviceLink());

  if (this->IsDeviceLink()) {
    // wrap host link options
    std::string const wrapper(this->m_pMakefile->GetSafeDefinition(
      "CMAKE_" + language + "_DEVICE_COMPILER_WRAPPER_FLAG"));
    cmList wrapperFlag{ wrapper };
    std::string const wrapperSep(this->m_pMakefile->GetSafeDefinition(
      "CMAKE_" + language + "_DEVICE_COMPILER_WRAPPER_FLAG_SEP"));
    bool concatFlagAndArgs = true;
    if (!wrapperFlag.empty() && wrapperFlag.back() == " ") {
      concatFlagAndArgs = false;
      wrapperFlag.pop_back();
    }

    auto it = result.begin();
    while (it != result.end()) {
      if (it->Value == DL_BEGIN) {
        // device link options, no treatment
        it = result.erase(it);
        it = std::find_if(it, result.end(), [](const BT<std::string>& item) {
          return item.Value == DL_END;
        });
        if (it != result.end()) {
          it = result.erase(it);
        }
      } else {
        // host link options must be wrapped
        std::vector<std::string> options;
        cmSystemTools::ParseUnixCommandLine(it->Value.c_str(), options);
        auto hostOptions =
          wrapOptions(options, it->m_backtrace, wrapperFlag, wrapperSep,
                      concatFlagAndArgs, NestedLinkerFlags::Normalize);
        it = result.erase(it);
        // some compilers (like gcc 4.8 or Intel 19.0 or XLC 16) do not respect
        // C++11 standard: 'std::vector::insert()' do not returns an iterator,
        // so need to recompute the iterator after insertion.
        if (it == result.end()) {
          cm::append(result, hostOptions);
          it = result.end();
        } else {
          auto index = it - result.begin();
          result.insert(it, hostOptions.begin(), hostOptions.end());
          it = result.begin() + index + hostOptions.size();
        }
      }
    }
  }

  // Last step: replace "LINKER:" prefixed elements by
  // actual linker wrapper
  result = this->ResolveLinkerWrapper(result, language);

  this->LinkOptionsCache.emplace(cacheKey, result);
  return result;
}

std::vector<BT<std::string>>& cmGeneratorTarget::ResolvePrefixWrapper(
  std::vector<BT<std::string>>& result, cm::string_view prefix,
  std::string const& language, bool joinItems) const
{
  // replace "LINKER:" or "ARCHIVER:" prefixed elements by actual linker or
  // archiver wrapper
  std::string const wrapper(this->m_pMakefile->GetSafeDefinition(
    cmStrCat("CMAKE_", language, (this->IsDeviceLink() ? "_DEVICE_" : "_"),
             prefix, "_WRAPPER_FLAG")));
  cmList wrapperFlag{ wrapper };
  std::string const wrapperSep(this->m_pMakefile->GetSafeDefinition(
    cmStrCat("CMAKE_", language, (this->IsDeviceLink() ? "_DEVICE_" : "_"),
             prefix, "_WRAPPER_FLAG_SEP")));
  bool concatFlagAndArgs = true;
  if (!wrapperFlag.empty() && wrapperFlag.back() == " ") {
    concatFlagAndArgs = false;
    wrapperFlag.pop_back();
  }

  std::string const PREFIX{ cmStrCat(prefix, ':') };
  std::string const SHELL{ "SHELL:" };
  std::string const PREFIX_SHELL = cmStrCat(PREFIX, SHELL);

  for (auto entry = result.begin(); entry != result.end();) {
    if (entry->Value.compare(0, PREFIX.length(), PREFIX) != 0) {
      ++entry;
      continue;
    }

    std::string value = std::move(entry->Value);
    cmListFileBacktrace bt = std::move(entry->m_backtrace);
    entry = result.erase(entry);

    std::vector<std::string> options;
    if (value.compare(0, PREFIX_SHELL.length(), PREFIX_SHELL) == 0) {
      cmSystemTools::ParseUnixCommandLine(
        value.c_str() + PREFIX_SHELL.length(), options);
    } else {
      options =
        cmTokenize(value.substr(PREFIX.length()), ',', cmTokenizerMode::New);
    }

    if (options.empty()) {
      continue;
    }

    // for now, raise an error if prefix SHELL: is part of arguments
    if (std::find_if(options.begin(), options.end(),
                     [&SHELL](std::string const& item) -> bool {
                       return item.find(SHELL) != std::string::npos;
                     }) != options.end()) {
      this->LocalGenerator->GetCMakeInstance()->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("'SHELL:' prefix is not supported as part of '", prefix,
                 ":' arguments."),
        this->GetBacktrace());
      return result;
    }

    // Very old versions of the C++ standard library return void for insert, so
    // can't use it to get the new iterator
    auto const index = entry - result.begin();
    std::vector<BT<std::string>> processedOptions =
      wrapOptions(options, bt, wrapperFlag, wrapperSep, concatFlagAndArgs,
                  NestedLinkerFlags::PreserveAsSpelled);
    if (joinItems) {
      result.insert(
        entry,
        cmJoin(cmMakeRange(processedOptions.begin(), processedOptions.end()),
               " "_s));
      entry = std::next(result.begin(), index + 1);
    } else {
      result.insert(entry, processedOptions.begin(), processedOptions.end());
      entry = std::next(result.begin(), index + processedOptions.size());
    }
  }
  return result;
}

std::vector<BT<std::string>>& cmGeneratorTarget::ResolveLinkerWrapper(
  std::vector<BT<std::string>>& result, std::string const& language,
  bool joinItems) const
{
  return this->ResolvePrefixWrapper(result, "LINKER"_s, language, joinItems);
}

void cmGeneratorTarget::GetStaticLibraryLinkOptions(
  std::vector<std::string>& result, std::string const& config,
  std::string const& language) const
{
  std::vector<BT<std::string>> tmp =
    this->GetStaticLibraryLinkOptions(config, language);
  result.reserve(tmp.size());
  for (BT<std::string>& v : tmp) {
    result.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmGeneratorTarget::GetStaticLibraryLinkOptions(
  std::string const& config, std::string const& language) const
{
  std::vector<BT<std::string>> result;
  std::unordered_set<std::string> uniqueOptions;

  cmGeneratorExpressionDAGChecker dagChecker{
    this,    "STATIC_LIBRARY_OPTIONS", nullptr,
    nullptr, this->LocalGenerator,     config,
  };

  EvaluatedTargetPropertyEntries entries;
  if (cmValue linkOptions = this->GetProperty("STATIC_LIBRARY_OPTIONS")) {
    std::unique_ptr<TargetPropertyEntry> entry = TargetPropertyEntry::Create(
      *this->LocalGenerator->GetCMakeInstance(), *linkOptions);
    entries.Entries.emplace_back(EvaluateTargetPropertyEntry(
      this, config, language, &dagChecker, *entry));
  }
  processOptions(this, entries, result, uniqueOptions, false,
                 "static library link options", OptionsParse::Shell);

  // Last step: replace "ARCHIVER:" prefixed elements by
  // actual archiver wrapper
  this->ResolveArchiverWrapper(result, language);

  return result;
}

std::vector<BT<std::string>>& cmGeneratorTarget::ResolveArchiverWrapper(
  std::vector<BT<std::string>>& result, std::string const& language,
  bool joinItems) const
{
  return this->ResolvePrefixWrapper(result, "ARCHIVER"_s, language, joinItems);
}

void cmGeneratorTarget::GetLinkDepends(std::vector<std::string>& result,
                                       std::string const& config,
                                       std::string const& language) const
{
  std::vector<BT<std::string>> tmp = this->GetLinkDepends(config, language);
  result.reserve(tmp.size());
  for (BT<std::string>& v : tmp) {
    result.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmGeneratorTarget::GetLinkDepends(
  std::string const& config, std::string const& language) const
{
  std::vector<BT<std::string>> result;
  std::unordered_set<std::string> uniqueOptions;
  cmGeneratorExpressionDAGChecker dagChecker{
    this, "LINK_DEPENDS", nullptr, nullptr, this->LocalGenerator, config,
  };

  EvaluatedTargetPropertyEntries entries;
  if (cmValue linkDepends = this->GetProperty("LINK_DEPENDS")) {
    cmList depends{ *linkDepends };
    for (auto const& depend : depends) {
      std::unique_ptr<TargetPropertyEntry> entry = TargetPropertyEntry::Create(
        *this->LocalGenerator->GetCMakeInstance(), depend);
      entries.Entries.emplace_back(EvaluateTargetPropertyEntry(
        this, config, language, &dagChecker, *entry));
    }
  }
  AddInterfaceEntries(this, config, "INTERFACE_LINK_DEPENDS", language,
                      &dagChecker, entries, IncludeRuntimeInterface::Yes,
                      this->GetPolicyStatusCMP0099() == cmPolicies::NEW
                        ? UseTo::Link
                        : UseTo::Compile);

  processOptions(this, entries, result, uniqueOptions, false, "link depends",
                 OptionsParse::None);

  return result;
}
