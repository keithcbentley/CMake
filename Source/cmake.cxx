/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmake.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <cm/filesystem>
#include <cm/memory>
#include <cm/optional>
#include <cm/string_view>

#include "cmakeMessage.h"
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(CMAKE_BOOT_MINGW)
#  include <cm/iterator>
#endif

#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmsys/FStream.hxx"
#include "cmsys/Glob.hxx"
#include "cmsys/RegularExpression.hxx"

#include "cm_sys_stat.h"

#include "cmBuildOptions.h"
#include "cmCMakePath.h"
#include "cmCMakePresetsGraph.h"
#include "cmCommandLineArgument.h"
#include "cmCommands.h"
#ifdef CMake_ENABLE_DEBUGGER
#  include "cmDebuggerAdapter.h"
#  ifdef _WIN32
#    include "cmDebuggerWindowsPipeConnection.h"
#  else //!_WIN32
#    include "cmDebuggerPosixPipeConnection.h"
#  endif //_WIN32
#endif
#include "cmDocumentation.h"
#include "cmDocumentationEntry.h"
#include "cmDuration.h"
#include "cmExternalMakefileProjectGenerator.h"
#include "cmFileTimeCache.h"
#include "cmGeneratorTarget.h"
#include "cmGlobCacheEntry.h"
#include "cmGlobalGenerator.h"
#include "cmGlobalGeneratorFactory.h"
#include "cmLinkLineComputer.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#if !defined(CMAKE_BOOTSTRAP)
#  include "cmMakefileProfilingData.h"
#endif
#include "cmJSONState.h"
#include "cmList.h"
#include "cmMessenger.h"
#ifndef CMAKE_BOOTSTRAP
#  include "cmSarifLog.h"
#endif
#include "cmState.h"
#include "cmStateDirectory.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTargetLinkLibraryType.h"
#include "cmUVProcessChain.h"
#include "cmUtils.hxx"
#include "cmVersionConfig.h"
#include "cmWorkingDirectory.h"

#if !defined(CMAKE_BOOTSTRAP)
#  include <unordered_map>

#  include <cm3p/curl/curl.h>
#  include <cm3p/json/writer.h>

#  include "cmConfigureLog.h"
#  include "cmFileAPI.h"
#  include "cmGraphVizWriter.h"
#  include "cmVariableWatch.h"
#endif

#if defined(__MINGW32__) && defined(CMAKE_BOOTSTRAP)
#  define CMAKE_BOOT_MINGW
#endif

// include the generator
#if defined(_WIN32) && !defined(__CYGWIN__)
#  if !defined(CMAKE_BOOT_MINGW)
#    include <cmext/memory>

#    include "cmGlobalBorlandMakefileGenerator.h"
#    include "cmGlobalJOMMakefileGenerator.h"
#    include "cmGlobalNMakeMakefileGenerator.h"
#    include "cmGlobalVisualStudio14Generator.h"
#    include "cmGlobalVisualStudioVersionedGenerator.h"
#    include "cmVSSetupHelper.h"

#    define CMAKE_HAVE_VS_GENERATORS
#  endif
#  include "cmGlobalMSYSMakefileGenerator.h"
#  include "cmGlobalMinGWMakefileGenerator.h"
#else
#endif
#if defined(CMAKE_USE_WMAKE)
#  include "cmGlobalWatcomWMakeGenerator.h"
#endif
#if !defined(CMAKE_BOOTSTRAP)
#  include "cmGlobalNinjaGenerator.h"
#  include "cmGlobalUnixMakefileGenerator3.h"
#elif defined(CMAKE_BOOTSTRAP_MAKEFILES)
#  include "cmGlobalUnixMakefileGenerator3.h"
#elif defined(CMAKE_BOOTSTRAP_NINJA)
#  include "cmGlobalNinjaGenerator.h"
#endif

#if !defined(CMAKE_BOOTSTRAP)
#  include "cmExtraCodeBlocksGenerator.h"
#  include "cmExtraCodeLiteGenerator.h"
#  include "cmExtraEclipseCDT4Generator.h"
#  include "cmExtraKateGenerator.h"
#  include "cmExtraSublimeTextGenerator.h"
#endif

// NOTE: the __linux__ macro is predefined on Android host too, but
// main CMakeLists.txt filters out this generator by host name.
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
#  include "cmGlobalGhsMultiGenerator.h"
#endif

#if defined(__APPLE__)
#  if !defined(CMAKE_BOOTSTRAP)
#    include "cmGlobalXCodeGenerator.h"

#    define CMAKE_USE_XCODE 1
#  endif
#  include <sys/resource.h>
#  include <sys/time.h>
#endif

namespace {

#if !defined(CMAKE_BOOTSTRAP)
using JsonValueMapType = std::unordered_map<std::string, Json::Value>;
#endif

auto IgnoreAndTrueLambda = [](std::string const&, CMake*) -> bool { return true; };

using CommandArgument = cmCommandLineArgument<bool(std::string const& value, CMake* state)>;

#ifndef CMAKE_BOOTSTRAP
void cmWarnUnusedCliWarning(
  std::string const& variable,
  int /*unused*/,
  void* ctx,
  char const* /*unused*/,
  cmMakefile const* /*unused*/)
{
  CMake* cm = reinterpret_cast<CMake*>(ctx);
  cm->MarkCliAsUsed(variable);
}
#endif

bool isStampFileUpToDate(std::string const& stampName)
{
  // The stamp file does not exist.  Use the stamp dependencies to
  // determine whether it is really out of date.  This works in
  // conjunction with cmLocalVisualStudio7Generator to avoid
  // repeatedly re-running CMake when the user rebuilds the entire
  // solution.
  std::string stampDepends = cmStrCat(stampName, ".depend");
#if defined(_WIN32) || defined(__CYGWIN__)
  cmsys::ifstream fin(stampDepends.c_str(), std::ios::in | std::ios::binary);
#else
  cmsys::ifstream fin(stampDepends.c_str());
#endif
  if (!fin) {
    // The stamp dependencies file cannot be read.  Just assume the
    // build system is really out of date.
    std::cout << "CMake is re-running because " << stampName << " dependency file is missing.\n";
    return false;
  }

  // Compare the stamp dependencies against the dependency file itself.
  {
    cmFileTimeCache ftc;
    std::string dep;
    while (cmSystemTools::GetLineFromStream(fin, dep)) {
      int result;
      if (!dep.empty() && dep[0] != '#' && (!ftc.Compare(stampDepends, dep, &result) || result < 0)) {
        // The stamp depends file is older than this dependency.  The
        // build system is really out of date.
        /* clang-format off */
        std::cout << "CMake is re-running because " << stampName
                  << " is out-of-date.\n"
                     "  the file '" << dep << "'\n"
                     "  is newer than '" << stampDepends << "'\n"
                     "  result='" << result << "'\n";
        /* clang-format on */
        return false;
      }
    }
  }

  // The build system is up to date.  The stamp file has been removed
  // by the VS IDE due to a "rebuild" request.  Restore it atomically.
  std::ostringstream stampTempStream;
  stampTempStream << stampName << ".tmp" << cmSystemTools::RandomNumber();
  std::string stampTemp = stampTempStream.str();
  {
    // TODO: Teach cmGeneratedFileStream to use a random temp file (with
    // multiple tries in unlikely case of conflict) and use that here.
    cmsys::ofstream stamp(stampTemp.c_str());
    stamp << "# CMake generation timestamp file for this directory.\n";
  }
  std::string err;
  if (
    cmSystemTools::RenameFile(stampTemp, stampName, cmSystemTools::Replace::Yes, &err) ==
    cmSystemTools::RenameResult::Success) {
    // CMake does not need to re-run because the stamp file is up-to-date.
    return true;
  }
  cmSystemTools::RemoveFile(stampTemp);
  cmSystemTools::Error(cmStrCat("Cannot restore timestamp \"", stampName, "\": ", err));
  return false;
}

bool isGenerateStampListUpToDate(std::string const& stampList)
{
  // If the stamp list does not exist CMake must rerun to generate it.
  if (!cmSystemTools::FileExists(stampList)) {
    std::cout << "CMake is re-running because generate.stamp.list "
                 "is missing.\n";
    return false;
  }
  cmsys::ifstream fin(stampList.c_str());
  if (!fin) {
    std::cout << "CMake is re-running because generate.stamp.list "
                 "could not be read.\n";
    return false;
  }

  // Check each stamp.
  std::string stampName;
  while (cmSystemTools::GetLineFromStream(fin, stampName)) {
    if (!isStampFileUpToDate(stampName)) {
      return false;
    }
  }
  return true;
}

} // namespace

// cmDocumentationEntry CMake::CMAKE_STANDARD_OPTIONS_TABLE[19] = {
//   { "-S <path-to-source>", "Explicitly specify a source directory." },
//   { "-B <path-to-build>", "Explicitly specify a build directory." },
//   { "-C <initial-cache>", "Pre-load a script to populate the cache." },
//   { "-D <var>[:<type>]=<value>", "Create or update a cmake cache entry." },
//   { "-U <globbing_expr>", "Remove matching entries from CMake cache." },
//   { "-G <generator-name>", "Specify a build system generator." },
//   { "-T <toolset-name>", "Specify toolset name if supported by generator." },
//   { "-A <platform-name>", "Specify platform name if supported by generator." },
//   { "--toolchain <file>", "Specify toolchain file [CMAKE_TOOLCHAIN_FILE]." },
//   { "--install-prefix <directory>", "Specify install directory [CMAKE_INSTALL_PREFIX]." },
//   { "--project-file <project-file-name>", "Specify an alternate project file name." },
//   { "-Wdev", "Enable developer warnings." },
//   { "-Wno-dev", "Suppress developer warnings." },
//   { "-Werror=dev", "Make developer warnings errors." },
//   { "-Wno-error=dev", "Make developer warnings not errors." },
//   { "-Wdeprecated", "Enable deprecation warnings." },
//   { "-Wno-deprecated", "Suppress deprecation warnings." },
//   { "-Werror=deprecated",
//     "Make deprecated macro and function warnings "
//     "errors." },
//   { "-Wno-error=deprecated",
//     "Make deprecated macro and function warnings "
//     "not errors." }
// };

CMake::CMake(
  Role role,
  cmState::Mode mode,
  cmState::ProjectKind projectKind)
  : m_cmakeWorkingDirectory(cmSystemTools::GetLogicalWorkingDirectory())
  , m_fileTimeCache(cm::make_unique<cmFileTimeCache>())
#ifndef CMAKE_BOOTSTRAP
  , m_pVariableWatch(cm::make_unique<cmVariableWatch>())
#endif
  , m_pState(
      cm::make_unique<cmState>(
        mode,
        projectKind))
  , m_pMessenger(cm::make_unique<cmMessenger>())
{
  m_traceFile.close();
  m_currentSnapshot = m_pState->CreateBaseSnapshot();

#ifdef __APPLE__
  struct rlimit rlp;
  if (!getrlimit(RLIMIT_STACK, &rlp)) {
    if (rlp.rlim_cur != rlp.rlim_max) {
      rlp.rlim_cur = rlp.rlim_max;
      setrlimit(RLIMIT_STACK, &rlp);
    }
  }
#endif

  AddDefaultGenerators();
  AddDefaultExtraGenerators();
  //    TODO: does this really need to be conditional? Can't we just add and load everything?
  if (role == RoleScript || role == RoleProject) {
    AddScriptingCommands();
  }
  if (role == RoleProject) {
    AddProjectCommands();
  }

  if (mode == cmState::Project || mode == cmState::Help) {
    LoadEnvironmentPresets();
  }

  // Make sure we can capture the build tool output.
  cmSystemTools::EnableVSConsoleOutput();

  // Set up a list of source and header extensions.
  // These are used to find files when the extension is not given.
  //    TODO: does this really need a lambda in the middle of this code?
  {
    auto setupExts = [](FileExtensions& exts, std::initializer_list<cm::string_view> extList) {
      // Fill ordered vector
      exts.ordered.reserve(extList.size());
      for (cm::string_view ext : extList) {
        exts.ordered.emplace_back(ext);
      }
      // Fill unordered set
      exts.unordered.insert(exts.ordered.begin(), exts.ordered.end());
    };

    // The "c" extension MUST precede the "C" extension.
    //  TODO: why?
    setupExts(
      m_CLikeSourceFileExtensions,
      { "c", "C", "c++", "cc", "cpp", "cxx", "cu", "mpp", "m", "M", "mm", "ixx", "cppm", "ccm", "cxxm", "c++m" });
    setupExts(m_headerFileExtensions, { "h", "hh", "h++", "hm", "hpp", "hxx", "in", "txx" });
    setupExts(m_cudaFileExtensions, { "cu" });
    setupExts(m_FortranFileExtensions, { "f", "F", "for", "f77", "f90", "f95", "f03" });
    setupExts(m_HipFileExtensions, { "hip" });
    setupExts(m_ISPCFileExtensions, { "ispc" });
  }
}

CMake::~CMake() = default;

#if !defined(CMAKE_BOOTSTRAP)
Json::Value CMake::ReportVersionJson() const
{
  Json::Value version = Json::objectValue;
  version["string"] = CMake_VERSION;
  version["major"] = CMake_VERSION_MAJOR;
  version["minor"] = CMake_VERSION_MINOR;
  version["suffix"] = CMake_VERSION_SUFFIX;
  version["isDirty"] = (CMake_VERSION_IS_DIRTY == 1);
  version["patch"] = CMake_VERSION_PATCH;
  return version;
}

Json::Value CMake::ReportCapabilitiesJson() const
{
  Json::Value obj = Json::objectValue;

  // Version information:
  obj["version"] = ReportVersionJson();

  // Generators:
  std::vector<CMake::GeneratorInfo> generatorInfoList;
  GetRegisteredGenerators(generatorInfoList);

  auto* curlVersion = curl_version_info(CURLVERSION_FIRST);

  JsonValueMapType generatorMap;
  for (CMake::GeneratorInfo const& gi : generatorInfoList) {
    if (gi.isAlias) { // skip aliases, they are there for compatibility reasons
                      // only
      continue;
    }

    if (gi.extraName.empty()) {
      Json::Value gen = Json::objectValue;
      gen["name"] = gi.name;
      gen["toolsetSupport"] = gi.supportsToolset;
      gen["platformSupport"] = gi.supportsPlatform;
      if (!gi.supportedPlatforms.empty()) {
        Json::Value supportedPlatforms = Json::arrayValue;
        for (std::string const& platform : gi.supportedPlatforms) {
          supportedPlatforms.append(platform);
        }
        gen["supportedPlatforms"] = std::move(supportedPlatforms);
      }
      gen["extraGenerators"] = Json::arrayValue;
      generatorMap[gi.name] = gen;
    } else {
      Json::Value& gen = generatorMap[gi.baseName];
      gen["extraGenerators"].append(gi.extraName);
    }
  }

  Json::Value generators = Json::arrayValue;
  for (auto const& i : generatorMap) {
    generators.append(i.second);
  }
  obj["generators"] = generators;
  obj["fileApi"] = cmFileAPI::ReportCapabilities();
  obj["serverMode"] = false;
  obj["tls"] = static_cast<bool>(curlVersion->features & CURL_VERSION_SSL);
#  ifdef CMake_ENABLE_DEBUGGER
  obj["debugger"] = true;
#  else
  obj["debugger"] = false;
#  endif

  return obj;
}
#endif

std::string CMake::ReportCapabilities() const
{
  std::string result;
#if !defined(CMAKE_BOOTSTRAP)
  Json::FastWriter writer;
  result = writer.write(ReportCapabilitiesJson());
#else
  result = "Not supported";
#endif
  return result;
}

void CMake::CleanupCommandsAndMacros()
{
  m_currentSnapshot = m_pState->Reset();
  m_pState->RemoveUserDefinedCommands();
  m_currentSnapshot.SetDefaultDefinitions();
  // FIXME: InstalledFiles probably belongs in the global generator.
  m_installedFiles.clear();
}

#ifndef CMAKE_BOOTSTRAP
void CMake::SetWarningFromPreset(
  std::string const& name,
  cm::optional<bool> const& warning,
  cm::optional<bool> const& error)
{
  if (warning) {
    if (*warning) {
      m_diagLevels[name] = std::max(m_diagLevels[name], DIAG_WARN);
    } else {
      m_diagLevels[name] = DIAG_IGNORE;
    }
  }
  if (error) {
    if (*error) {
      m_diagLevels[name] = DIAG_ERROR;
    } else {
      m_diagLevels[name] = std::min(m_diagLevels[name], DIAG_WARN);
    }
  }
}

void CMake::ProcessPresetVariables()
{
  FunctionTrace f(__func__);

  for (auto const& var : m_unprocessedPresetVariables) {
    if (!var.second) {
      continue;
    }
    cmStateEnums::CacheEntryType type = cmStateEnums::UNINITIALIZED;
    if (!var.second->Type.empty()) {
      type = cmState::StringToCacheEntryType(var.second->Type);
    }
    ProcessCacheArg(var.first, var.second->Value, type);
  }
}

void CMake::PrintPresetVariables()
{
  bool first = true;
  for (auto const& var : m_unprocessedPresetVariables) {
    if (!var.second) {
      continue;
    }
    cmStateEnums::CacheEntryType type = cmStateEnums::UNINITIALIZED;
    if (!var.second->Type.empty()) {
      type = cmState::StringToCacheEntryType(var.second->Type);
    }
    if (first) {
      std::cout << "Preset CMake variables:\n\n";
      first = false;
    }
    std::cout << "  " << var.first;
    if (type != cmStateEnums::UNINITIALIZED) {
      std::cout << ':' << cmState::CacheEntryTypeToString(type);
    }
    std::cout << "=\"" << var.second->Value << "\"\n";
  }
  if (!first) {
    std::cout << '\n';
  }
  m_unprocessedPresetVariables.clear();
}

void CMake::ProcessPresetEnvironment()
{
  FunctionTrace f(__func__);

  for (auto const& var : m_unprocessedPresetEnvironment) {
    if (var.second) {
      cmSystemTools::PutEnv(cmStrCat(var.first, '=', *var.second));
    }
  }
}

void CMake::PrintPresetEnvironment()
{
  bool first = true;
  for (auto const& var : m_unprocessedPresetEnvironment) {
    if (!var.second) {
      continue;
    }
    if (first) {
      std::cout << "Preset environment variables:\n\n";
      first = false;
    }
    std::cout << "  " << var.first << "=\"" << *var.second << "\"\n";
  }
  if (!first) {
    std::cout << '\n';
  }
  m_unprocessedPresetEnvironment.clear();
}
#endif

// Parse the args
bool CMake::SetCacheArgs(std::vector<std::string> const& args)
{
  FunctionTrace f(__func__);

  //  TODO: Should errors throw?
  static std::string const kCMAKE_POLICY_VERSION_MINIMUM = "CMAKE_POLICY_VERSION_MINIMUM";
  if (!m_pState->GetInitializedCacheValue(kCMAKE_POLICY_VERSION_MINIMUM)) {
    cm::optional<std::string> policyVersion = cmSystemTools::GetEnvVar(kCMAKE_POLICY_VERSION_MINIMUM);
    if (policyVersion && !policyVersion->empty()) {
      AddCacheEntry(
        kCMAKE_POLICY_VERSION_MINIMUM, *policyVersion, "Override policy version for cmake_minimum_required calls.",
        cmStateEnums::STRING);
      m_pState->SetCacheEntryProperty(kCMAKE_POLICY_VERSION_MINIMUM, "ADVANCED", "1");
    }
  }

  //    TODO: What's with all the f'ing lambdas?
  auto DefineLambda = [](std::string const& entry, CMake* pCMake) -> bool {
    std::string var;
    std::string value;
    cmStateEnums::CacheEntryType type = cmStateEnums::UNINITIALIZED;
    if (cmState::ParseCacheEntry(entry, var, value, type)) {
#ifndef CMAKE_BOOTSTRAP
      pCMake->m_unprocessedPresetVariables.erase(var);
#endif
      pCMake->ProcessCacheArg(var, value, type);
    } else {
      cmSystemTools::Error(cmStrCat("Parse error in command line argument: ", entry, "\n Should be: VAR:type=value\n"));
      return false;
    }
    return true;
  };

  auto WarningLambda = [](cm::string_view entry, CMake* pCMake) -> bool {
    bool foundNo = false;
    bool foundError = false;

    if (cmHasLiteralPrefix(entry, "no-")) {
      foundNo = true;
      entry.remove_prefix(3);
    }

    if (cmHasLiteralPrefix(entry, "error=")) {
      foundError = true;
      entry.remove_prefix(6);
    }

    if (entry.empty()) {
      cmSystemTools::Error("No warning name provided.");
      return false;
    }

    std::string const name = std::string(entry);
    if (!foundNo && !foundError) {
      // -W<name>
      pCMake->m_diagLevels[name] = std::max(pCMake->m_diagLevels[name], DIAG_WARN);
    } else if (foundNo && !foundError) {
      // -Wno<name>
      pCMake->m_diagLevels[name] = DIAG_IGNORE;
    } else if (!foundNo && foundError) {
      // -Werror=<name>
      pCMake->m_diagLevels[name] = DIAG_ERROR;
    } else {
      // -Wno-error=<name>
      // This can downgrade an error to a warning, but should not enable
      // or disable a warning in the first place.
      auto dli = pCMake->m_diagLevels.find(name);
      if (dli != pCMake->m_diagLevels.end()) {
        dli->second = std::min(dli->second, DIAG_WARN);
      }
    }
    return true;
  };

  auto UnSetLambda = [](std::string const& entryPattern, CMake* pCMake) -> bool {
    cmsys::RegularExpression regex(cmsys::Glob::PatternToRegex(entryPattern, true, true));
    // go through all cache entries and collect the vars which will be
    // removed
    std::vector<std::string> entriesToDelete;
    std::vector<std::string> cacheKeys = pCMake->m_pState->GetCacheEntryKeys();
    for (std::string const& ck : cacheKeys) {
      cmStateEnums::CacheEntryType t = pCMake->m_pState->GetCacheEntryType(ck);
      if (t != cmStateEnums::STATIC) {
        if (regex.find(ck)) {
          entriesToDelete.push_back(ck);
        }
      }
    }

    // now remove them from the cache
    for (std::string const& currentEntry : entriesToDelete) {
#ifndef CMAKE_BOOTSTRAP
      pCMake->m_unprocessedPresetVariables.erase(currentEntry);
#endif
      pCMake->m_pState->RemoveCacheEntry(currentEntry);
    }
    return true;
  };

  auto ScriptLambda = [&](std::string const& path, CMake* pCMake) -> bool {
#ifdef CMake_ENABLE_DEBUGGER
    // Script mode doesn't hit the usual code path in cmake::Run() that starts
    // the debugger, so start it manually here instead.
    //    TODO: So maybe fix it to start the debugger properly?
    if (!StartDebuggerIfEnabled()) {
      return false;
    }
#endif
    // Register fake project commands that hint misuse in script mode.
    GetProjectCommandsInScriptMode(pCMake->GetState());
    // Documented behavior of CMAKE{,_CURRENT}_{SOURCE,BINARY}_DIR is to be
    // set to $PWD for -P mode.
    pCMake->SetWorkingMode(SCRIPT_MODE, CMake::CommandFailureAction::FATAL_ERROR);
    pCMake->SetHomeDirectory(cmSystemTools::GetLogicalWorkingDirectory());
    pCMake->SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());
    pCMake->ReadListFile(args, path);
    return true;
  };

  auto PrefixLambda = [&](std::string const& path, CMake* pCMake) -> bool {
    std::string const var = "CMAKE_INSTALL_PREFIX";
    cmStateEnums::CacheEntryType type = cmStateEnums::PATH;
    cmCMakePath absolutePath(path);
    if (absolutePath.IsAbsolute()) {
#ifndef CMAKE_BOOTSTRAP
      pCMake->m_unprocessedPresetVariables.erase(var);
#endif
      pCMake->ProcessCacheArg(var, path, type);
      return true;
    }
    cmSystemTools::Error("Absolute paths are required for --install-prefix");
    return false;
  };

  auto ToolchainLambda = [&](std::string const& path, CMake* pCMake) -> bool {
    std::string const var = "CMAKE_TOOLCHAIN_FILE";
    cmStateEnums::CacheEntryType type = cmStateEnums::FILEPATH;
#ifndef CMAKE_BOOTSTRAP
    pCMake->m_unprocessedPresetVariables.erase(var);
#endif
    pCMake->ProcessCacheArg(var, path, type);
    return true;
  };

  //    TODO: Why is this in the middle of this function?
  std::vector<CommandArgument> arguments = {
    CommandArgument{ "-D", "-D must be followed with VAR=VALUE.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, DefineLambda },
    CommandArgument{ "-W", "-W must be followed with [no-]<name>.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, WarningLambda },
    CommandArgument{ "-U", "-U must be followed with VAR.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, UnSetLambda },
    CommandArgument{ "-C", "-C must be followed by a file name.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No,
                     [&](std::string const& value, CMake* state) -> bool {
                       if (value.empty()) {
                         cmSystemTools::Error("No file name specified for -C");
                         return false;
                       }
                       cmSystemTools::Stdout("loading initial cache file " + value + "\n");
                       // Resolve script path specified on command line
                       // relative to $PWD.
                       auto path = cmSystemTools::ToNormalizedPathOnDisk(value);
                       state->ReadListFile(args, path);
                       return true;
                     } },

    CommandArgument{ "-P", "-P must be followed by a file name.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, ScriptLambda },
    CommandArgument{ "--toolchain", "No file specified for --toolchain", CommandArgument::Values::One,
                     ToolchainLambda },
    CommandArgument{ "--install-prefix", "No install directory specified for --install-prefix",
                     CommandArgument::Values::One, PrefixLambda },
    CommandArgument{ "--find-package", CommandArgument::Values::Zero, IgnoreAndTrueLambda },
  };
  for (decltype(args.size()) i = 1; i < args.size(); ++i) {
    std::string const& arg = args[i];

    if (arg == "--" && GetWorkingMode() == SCRIPT_MODE) {
      // Stop processing CMake args and avoid possible errors
      // when arbitrary args are given to CMake script.
      break;
    }
    for (auto const& m : arguments) {
      if (m.matches(arg)) {
        bool const parsedCorrectly = m.parse(arg, i, args, this);
        if (!parsedCorrectly) {
          return false;
        }
      }
    }
  }

  if (GetWorkingMode() == FIND_PACKAGE_MODE) {
    return FindPackage(args);
  }

  return true;
}

void CMake::ProcessCacheArg(
  std::string const& var,
  std::string const& value,
  cmStateEnums::CacheEntryType type)
{
  // The value is transformed if it is a filepath for example, so
  // we can't compare whether the value is already in the cache until
  // after we call AddCacheEntry.
  bool haveValue = false;
  std::string cachedValue;
  if (m_warnUnusedCli) {
    if (cmValue v = m_pState->GetInitializedCacheValue(var)) {
      haveValue = true;
      cachedValue = *v;
    }
  }

  AddCacheEntry(var, value, "No help, variable specified on the command line.", type);

  if (m_warnUnusedCli) {
    if (!haveValue || cachedValue != *m_pState->GetInitializedCacheValue(var)) {
      WatchUnusedCli(var);
    }
  }
}

void CMake::ReadListFile(
  std::vector<std::string> const& args,
  std::string const& path)
{
  FunctionTrace f(__func__);

  // if a generator was not yet created, temporarily create one
  cmGlobalGenerator* gg = GetGlobalGenerator();

  // if a generator was not specified use a generic one
  std::unique_ptr<cmGlobalGenerator> gen;
  if (!gg) {
    gen = cm::make_unique<cmGlobalGenerator>(this);
    gg = gen.get();
  }

  // read in the list file to fill the cache
  if (!path.empty()) {
    m_currentSnapshot = m_pState->Reset();
    cmStateSnapshot snapshot = GetCurrentSnapshot();
    snapshot.GetDirectory().SetCurrentBinary(GetHomeOutputDirectory());
    snapshot.GetDirectory().SetCurrentSource(GetHomeDirectory());
    snapshot.SetDefaultDefinitions();
    cmMakefile mf(gg, snapshot);
    if (GetWorkingMode() != NORMAL_MODE) {
      mf.SetScriptModeFile(cmSystemTools::ToNormalizedPathOnDisk(path));
      mf.SetArgcArgv(args);
    }
    if (!cmSystemTools::FileExists(path, true)) {
      cmSystemTools::Error("Not a file: " + path);
    }
    if (!mf.ReadListFile(path)) {
      cmSystemTools::Error("Error processing file: " + path);
    }
  }
}

bool CMake::FindPackage(std::vector<std::string> const& args)
{
  FunctionTrace f(__func__);

  SetHomeDirectory(cmSystemTools::GetLogicalWorkingDirectory());
  SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());

  SetGlobalGenerator(cm::make_unique<cmGlobalGenerator>(this));

  cmStateSnapshot snapshot = GetCurrentSnapshot();
  snapshot.GetDirectory().SetCurrentBinary(cmSystemTools::GetLogicalWorkingDirectory());
  snapshot.GetDirectory().SetCurrentSource(cmSystemTools::GetLogicalWorkingDirectory());
  // read in the list file to fill the cache
  snapshot.SetDefaultDefinitions();
  auto mfu = cm::make_unique<cmMakefile>(GetGlobalGenerator(), snapshot);
  cmMakefile* mf = mfu.get();
  m_pGlobalGenerator->AddMakefile(std::move(mfu));

  mf->SetArgcArgv(args);

  std::string systemFile = mf->GetModulesFile("CMakeFindPackageMode.cmake");
  mf->ReadListFile(systemFile);

  std::string language = mf->GetSafeDefinition("LANGUAGE");
  std::string mode = mf->GetSafeDefinition("MODE");
  std::string packageName = mf->GetSafeDefinition("NAME");
  bool packageFound = mf->IsOn("PACKAGE_FOUND");
  bool quiet = mf->IsOn("PACKAGE_QUIET");

  if (!packageFound) {
    if (!quiet) {
      printf("%s not found.\n", packageName.c_str());
    }
  } else if (mode == "EXIST"_s) {
    if (!quiet) {
      printf("%s found.\n", packageName.c_str());
    }
  } else if (mode == "COMPILE"_s) {
    std::string includes = mf->GetSafeDefinition("PACKAGE_INCLUDE_DIRS");
    cmList includeDirs{ includes };

    m_pGlobalGenerator->CreateGenerationObjects();
    auto const& lg = m_pGlobalGenerator->LocalGenerators[0];
    std::string includeFlags = lg->GetIncludeFlags(includeDirs, nullptr, language, std::string());

    std::string definitions = mf->GetSafeDefinition("PACKAGE_DEFINITIONS");
    printf("%s %s\n", includeFlags.c_str(), definitions.c_str());
  } else if (mode == "LINK"_s) {
    char const* targetName = "dummy";
    std::vector<std::string> srcs;
    cmTarget* tgt = mf->AddExecutable(targetName, srcs, true);
    tgt->SetProperty("LINKER_LANGUAGE", language);

    std::string libs = mf->GetSafeDefinition("PACKAGE_LIBRARIES");
    cmList libList{ libs };
    for (std::string const& lib : libList) {
      tgt->AddLinkLibrary(*mf, lib, GENERAL_LibraryType);
    }

    std::string buildType = mf->GetSafeDefinition("CMAKE_BUILD_TYPE");
    buildType = cmSystemTools::UpperCase(buildType);

    std::string linkLibs;
    std::string frameworkPath;
    std::string linkPath;
    std::string flags;
    std::string linkFlags;
    m_pGlobalGenerator->CreateGenerationObjects();
    cmGeneratorTarget* gtgt = m_pGlobalGenerator->FindGeneratorTarget(tgt->GetName());
    cmLocalGenerator* lg = gtgt->GetLocalGenerator();
    cmLinkLineComputer linkLineComputer(lg, lg->GetStateSnapshot().GetDirectory());
    lg->GetTargetFlags(&linkLineComputer, buildType, linkLibs, flags, linkFlags, frameworkPath, linkPath, gtgt);
    linkLibs = frameworkPath + linkPath + linkLibs;

    printf("%s\n", linkLibs.c_str());

    /*    if ( use_win32 )
          {
          tgt->SetProperty("WIN32_EXECUTABLE", "ON");
          }
        if ( use_macbundle)
          {
          tgt->SetProperty("MACOSX_BUNDLE", "ON");
          }*/
  }

  return packageFound;
}

void CMake::LoadEnvironmentPresets()
{
  FunctionTrace f(__func__);

  std::string envGenVar;
  bool hasEnvironmentGenerator = false;
  if (cmSystemTools::GetEnv("CMAKE_GENERATOR", envGenVar)) {
    hasEnvironmentGenerator = true;
    m_environmentGenerator = envGenVar;
  }

  auto readGeneratorVar = [&](std::string const& name, std::string& key) {
    std::string varValue;
    if (cmSystemTools::GetEnv(name, varValue)) {
      if (hasEnvironmentGenerator) {
        key = varValue;
      } else if (!GetIsInTryCompile()) {
        std::string message =
          cmStrCat("Warning: Environment variable ", name, " will be ignored, because CMAKE_GENERATOR is not set.");
        cmSystemTools::Message(message, "Warning");
      }
    }
  };

  readGeneratorVar("CMAKE_GENERATOR_INSTANCE", m_generatorInstance);
  readGeneratorVar("CMAKE_GENERATOR_PLATFORM", m_generatorPlatform);
  readGeneratorVar("CMAKE_GENERATOR_TOOLSET", m_generatorToolset);
}

namespace {
enum class ListPresets
{
  None,
  Configure,
  Build,
  Test,
  Package,
  Workflow,
  All,
};
}

// Parse the args
void CMake::SetArgs(std::vector<std::string> const& args)
{
  m_cmdArgs = args;
  bool haveToolset = false;
  bool havePlatform = false;
  bool haveBArg = false;
  bool haveCMLName = false;
  std::string possibleUnknownArg;
  std::string extraProvidedPath;
#if !defined(CMAKE_BOOTSTRAP)
  std::string profilingFormat;
  std::string profilingOutput;
  std::string presetName;

  ListPresets listPresets = ListPresets::None;
#endif

  auto EmptyStringArgLambda = [](std::string const&, CMake* state) -> bool {
    state->IssueMessage(MessageType::WARNING, "Ignoring empty string (\"\") provided on the command line.");
    return true;
  };

  auto SourceArgLambda = [](std::string const& value, CMake* state) -> bool {
    if (value.empty()) {
      cmSystemTools::Error("No source directory specified for -S");
      return false;
    }
    state->SetHomeDirectoryViaCommandLine(cmSystemTools::ToNormalizedPathOnDisk(value));
    return true;
  };

  auto BuildArgLambda = [&](std::string const& value, CMake* state) -> bool {
    if (value.empty()) {
      cmSystemTools::Error("No build directory specified for -B");
      return false;
    }
    state->SetHomeOutputDirectory(cmSystemTools::ToNormalizedPathOnDisk(value));
    haveBArg = true;
    return true;
  };

  auto PlatformLambda = [&](std::string const& value, CMake* state) -> bool {
    if (havePlatform) {
      cmSystemTools::Error("Multiple -A options not allowed");
      return false;
    }
    state->SetGeneratorPlatform(value);
    havePlatform = true;
    return true;
  };

  auto ToolsetLambda = [&](std::string const& value, CMake* state) -> bool {
    if (haveToolset) {
      cmSystemTools::Error("Multiple -T options not allowed");
      return false;
    }
    state->SetGeneratorToolset(value);
    haveToolset = true;
    return true;
  };

  auto CMakeListsFileLambda = [&](std::string const& value, CMake* state) -> bool {
    if (haveCMLName) {
      cmSystemTools::Error("Multiple --project-file options not allowed");
      return false;
    }
    state->SetCMakeListName(value);
    haveCMLName = true;
    return true;
  };

  std::vector<CommandArgument> arguments = {
    CommandArgument{ "", CommandArgument::Values::Zero, EmptyStringArgLambda },
    CommandArgument{ "-S", "No source directory specified for -S", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, SourceArgLambda },
    CommandArgument{ "-H", "No source directory specified for -H", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, SourceArgLambda },
    CommandArgument{ "-O", CommandArgument::Values::Zero, IgnoreAndTrueLambda },
    CommandArgument{ "-B", "No build directory specified for -B", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, BuildArgLambda },
    CommandArgument{ "--fresh", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* cm) -> bool {
                       cm->m_freshCache = true;
                       return true;
                     } },
    CommandArgument{ "-P", "-P must be followed by a file name.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, IgnoreAndTrueLambda },
    CommandArgument{ "-D", "-D must be followed with VAR=VALUE.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, IgnoreAndTrueLambda },
    CommandArgument{ "-C", "-C must be followed by a file name.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, IgnoreAndTrueLambda },
    CommandArgument{ "-U", "-U must be followed with VAR.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, IgnoreAndTrueLambda },
    CommandArgument{ "-W", "-W must be followed with [no-]<name>.", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, IgnoreAndTrueLambda },
    CommandArgument{ "-A", "No platform specified for -A", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, PlatformLambda },
    CommandArgument{ "-T", "No toolset specified for -T", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No, ToolsetLambda },
    CommandArgument{ "--toolchain", "No file specified for --toolchain", CommandArgument::Values::One,
                     IgnoreAndTrueLambda },
    CommandArgument{ "--install-prefix", "No install directory specified for --install-prefix",
                     CommandArgument::Values::One, IgnoreAndTrueLambda },

    CommandArgument{ "--check-build-system", CommandArgument::Values::Two,
                     [](std::string const& value, CMake* state) -> bool {
                       cmList values{ value };
                       state->m_checkBuildSystemArgument = values[0];
                       state->m_clearBuildSystem = (atoi(values[1].c_str()) > 0);
                       return true;
                     } },
    CommandArgument{ "--check-stamp-file", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       state->m_checkStampFile = value;
                       return true;
                     } },
    CommandArgument{ "--check-stamp-list", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       state->m_checkStampList = value;
                       return true;
                     } },
    CommandArgument{ "--regenerate-during-build", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       state->m_regenerateDuringBuild = true;
                       return true;
                     } },

    CommandArgument{ "--find-package", CommandArgument::Values::Zero, IgnoreAndTrueLambda },

    CommandArgument{ "--graphviz", "No file specified for --graphviz", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       state->SetGraphVizFile(cmSystemTools::ToNormalizedPathOnDisk(value));
                       return true;
                     } },

    CommandArgument{ "--debug-trycompile", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "debug trycompile on\n";
                       state->DebugTryCompileOn();
                       return true;
                     } },
    CommandArgument{ "--debug-output", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Running with debug output on.\n";
                       state->SetDebugOutputOn(true);
                       return true;
                     } },

    CommandArgument{ "--log-level", "Invalid level specified for --log-level", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       auto const logLevel = StringToLogLevel(value);
                       if (logLevel == Message::LogLevel::LOG_UNDEFINED) {
                         cmSystemTools::Error("Invalid level specified for --log-level");
                         return false;
                       }
                       state->SetLogLevel(logLevel);
                       state->m_logLevelWasSetViaCLI = true;
                       return true;
                     } },
    // This is supported for backward compatibility. This option only
    // appeared in the 3.15.x release series and was renamed to
    // --log-level in 3.16.0
    CommandArgument{ "--loglevel", "Invalid level specified for --loglevel", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       auto const logLevel = StringToLogLevel(value);
                       if (logLevel == Message::LogLevel::LOG_UNDEFINED) {
                         cmSystemTools::Error("Invalid level specified for --loglevel");
                         return false;
                       }
                       state->SetLogLevel(logLevel);
                       state->m_logLevelWasSetViaCLI = true;
                       return true;
                     } },

    CommandArgument{ "--log-context", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       state->SetShowLogContext(true);
                       return true;
                     } },
    CommandArgument{ "--project-file", "No filename specified for --project-file", CommandArgument::Values::One,
                     CMakeListsFileLambda },
    CommandArgument{ "--debug-find", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Running with debug output on for the `find` commands.\n";
                       state->SetDebugFindOutput(true);
                       return true;
                     } },
    CommandArgument{ "--debug-find-pkg", "Provide a package argument for --debug-find-pkg",
                     CommandArgument::Values::One, CommandArgument::RequiresSeparator::Yes,
                     [](std::string const& value, CMake* state) -> bool {
                       std::vector<std::string> find_pkgs(cmTokenize(value, ','));
                       std::cout << "Running with debug output on for the 'find' commands "
                                    "for package(s)";
                       for (auto const& v : find_pkgs) {
                         std::cout << ' ' << v;
                         state->SetDebugFindOutputPkgs(v);
                       }
                       std::cout << ".\n";
                       return true;
                     } },
    CommandArgument{ "--debug-find-var", CommandArgument::Values::One, CommandArgument::RequiresSeparator::Yes,
                     [](std::string const& value, CMake* state) -> bool {
                       std::vector<std::string> find_vars(cmTokenize(value, ','));
                       std::cout << "Running with debug output on for the variable(s)";
                       for (auto const& v : find_vars) {
                         std::cout << ' ' << v;
                         state->SetDebugFindOutputVars(v);
                       }
                       std::cout << ".\n";
                       return true;
                     } },
    CommandArgument{ "--trace", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Put cmake in trace mode.\n";
                       state->SetTrace(true);
                       state->SetTraceExpand(false);
                       return true;
                     } },
    CommandArgument{ "--trace-expand", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Put cmake in trace mode, but with "
                                    "variables expanded.\n";
                       state->SetTrace(true);
                       state->SetTraceExpand(true);
                       return true;
                     } },
    CommandArgument{ "--trace-format", "Invalid format specified for --trace-format", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       std::cout << "Put cmake in trace mode and sets the "
                                    "trace output format.\n";
                       state->SetTrace(true);
                       auto const traceFormat = StringToTraceFormat(value);
                       if (traceFormat == TraceFormat::Undefined) {
                         cmSystemTools::Error(
                           "Invalid format specified for --trace-format. "
                           "Valid formats are human, json-v1.");
                         return false;
                       }
                       state->SetTraceFormat(traceFormat);
                       return true;
                     } },
    CommandArgument{ "--trace-source", "No file specified for --trace-source", CommandArgument::Values::OneOrMore,
                     [](std::string const& values, CMake* state) -> bool {
                       std::cout << "Put cmake in trace mode, but output only "
                                    "lines of a specified file. Multiple "
                                    "options are allowed.\n";
                       for (auto file : cmSystemTools::SplitString(values, ';')) {
                         cmSystemTools::ConvertToUnixSlashes(file);
                         state->AddTraceSource(file);
                       }
                       state->SetTrace(true);
                       return true;
                     } },
    CommandArgument{ "--trace-redirect", "No file specified for --trace-redirect", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       std::cout << "Put cmake in trace mode and redirect trace "
                                    "output to a file instead of stderr.\n";
                       std::string file(value);
                       cmSystemTools::ConvertToUnixSlashes(file);
                       state->SetTraceFile(file);
                       state->SetTrace(true);
                       return true;
                     } },
    CommandArgument{ "--warn-uninitialized", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Warn about uninitialized values.\n";
                       state->SetWarnUninitialized(true);
                       return true;
                     } },
    CommandArgument{ "--warn-unused-vars", CommandArgument::Values::Zero, IgnoreAndTrueLambda }, // Option was removed.
    CommandArgument{ "--no-warn-unused-cli", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Not searching for unused variables given on the "
                                    "command line.\n";
                       state->SetWarnUnusedCli(false);
                       return true;
                     } },
    CommandArgument{ "--check-system-vars", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Also check system files when warning about unused and "
                                    "uninitialized variables.\n";
                       state->SetCheckSystemVars(true);
                       return true;
                     } },
    CommandArgument{ "--compile-no-warning-as-error", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Ignoring COMPILE_WARNING_AS_ERROR target property and "
                                    "CMAKE_COMPILE_WARNING_AS_ERROR variable.\n";
                       state->SetIgnoreCompileWarningAsError(true);
                       return true;
                     } },
    CommandArgument{ "--link-no-warning-as-error", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
                       std::cout << "Ignoring LINK_WARNING_AS_ERROR target property and "
                                    "CMAKE_LINK_WARNING_AS_ERROR variable.\n";
                       state->SetIgnoreLinkWarningAsError(true);
                       return true;
                     } },
#ifndef CMAKE_BOOTSTRAP
    CommandArgument{ "--sarif-output", "No file specified for --sarif-output", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
                       state->m_SarifFilePath = cmSystemTools::ToNormalizedPathOnDisk(value);
                       state->m_SarifFileOutput = true;
                       return true;
                     } },
#endif
    CommandArgument{ "--debugger", CommandArgument::Values::Zero,
                     [](std::string const&, CMake* state) -> bool {
#ifdef CMake_ENABLE_DEBUGGER
                       std::cout << "Running with debugger on.\n";
                       state->SetDebuggerOn(true);
                       return true;
#else
                       static_cast<void>(state);
                       cmSystemTools::Error("CMake was not built with support for --debugger");
                       return false;
#endif
                     } },
    CommandArgument{ "--debugger-pipe", "No path specified for --debugger-pipe", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
#ifdef CMake_ENABLE_DEBUGGER
                       state->m_debuggerPipe = value;
                       return true;
#else
                       static_cast<void>(value);
                       static_cast<void>(state);
                       cmSystemTools::Error(
                         "CMake was not built with support "
                         "for --debugger-pipe");
                       return false;
#endif
                     } },
    CommandArgument{ "--debugger-dap-log", "No file specified for --debugger-dap-log", CommandArgument::Values::One,
                     [](std::string const& value, CMake* state) -> bool {
#ifdef CMake_ENABLE_DEBUGGER
                       state->m_debuggerDapLogFile = cmSystemTools::ToNormalizedPathOnDisk(value);
                       return true;
#else
                       static_cast<void>(value);
                       static_cast<void>(state);
                       cmSystemTools::Error(
                         "CMake was not built with support "
                         "for --debugger-dap-log");
                       return false;
#endif
                     } },
  };

#if defined(CMAKE_HAVE_VS_GENERATORS)
  arguments.emplace_back(
    "--vs-solution-file", CommandArgument::Values::One, [](std::string const& value, CMake* state) -> bool {
      state->m_VSSolutionFile = value;
      return true;
    });
#endif

#if !defined(CMAKE_BOOTSTRAP)
  arguments.emplace_back(
    "--profiling-format", "No format specified for --profiling-format", CommandArgument::Values::One,
    [&](std::string const& value, CMake*) -> bool {
      profilingFormat = value;
      return true;
    });
  arguments.emplace_back(
    "--profiling-output", "No path specified for --profiling-output", CommandArgument::Values::One,
    [&profilingOutput](std::string const& value, CMake*) -> bool {
      profilingOutput = cmSystemTools::ToNormalizedPathOnDisk(value);
      return true;
    });
  arguments.emplace_back(
    "--preset", "No preset specified for --preset", CommandArgument::Values::One,
    [&](std::string const& value, CMake*) -> bool {
      presetName = value;
      return true;
    });
  arguments.emplace_back(
    "--list-presets", CommandArgument::Values::ZeroOrOne, [&](std::string const& value, CMake*) -> bool {
      if (value.empty() || value == "configure") {
        listPresets = ListPresets::Configure;
      } else if (value == "build") {
        listPresets = ListPresets::Build;
      } else if (value == "test") {
        listPresets = ListPresets::Test;
      } else if (value == "package") {
        listPresets = ListPresets::Package;
      } else if (value == "workflow") {
        listPresets = ListPresets::Workflow;
      } else if (value == "all") {
        listPresets = ListPresets::All;
      } else {
        cmSystemTools::Error(
          "Invalid value specified for --list-presets.\n"
          "Valid values are configure, build, test, package, or all. "
          "When no value is passed the default is configure.");
        return false;
      }

      return true;
    });

#endif

  bool badGeneratorName = false;
  CommandArgument generatorCommand(
    "-G", "No generator specified for -G", CommandArgument::Values::One, CommandArgument::RequiresSeparator::No,
    [&](std::string const& value, CMake* state) -> bool {
      bool valid = state->CreateAndSetGlobalGenerator(value);
      badGeneratorName = !valid;
      return valid;
    });

  for (decltype(args.size()) i = 1; i < args.size(); ++i) {
    // iterate each argument
    std::string const& arg = args[i];

    if (GetWorkingMode() == SCRIPT_MODE && arg == "--") {
      // Stop processing CMake args and avoid possible errors
      // when arbitrary args are given to CMake script.
      break;
    }

    // Generator flag has special handling for when to print help
    // so it becomes the exception
    if (generatorCommand.matches(arg)) {
      bool parsed = generatorCommand.parse(arg, i, args, this);
      if (!parsed && !badGeneratorName) {
        //        PrintGeneratorList();
        return;
      }
      continue;
    }

    bool matched = false;
    bool parsedCorrectly = true; // needs to be true so we can ignore
                                 // arguments so as -E
    for (auto const& m : arguments) {
      if (m.matches(arg)) {
        matched = true;
        parsedCorrectly = m.parse(arg, i, args, this);
        break;
      }
    }

    // We have an issue where arguments to a "-P" script mode
    // can be provided before the "-P" argument. This means
    // that we need to lazily check this argument after checking
    // all args.
    // Additionally it can't be the source/binary tree location
    if (!parsedCorrectly) {
      cmSystemTools::Error("Run 'cmake --help' for all supported options.");
      exit(1);
    } else if (!matched && cmHasLiteralPrefix(arg, "-")) {
      possibleUnknownArg = arg;
    } else if (!matched) {
      bool parsedDirectory = SetDirectoriesFromFile(arg);
      if (!parsedDirectory) {
        extraProvidedPath = arg;
      }
    }
  }

  if (!extraProvidedPath.empty() && GetWorkingMode() == NORMAL_MODE) {
    IssueMessage(
      MessageType::WARNING, cmStrCat("Ignoring extra path from command line:\n \"", extraProvidedPath, "\""));
  }
  if (!possibleUnknownArg.empty() && GetWorkingMode() != SCRIPT_MODE) {
    cmSystemTools::Error(cmStrCat("Unknown argument ", possibleUnknownArg));
    cmSystemTools::Error("Run 'cmake --help' for all supported options.");
    exit(1);
  }

  // Empty instance, platform and toolset if only a generator is specified
  if (m_pGlobalGenerator) {
    m_generatorInstance = "";
    if (!m_generatorPlatformSet) {
      m_generatorPlatform = "";
    }
    if (!m_generatorToolsetSet) {
      m_generatorToolset = "";
    }
  }

#if !defined(CMAKE_BOOTSTRAP)
  if (!profilingOutput.empty() || !profilingFormat.empty()) {
    if (profilingOutput.empty()) {
      cmSystemTools::Error("--profiling-format specified but no --profiling-output!");
      return;
    }
    if (profilingFormat == "google-trace"_s) {
      try {
        m_profilingOutput = cm::make_unique<cmMakefileProfilingData>(profilingOutput);
      } catch (std::runtime_error& e) {
        cmSystemTools::Error(cmStrCat("Could not start profiling: ", e.what()));
        return;
      }
    } else {
      cmSystemTools::Error("Invalid format specified for --profiling-format");
      return;
    }
  }
#endif

  bool const haveSourceDir = !GetHomeDirectory().empty();
  bool const haveBinaryDir = !GetHomeOutputDirectory().empty();
  bool const havePreset =
#ifdef CMAKE_BOOTSTRAP
    false;
#else
    !presetName.empty();
#endif

  if (m_currentWorkingMode == CMake::NORMAL_MODE && !haveSourceDir && !haveBinaryDir && !havePreset) {
    IssueMessage(
      MessageType::WARNING,
      "No source or binary directory provided. Both will be assumed to be "
      "the same as the current working directory, but note that this "
      "warning will become a fatal error in future CMake releases.");
  }

  if (!haveSourceDir) {
    SetHomeDirectory(cmSystemTools::GetLogicalWorkingDirectory());
  }
  if (!haveBinaryDir) {
    SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());
  }

#if !defined(CMAKE_BOOTSTRAP)
  if (listPresets != ListPresets::None || !presetName.empty()) {
    cmCMakePresetsGraph presetsGraph;
    auto result = presetsGraph.ReadProjectPresets(GetHomeDirectory());
    if (result != true) {
      std::string errorMsg =
        cmStrCat("Could not read presets from ", GetHomeDirectory(), ":\n", presetsGraph.parseState.GetErrorMessage());
      cmSystemTools::Error(errorMsg);
      return;
    }

    if (listPresets != ListPresets::None) {
      if (listPresets == ListPresets::Configure) {
        PrintPresetList(presetsGraph);
      } else if (listPresets == ListPresets::Build) {
        presetsGraph.PrintBuildPresetList();
      } else if (listPresets == ListPresets::Test) {
        presetsGraph.PrintTestPresetList();
      } else if (listPresets == ListPresets::Package) {
        presetsGraph.PrintPackagePresetList();
      } else if (listPresets == ListPresets::Workflow) {
        presetsGraph.PrintWorkflowPresetList();
      } else if (listPresets == ListPresets::All) {
        presetsGraph.PrintAllPresets();
      }

      SetWorkingMode(WorkingMode::HELP_MODE, CMake::CommandFailureAction::FATAL_ERROR);
      return;
    }

    auto preset = presetsGraph.ConfigurePresets.find(presetName);
    if (preset == presetsGraph.ConfigurePresets.end()) {
      cmSystemTools::Error(cmStrCat("No such preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
      PrintPresetList(presetsGraph);
      return;
    }
    if (preset->second.Unexpanded.Hidden) {
      cmSystemTools::Error(cmStrCat("Cannot use hidden preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
      PrintPresetList(presetsGraph);
      return;
    }
    auto const& expandedPreset = preset->second.Expanded;
    if (!expandedPreset) {
      cmSystemTools::Error(
        cmStrCat("Could not evaluate preset \"", preset->second.Unexpanded.m_name, "\": Invalid macro expansion"));
      return;
    }
    if (!expandedPreset->ConditionResult) {
      cmSystemTools::Error(cmStrCat("Could not use disabled preset \"", preset->second.Unexpanded.m_name, "\""));
      return;
    }

    if (!m_pState->IsCacheLoaded() && !haveBArg && !expandedPreset->BinaryDir.empty()) {
      SetHomeOutputDirectory(expandedPreset->BinaryDir);
    }
    if (!m_pGlobalGenerator && !expandedPreset->Generator.empty()) {
      if (!CreateAndSetGlobalGenerator(expandedPreset->Generator)) {
        return;
      }
    }
    m_unprocessedPresetVariables = expandedPreset->CacheVariables;
    m_unprocessedPresetEnvironment = expandedPreset->Environment;

    if (!expandedPreset->InstallDir.empty() && !m_pState->GetInitializedCacheValue("CMAKE_INSTALL_PREFIX")) {
      m_unprocessedPresetVariables["CMAKE_INSTALL_PREFIX"] = { "PATH", expandedPreset->InstallDir };
    }
    if (!expandedPreset->ToolchainFile.empty() && !m_pState->GetInitializedCacheValue("CMAKE_TOOLCHAIN_FILE")) {
      m_unprocessedPresetVariables["CMAKE_TOOLCHAIN_FILE"] = { "FILEPATH", expandedPreset->ToolchainFile };
    }

    if (
      !expandedPreset->ArchitectureStrategy ||
      expandedPreset->ArchitectureStrategy == cmCMakePresetsGraph::ArchToolsetStrategy::m_set) {
      if (!m_generatorPlatformSet && !expandedPreset->Architecture.empty()) {
        SetGeneratorPlatform(expandedPreset->Architecture);
      }
    }
    if (
      !expandedPreset->ToolsetStrategy ||
      expandedPreset->ToolsetStrategy == cmCMakePresetsGraph::ArchToolsetStrategy::m_set) {
      if (!m_generatorToolsetSet && !expandedPreset->Toolset.empty()) {
        SetGeneratorToolset(expandedPreset->Toolset);
      }
    }

    if (!expandedPreset->m_graphVizFile.empty()) {
      if (m_graphVizFile.empty()) {
        SetGraphVizFile(cmSystemTools::CollapseFullPath(expandedPreset->m_graphVizFile));
      }
    }

    SetWarningFromPreset("dev", expandedPreset->WarnDev, expandedPreset->ErrorDev);
    SetWarningFromPreset("deprecated", expandedPreset->WarnDeprecated, expandedPreset->ErrorDeprecated);
    if (expandedPreset->m_warnUninitialized == true) {
      SetWarnUninitialized(true);
    }
    if (expandedPreset->m_warnUnusedCli == false) {
      SetWarnUnusedCli(false);
    }
    if (expandedPreset->WarnSystemVars == true) {
      SetCheckSystemVars(true);
    }
    if (expandedPreset->m_debugOutput == true) {
      SetDebugOutputOn(true);
    }
    if (expandedPreset->m_debugTryCompile == true) {
      DebugTryCompileOn();
    }
    if (expandedPreset->DebugFind == true) {
      SetDebugFindOutput(true);
    }
    if (expandedPreset->TraceMode && expandedPreset->TraceMode != cmCMakePresetsGraph::TraceEnableMode::Disable) {
      SetTrace(true);
      if (expandedPreset->TraceMode == cmCMakePresetsGraph::TraceEnableMode::Expand) {
        SetTraceExpand(true);
      }
    }
    if (expandedPreset->TraceFormat) {
      SetTrace(true);
      SetTraceFormat(*expandedPreset->TraceFormat);
    }
    if (!expandedPreset->TraceSource.empty()) {
      SetTrace(true);
      for (std::string const& filePaths : expandedPreset->TraceSource) {
        AddTraceSource(filePaths);
      }
    }
    if (!expandedPreset->m_traceRedirect.empty()) {
      SetTrace(true);
      SetTraceFile(expandedPreset->m_traceRedirect);
    }
  }
#endif
}

namespace {
using LevelsPair = std::pair<cm::string_view, Message::LogLevel>;
using LevelsPairArray = std::array<LevelsPair, 7>;
LevelsPairArray const& getStringToLogLevelPairs()
{
  static LevelsPairArray const levels = { { { "error", Message::LogLevel::LOG_ERROR },
                                            { "warning", Message::LogLevel::LOG_WARNING },
                                            { "notice", Message::LogLevel::LOG_NOTICE },
                                            { "status", Message::LogLevel::LOG_STATUS },
                                            { "verbose", Message::LogLevel::LOG_VERBOSE },
                                            { "debug", Message::LogLevel::LOG_DEBUG },
                                            { "trace", Message::LogLevel::LOG_TRACE } } };
  return levels;
}
} // namespace

Message::LogLevel CMake::StringToLogLevel(cm::string_view levelStr)
{
  LevelsPairArray const& levels = getStringToLogLevelPairs();

  auto const levelStrLowCase = cmSystemTools::LowerCase(std::string{ levelStr });

  // NOLINTNEXTLINE(readability-qualified-auto)
  auto const it = std::find_if(
    levels.cbegin(), levels.cend(), [&levelStrLowCase](LevelsPair const& p) { return p.first == levelStrLowCase; });
  return (it != levels.cend()) ? it->second : Message::LogLevel::LOG_UNDEFINED;
}

std::string CMake::LogLevelToString(Message::LogLevel level)
{
  LevelsPairArray const& levels = getStringToLogLevelPairs();

  // NOLINTNEXTLINE(readability-qualified-auto)
  auto const it =
    std::find_if(levels.cbegin(), levels.cend(), [&level](LevelsPair const& p) { return p.second == level; });
  cm::string_view const levelStrLowerCase = (it != levels.cend()) ? it->first : "undefined";
  std::string levelStrUpperCase = cmSystemTools::UpperCase(std::string{ levelStrLowerCase });
  return levelStrUpperCase;
}

CMake::TraceFormat CMake::StringToTraceFormat(std::string const& traceStr)
{
  using TracePair = std::pair<std::string, TraceFormat>;
  static std::vector<TracePair> const levels = {
    { "human", TraceFormat::Human },
    { "json-v1", TraceFormat::JSONv1 },
  };

  auto const traceStrLowCase = cmSystemTools::LowerCase(traceStr);

  auto const it = std::find_if(
    levels.cbegin(), levels.cend(), [&traceStrLowCase](TracePair const& p) { return p.first == traceStrLowCase; });
  return (it != levels.cend()) ? it->second : TraceFormat::Undefined;
}

void CMake::SetTraceFile(std::string const& file)
{
  m_traceFile.close();
  m_traceFile.open(file.c_str());
  if (!m_traceFile) {
    cmSystemTools::Error(cmStrCat("Error opening trace file ", file, ": ", cmSystemTools::GetLastSystemError()));
    return;
  }
  std::cout << "Trace will be written to " << file << '\n';
}

void CMake::PrintTraceFormatVersion()
{
  if (!GetTrace()) {
    return;
  }

  std::string msg;

  switch (GetTraceFormat()) {
    case TraceFormat::JSONv1: {
#ifndef CMAKE_BOOTSTRAP
      Json::Value val;
      Json::Value version;
      Json::StreamWriterBuilder builder;
      builder["indentation"] = "";
      version["major"] = 1;
      version["minor"] = 2;
      val["version"] = version;
      msg = Json::writeString(builder, val);
#endif
      break;
    }
    case TraceFormat::Human:
      msg = "";
      break;
    case TraceFormat::Undefined:
      msg = "INTERNAL ERROR: Trace format is Undefined";
      break;
  }

  if (msg.empty()) {
    return;
  }

  auto& f = GetTraceFile();
  if (f) {
    f << msg << '\n';
  } else {
    cmSystemTools::Message(msg);
  }
}

void CMake::SetTraceRedirect(CMake* other)
{
  m_trace = other->m_trace;
  m_traceExpand = other->m_traceExpand;
  m_traceFormatVar = other->m_traceFormatVar;
  m_traceOnlyThisSources = other->m_traceOnlyThisSources;

  m_traceRedirect = other;
}

bool CMake::SetDirectoriesFromFile(std::string const& arg)
{
  // Check if the argument refers to a CMakeCache.txt or CMakeLists.txt file.
  // Do not check for the custom project filename CMAKE_LIST_FILE_NAME, as it
  // cannot be determined until after reading the CMakeCache.txt
  std::string listPath;
  std::string cachePath;
  bool is_source_dir = false;
  bool is_empty_directory = false;
  if (cmSystemTools::FileIsDirectory(arg)) {
    std::string path = cmSystemTools::ToNormalizedPathOnDisk(arg);
    std::string cacheFile = cmStrCat(path, "/CMakeCache.txt");
    std::string listFile = GetCMakeListFile(path);

    is_empty_directory = true;
    if (cmSystemTools::FileExists(cacheFile)) {
      cachePath = path;
      is_empty_directory = false;
    }
    if (cmSystemTools::FileExists(listFile)) {
      listPath = path;
      is_empty_directory = false;
      is_source_dir = true;
    }
  } else if (cmSystemTools::FileExists(arg)) {
    std::string fullPath = cmSystemTools::ToNormalizedPathOnDisk(arg);
    std::string name = cmSystemTools::GetFilenameName(fullPath);
    name = cmSystemTools::LowerCase(name);
    if (name == "cmakecache.txt"_s) {
      cachePath = cmSystemTools::GetFilenamePath(fullPath);
    } else if (name == "cmakelists.txt"_s) {
      listPath = cmSystemTools::GetFilenamePath(fullPath);
    }
  } else {
    // Specified file or directory does not exist.  Try to set things
    // up to produce a meaningful error message.
    std::string fullPath = cmSystemTools::CollapseFullPath(arg);
    std::string name = cmSystemTools::GetFilenameName(fullPath);
    name = cmSystemTools::LowerCase(name);
    if (name == "cmakecache.txt"_s || name == "cmakelists.txt"_s) {
      listPath = cmSystemTools::GetFilenamePath(fullPath);
    } else {
      listPath = fullPath;
    }
  }

  // If there is a CMakeCache.txt file, use its settings.
  if (!cachePath.empty()) {
    if (LoadCache(cachePath)) {
      cmValue existingValue = m_pState->GetCacheEntryValue("CMAKE_HOME_DIRECTORY");
      if (existingValue) {
        SetHomeOutputDirectory(cachePath);
        SetHomeDirectory(*existingValue);
        return true;
      }
    }
  }

  bool no_source_tree = GetHomeDirectory().empty();
  bool no_build_tree = GetHomeOutputDirectory().empty();

  // When invoked with a path that points to an existing CMakeCache
  // This function is called multiple times with the same path
  bool const passed_same_path = (listPath == GetHomeDirectory()) || (listPath == GetHomeOutputDirectory());
  bool used_provided_path = (passed_same_path || is_source_dir || no_build_tree);

  // If there is a CMakeLists.txt file, use it as the source tree.
  if (!listPath.empty()) {
    // When invoked with a path that points to an existing CMakeCache
    // This function is called multiple times with the same path
    if (is_source_dir) {
      SetHomeDirectoryViaCommandLine(listPath);
      if (no_build_tree) {
        SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());
      }
    } else if (no_source_tree && no_build_tree) {
      SetHomeDirectory(listPath);
      SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());
    } else if (no_build_tree) {
      SetHomeOutputDirectory(listPath);
    }
  } else {
    if (no_source_tree) {
      // We didn't find a CMakeLists.txt and it wasn't specified
      // with -S. Assume it is the path to the source tree
      SetHomeDirectory(cmSystemTools::ToNormalizedPathOnDisk(arg));
    }
    if (no_build_tree && !no_source_tree && is_empty_directory) {
      // passed `-S <path> <build_dir> when build_dir is an empty directory
      SetHomeOutputDirectory(cmSystemTools::ToNormalizedPathOnDisk(arg));
    } else if (no_build_tree) {
      // We didn't find a CMakeCache.txt and it wasn't specified
      // with -B. Assume the current working directory as the build tree.
      SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());
      used_provided_path = false;
    }
  }

  return used_provided_path;
}

// at the end of this CMAKE_ROOT and CMAKE_COMMAND should be added to the
// cache
int CMake::AddCMakePaths()
{
  FunctionTrace f(__func__);

  // Save the value in the cache
  AddCacheEntry("CMAKE_COMMAND", cmSystemTools::GetCMakeCommand(), "Path to CMake executable.", cmStateEnums::INTERNAL);
#ifndef CMAKE_BOOTSTRAP
  AddCacheEntry(
    "CMAKE_CTEST_COMMAND", cmSystemTools::GetCTestCommand(), "Path to ctest program executable.",
    cmStateEnums::INTERNAL);
  AddCacheEntry(
    "CMAKE_CPACK_COMMAND", cmSystemTools::GetCPackCommand(), "Path to cpack program executable.",
    cmStateEnums::INTERNAL);
#endif
  if (!cmSystemTools::FileExists((cmSystemTools::GetCMakeRoot() + "/Modules/CMake.cmake"))) {
    // couldn't find modules
    cmSystemTools::Error(
      "Could not find CMAKE_ROOT !!!\n"
      "CMake has most likely not been installed correctly.\n"
      "Modules directory not found in\n" +
      cmSystemTools::GetCMakeRoot());
    return 0;
  }
  AddCacheEntry("CMAKE_ROOT", cmSystemTools::GetCMakeRoot(), "Path to CMake installation.", cmStateEnums::INTERNAL);

  return 1;
}

void CMake::AddDefaultExtraGenerators()
{
#if !defined(CMAKE_BOOTSTRAP)
  m_extraGenerators.push_back(cmExtraCodeBlocksGenerator::GetFactory());
  m_extraGenerators.push_back(cmExtraCodeLiteGenerator::GetFactory());
  m_extraGenerators.push_back(cmExtraEclipseCDT4Generator::GetFactory());
  m_extraGenerators.push_back(cmExtraKateGenerator::GetFactory());
  m_extraGenerators.push_back(cmExtraSublimeTextGenerator::GetFactory());
#endif
}

void CMake::GetRegisteredGenerators(std::vector<GeneratorInfo>& generators) const
{
  for (auto const& gen : m_generators) {
    std::vector<std::string> names = gen->GetGeneratorNames();

    for (std::string const& name : names) {
      GeneratorInfo info;
      info.supportsToolset = gen->SupportsToolset();
      info.supportsPlatform = gen->SupportsPlatform();
      info.supportedPlatforms = gen->GetKnownPlatforms();
      info.defaultPlatform = gen->GetDefaultPlatformName();
      info.name = name;
      info.baseName = name;
      info.isAlias = false;
      generators.push_back(std::move(info));
    }
  }

  for (cmExternalMakefileProjectGeneratorFactory* eg : m_extraGenerators) {
    std::vector<std::string> const genList = eg->GetSupportedGlobalGenerators();
    for (std::string const& gen : genList) {
      GeneratorInfo info;
      info.name = cmExternalMakefileProjectGenerator::CreateFullGeneratorName(gen, eg->GetName());
      info.baseName = gen;
      info.extraName = eg->GetName();
      info.supportsPlatform = false;
      info.supportsToolset = false;
      info.isAlias = false;
      generators.push_back(std::move(info));
    }
    for (std::string const& a : eg->Aliases) {
      GeneratorInfo info;
      info.name = a;
      if (!genList.empty()) {
        info.baseName = genList.at(0);
      }
      info.extraName = eg->GetName();
      info.supportsPlatform = false;
      info.supportsToolset = false;
      info.isAlias = true;
      generators.push_back(std::move(info));
    }
  }
}

static std::pair<
  std::unique_ptr<cmExternalMakefileProjectGenerator>,
  std::string>
createExtraGenerator(
  std::vector<cmExternalMakefileProjectGeneratorFactory*> const& in,
  std::string const& name)
{
  for (cmExternalMakefileProjectGeneratorFactory* i : in) {
    std::vector<std::string> const generators = i->GetSupportedGlobalGenerators();
    if (i->GetName() == name) { // Match aliases
      return { i->CreateExternalMakefileProjectGenerator(), generators.at(0) };
    }
    for (std::string const& g : generators) {
      std::string const fullName = cmExternalMakefileProjectGenerator::CreateFullGeneratorName(g, i->GetName());
      if (fullName == name) {
        return { i->CreateExternalMakefileProjectGenerator(), g };
      }
    }
  }
  return { nullptr, name };
}

std::unique_ptr<cmGlobalGenerator> CMake::CreateGlobalGenerator(std::string const& gname)
{
  std::pair<std::unique_ptr<cmExternalMakefileProjectGenerator>, std::string> extra =
    createExtraGenerator(m_extraGenerators, gname);
  std::unique_ptr<cmExternalMakefileProjectGenerator>& extraGenerator = extra.first;
  std::string const& name = extra.second;

  std::unique_ptr<cmGlobalGenerator> generator;
  for (auto const& g : m_generators) {
    generator = g->CreateGlobalGenerator(name, this);
    if (generator) {
      break;
    }
  }

  if (generator) {
    generator->SetExternalMakefileProjectGenerator(std::move(extraGenerator));
  }

  return generator;
}

bool CMake::CreateAndSetGlobalGenerator(std::string const& name)
{
  auto gen = CreateGlobalGenerator(name);
  if (!gen) {
    std::string kdevError;
    std::string vsError;
    if (name.find("KDevelop3", 0) != std::string::npos) {
      kdevError = "\nThe KDevelop3 generator is not supported anymore.";
    }
    if (cmHasLiteralPrefix(name, "Visual Studio ") && name.length() >= cmStrLen("Visual Studio xx xxxx ")) {
      vsError = "\nUsing platforms in Visual Studio generator names is not "
                "supported in CMakePresets.json.";
    }

    cmSystemTools::Error(cmStrCat("Could not create named generator ", name, kdevError, vsError));
    //    PrintGeneratorList();
    return false;
  }

  SetGlobalGenerator(std::move(gen));
  return true;
}

#ifndef CMAKE_BOOTSTRAP
void CMake::PrintPresetList(cmCMakePresetsGraph const& graph) const
{
  std::vector<GeneratorInfo> generators;
  GetRegisteredGenerators(generators);
  auto filter = [&generators](cmCMakePresetsGraph::ConfigurePreset const& preset) -> bool {
    if (preset.Generator.empty()) {
      return true;
    }
    auto condition = [&preset](GeneratorInfo const& info) -> bool { return info.name == preset.Generator; };
    auto it = std::find_if(generators.begin(), generators.end(), condition);
    return it != generators.end();
  };

  graph.PrintConfigurePresetList(filter);
}
#endif

void CMake::SetHomeDirectoryViaCommandLine(std::string const& path)
{
  if (path.empty()) {
    return;
  }

  auto prev_path = GetHomeDirectory();
  if (prev_path != path && !prev_path.empty() && GetWorkingMode() == NORMAL_MODE) {
    IssueMessage(MessageType::WARNING, cmStrCat("Ignoring extra path from command line:\n \"", prev_path, "\""));
  }
  SetHomeDirectory(path);
}

void CMake::SetHomeDirectory(std::string const& dir)
{
  m_pState->SetSourceDirectory(dir);
  if (m_currentSnapshot.IsValid()) {
    m_currentSnapshot.SetDefinition("CMAKE_SOURCE_DIR", dir);
  }

  if (m_pState->GetProjectKind() == cmState::ProjectKind::Normal) {
    m_pMessenger->SetTopSource(GetHomeDirectory());
  } else {
    m_pMessenger->SetTopSource(cm::nullopt);
  }
}

std::string const& CMake::GetHomeDirectory() const
{
  return m_pState->GetSourceDirectory();
}

void CMake::SetHomeOutputDirectory(std::string const& dir)
{
  m_pState->SetBinaryDirectory(dir);
  if (m_currentSnapshot.IsValid()) {
    m_currentSnapshot.SetDefinition("CMAKE_BINARY_DIR", dir);
  }
}

std::string const& CMake::GetHomeOutputDirectory() const
{
  return m_pState->GetBinaryDirectory();
}

std::string CMake::FindCacheFile(std::string const& binaryDir)
{
  std::string cachePath = binaryDir;
  cmSystemTools::ConvertToUnixSlashes(cachePath);
  std::string cacheFile = cmStrCat(cachePath, "/CMakeCache.txt");
  if (!cmSystemTools::FileExists(cacheFile)) {
    // search in parent directories for cache
    std::string cmakeFiles = cmStrCat(cachePath, "/CMakeFiles");
    if (cmSystemTools::FileExists(cmakeFiles)) {
      std::string cachePathFound = cmSystemTools::FileExistsInParentDirectories("CMakeCache.txt", cachePath, "/");
      if (!cachePathFound.empty()) {
        cachePath = cmSystemTools::GetFilenamePath(cachePathFound);
      }
    }
  }
  return cachePath;
}

void CMake::SetGlobalGenerator(std::unique_ptr<cmGlobalGenerator> gg)
{
  if (!gg) {
    cmSystemTools::Error("Error SetGlobalGenerator called with null");
    return;
  }
  if (m_pGlobalGenerator) {
    // restore the original environment variables CXX and CC
    std::string env = "CC=";
    if (!m_CCEnvironment.empty()) {
      env += m_CCEnvironment;
      cmSystemTools::PutEnv(env);
    } else {
      cmSystemTools::UnPutEnv(env);
    }
    env = "CXX=";
    if (!m_CXXEnvironment.empty()) {
      env += m_CXXEnvironment;
      cmSystemTools::PutEnv(env);
    } else {
      cmSystemTools::UnPutEnv(env);
    }
  }

  // set the new
  m_pGlobalGenerator = std::move(gg);

  // set the global flag for unix style paths on cmSystemTools as soon as
  // the generator is set.  This allows gmake to be used on windows.
  cmSystemTools::SetForceUnixPaths(m_pGlobalGenerator->GetForceUnixPaths());

  // Save the environment variables CXX and CC
  if (!cmSystemTools::GetEnv("CXX", m_CXXEnvironment)) {
    m_CXXEnvironment.clear();
  }
  if (!cmSystemTools::GetEnv("CC", m_CCEnvironment)) {
    m_CCEnvironment.clear();
  }
}

int CMake::DoPreConfigureChecks()
{
  // Make sure the Source directory contains a CMakeLists.txt file.
  std::string srcList = cmStrCat(GetHomeDirectory(), "/", m_cmakeListName);
  if (!cmSystemTools::FileExists(srcList)) {
    std::ostringstream err;
    if (cmSystemTools::FileIsDirectory(GetHomeDirectory())) {
      err << "The source directory \"" << GetHomeDirectory() << "\" does not appear to contain " << m_cmakeListName
          << ".\n";
    } else if (cmSystemTools::FileExists(GetHomeDirectory())) {
      err << "The source directory \"" << GetHomeDirectory() << "\" is a file, not a directory.\n";
    } else {
      err << "The source directory \"" << GetHomeDirectory() << "\" does not exist.\n";
    }
    err << "Specify --help for usage, or press the help button on the CMake "
           "GUI.";
    cmSystemTools::Error(err.str());
    return -2;
  }

  // do a sanity check on some values
  if (m_pState->GetInitializedCacheValue("CMAKE_HOME_DIRECTORY")) {
    std::string cacheStart =
      cmStrCat(*m_pState->GetInitializedCacheValue("CMAKE_HOME_DIRECTORY"), "/", m_cmakeListName);
    if (!cmSystemTools::SameFile(cacheStart, srcList)) {
      std::string message = cmStrCat(
        "The source \"", srcList, "\" does not match the source \"", cacheStart,
        "\" used to generate cache.  Re-run cmake with a different "
        "source directory.");
      cmSystemTools::Error(message);
      return -2;
    }
  } else {
    return 0;
  }
  return 1;
}
struct SaveCacheEntry
{
  std::string key;
  std::string value;
  std::string help;
  cmStateEnums::CacheEntryType type;
};

int CMake::HandleDeleteCacheVariables(std::string const& var)
{
  cmList argsSplit{ var, cmList::EmptyElements::Yes };
  // erase the property to avoid infinite recursion
  m_pState->SetGlobalProperty("__CMAKE_DELETE_CACHE_CHANGE_VARS_", "");
  if (GetIsInTryCompile()) {
    return 0;
  }
  std::vector<SaveCacheEntry> saved;
  std::ostringstream warning;
  warning << "You have changed variables that require your cache to be deleted.\n"
             "Configure will be re-run and you may have to reset some variables.\n"
             "The following variables have changed:\n";
  for (auto i = argsSplit.begin(); i != argsSplit.end(); ++i) {
    SaveCacheEntry save;
    save.key = *i;
    warning << *i << "= ";
    i++;
    if (i != argsSplit.end()) {
      save.value = *i;
      warning << *i << '\n';
    } else {
      warning << '\n';
      i -= 1;
    }
    cmValue existingValue = m_pState->GetCacheEntryValue(save.key);
    if (existingValue) {
      save.type = m_pState->GetCacheEntryType(save.key);
      if (cmValue help = m_pState->GetCacheEntryProperty(save.key, "HELPSTRING")) {
        save.help = *help;
      }
    } else {
      save.type = cmStateEnums::CacheEntryType::UNINITIALIZED;
    }
    saved.push_back(std::move(save));
  }

  // remove the cache
  DeleteCache(GetHomeOutputDirectory());
  // load the empty cache
  LoadCache();
  // restore the changed compilers
  for (SaveCacheEntry const& i : saved) {
    AddCacheEntry(i.key, i.value, i.help, i.type);
  }
  cmSystemTools::Message(warning.str());
  // avoid reconfigure if there were errors
  if (!cmSystemTools::GetErrorOccurredFlag()) {
    // re-run configure
    return Configure();
  }
  return 0;
}

int CMake::Configure()
{
  FunctionTrace f(__func__);

#if !defined(CMAKE_BOOTSTRAP)
  auto profilingRAII = CreateProfilingEntry("project", "configure");
#endif

  DiagLevel diagLevel;

  if (m_diagLevels.count("deprecated") == 1) {

    diagLevel = m_diagLevels["deprecated"];
    if (diagLevel == DIAG_IGNORE) {
      SetSuppressDeprecatedWarnings(true);
      SetDeprecatedWarningsAsErrors(false);
    } else if (diagLevel == DIAG_WARN) {
      SetSuppressDeprecatedWarnings(false);
      SetDeprecatedWarningsAsErrors(false);
    } else if (diagLevel == DIAG_ERROR) {
      SetSuppressDeprecatedWarnings(false);
      SetDeprecatedWarningsAsErrors(true);
    }
  }

  if (m_diagLevels.count("dev") == 1) {
    bool setDeprecatedVariables = false;

    cmValue cachedWarnDeprecated = m_pState->GetCacheEntryValue("CMAKE_WARN_DEPRECATED");
    cmValue cachedErrorDeprecated = m_pState->GetCacheEntryValue("CMAKE_ERROR_DEPRECATED");

    // don't overwrite deprecated warning setting from a previous invocation
    if (!cachedWarnDeprecated && !cachedErrorDeprecated) {
      setDeprecatedVariables = true;
    }

    diagLevel = m_diagLevels["dev"];
    if (diagLevel == DIAG_IGNORE) {
      SetSuppressDevWarnings(true);
      SetDevWarningsAsErrors(false);

      if (setDeprecatedVariables) {
        SetSuppressDeprecatedWarnings(true);
        SetDeprecatedWarningsAsErrors(false);
      }
    } else if (diagLevel == DIAG_WARN) {
      SetSuppressDevWarnings(false);
      SetDevWarningsAsErrors(false);

      if (setDeprecatedVariables) {
        SetSuppressDeprecatedWarnings(false);
        SetDeprecatedWarningsAsErrors(false);
      }
    } else if (diagLevel == DIAG_ERROR) {
      SetSuppressDevWarnings(false);
      SetDevWarningsAsErrors(true);

      if (setDeprecatedVariables) {
        SetSuppressDeprecatedWarnings(false);
        SetDeprecatedWarningsAsErrors(true);
      }
    }
  }

  // Cache variables may have already been set by a previous invocation,
  // so we cannot rely on command line options alone. Always ensure our
  // messenger is in sync with the cache.
  cmValue value = m_pState->GetCacheEntryValue("CMAKE_WARN_DEPRECATED");
  m_pMessenger->SetSuppressDeprecatedWarnings(value && value.IsOff());

  value = m_pState->GetCacheEntryValue("CMAKE_ERROR_DEPRECATED");
  m_pMessenger->SetDeprecatedWarningsAsErrors(value.IsOn());

  value = m_pState->GetCacheEntryValue("CMAKE_SUPPRESS_DEVELOPER_WARNINGS");
  m_pMessenger->SetSuppressDevWarnings(value.IsOn());

  value = m_pState->GetCacheEntryValue("CMAKE_SUPPRESS_DEVELOPER_ERRORS");
  m_pMessenger->SetDevWarningsAsErrors(value && value.IsOff());

  int ret = ActualConfigure();
  cmValue delCacheVars = m_pState->GetGlobalProperty("__CMAKE_DELETE_CACHE_CHANGE_VARS_");
  if (delCacheVars && !delCacheVars->empty()) {
    return HandleDeleteCacheVariables(*delCacheVars);
  }
  return ret;
}

int CMake::ActualConfigure()
{
  // Construct right now our path conversion table before it's too late:
  CleanupCommandsAndMacros();

  cmSystemTools::RemoveADirectory(GetHomeOutputDirectory() + "/CMakeFiles/CMakeScratch");

  std::string cmlNameCache = m_pState->GetInitializedCacheValue("CMAKE_LIST_FILE_NAME");
  if (!cmlNameCache.empty() && !m_cmakeListName.empty() && cmlNameCache != m_cmakeListName) {
    std::string message = cmStrCat(
      "CMakeLists filename : \"", m_cmakeListName, "\"\nDoes not match the previous: \"", cmlNameCache,
      "\"\nEither remove the CMakeCache.txt file and CMakeFiles "
      "directory or choose a different binary directory.");
    cmSystemTools::Error(message);
    return -2;
  }
  if (m_cmakeListName.empty()) {
    m_cmakeListName = cmlNameCache.empty() ? "CMakeLists.txt" : cmlNameCache;
  }
  if (m_cmakeListName != "CMakeLists.txt") {
    IssueMessage(
      MessageType::WARNING,
      "This project has been configured with a project file other than "
      "CMakeLists.txt. This feature is intended for temporary use during "
      "development and not for publication of a final product.");
  }
  AddCacheEntry("CMAKE_LIST_FILE_NAME", m_cmakeListName, "Name of CMakeLists files to read", cmStateEnums::INTERNAL);

  int res = DoPreConfigureChecks();
  if (res < 0) {
    return -2;
  }
  if (!res) {
    AddCacheEntry(
      "CMAKE_HOME_DIRECTORY", GetHomeDirectory(),
      "Source directory with the top level CMakeLists.txt file for this "
      "project",
      cmStateEnums::INTERNAL);
  }

  // We want to create the package redirects directory as early as possible,
  // but not before pre-configure checks have passed. This ensures we get
  // errors about inappropriate source/binary directories first.
  auto const redirectsDir = cmStrCat(GetHomeOutputDirectory(), "/CMakeFiles/pkgRedirects");
  cmSystemTools::RemoveADirectory(redirectsDir);
  cmSystemTools::MakeDirectory(redirectsDir);
  // if (!cmSystemTools::MakeDirectory(redirectsDir)) {
  //  cmSystemTools::Error(cmStrCat(
  //    "Unable to (re)create the private pkgRedirects directory:\n  ", redirectsDir,
  //    "\n"
  //    "This may be caused by not having read/write access to "
  //    "the build directory.\n"
  //    "Try specifying a location with read/write access like:\n"
  //    "  cmake -B build\n"
  //    "If using a CMake presets file, ensure that preset parameter\n"
  //    "'binaryDir' expands to a writable directory.\n"));
  //  return -1;
  //}
  AddCacheEntry("CMAKE_FIND_PACKAGE_REDIRECTS_DIR", redirectsDir, "Value Computed by CMake.", cmStateEnums::STATIC);

  // no generator specified on the command line
  if (!m_pGlobalGenerator) {
    cmValue genName = m_pState->GetInitializedCacheValue("CMAKE_GENERATOR");
    cmValue extraGenName = m_pState->GetInitializedCacheValue("CMAKE_EXTRA_GENERATOR");
    if (genName) {
      std::string fullName =
        cmExternalMakefileProjectGenerator::CreateFullGeneratorName(*genName, extraGenName ? *extraGenName : "");
      m_pGlobalGenerator = CreateGlobalGenerator(fullName);
    }
    if (m_pGlobalGenerator) {
      // set the global flag for unix style paths on cmSystemTools as
      // soon as the generator is set.  This allows gmake to be used
      // on windows.
      cmSystemTools::SetForceUnixPaths(m_pGlobalGenerator->GetForceUnixPaths());
    } else {
      CreateDefaultGlobalGenerator();
    }
    if (!m_pGlobalGenerator) {
      cmSystemTools::Error("Could not create generator");
      return -1;
    }
  }

  cmValue genName = m_pState->GetInitializedCacheValue("CMAKE_GENERATOR");
  if (genName) {
    if (!m_pGlobalGenerator->MatchesGeneratorName(*genName)) {
      std::string message = cmStrCat(
        "Error: generator : ", m_pGlobalGenerator->GetName(), '\n',
        "Does not match the generator used previously: ", *genName, '\n',
        "Either remove the CMakeCache.txt file and CMakeFiles "
        "directory or choose a different binary directory.");
      cmSystemTools::Error(message);
      return -2;
    }
  }
  if (!genName) {
    AddCacheEntry("CMAKE_GENERATOR", m_pGlobalGenerator->GetName(), "Name of generator.", cmStateEnums::INTERNAL);
    AddCacheEntry(
      "CMAKE_EXTRA_GENERATOR", m_pGlobalGenerator->GetExtraGeneratorName(),
      "Name of external makefile project generator.", cmStateEnums::INTERNAL);

    if (!m_pState->GetInitializedCacheValue("CMAKE_TOOLCHAIN_FILE")) {
      std::string envToolchain;
      if (cmSystemTools::GetEnv("CMAKE_TOOLCHAIN_FILE", envToolchain) && !envToolchain.empty()) {
        AddCacheEntry("CMAKE_TOOLCHAIN_FILE", envToolchain, "The CMake toolchain file", cmStateEnums::FILEPATH);
      }
    }
  }

  if (cmValue instance = m_pState->GetInitializedCacheValue("CMAKE_GENERATOR_INSTANCE")) {
    if (m_generatorInstanceSet && m_generatorInstance != *instance) {
      std::string message = cmStrCat(
        "Error: generator instance: ", m_generatorInstance, '\n',
        "Does not match the instance used previously: ", *instance, '\n',
        "Either remove the CMakeCache.txt file and CMakeFiles "
        "directory or choose a different binary directory.");
      cmSystemTools::Error(message);
      return -2;
    }
  } else {
    AddCacheEntry(
      "CMAKE_GENERATOR_INSTANCE", m_generatorInstance, "Generator instance identifier.", cmStateEnums::INTERNAL);
  }

  if (cmValue platformName = m_pState->GetInitializedCacheValue("CMAKE_GENERATOR_PLATFORM")) {
    if (m_generatorPlatformSet && m_generatorPlatform != *platformName) {
      std::string message = cmStrCat(
        "Error: generator platform: ", m_generatorPlatform, '\n',
        "Does not match the platform used previously: ", *platformName, '\n',
        "Either remove the CMakeCache.txt file and CMakeFiles "
        "directory or choose a different binary directory.");
      cmSystemTools::Error(message);
      return -2;
    }
  } else {
    AddCacheEntry(
      "CMAKE_GENERATOR_PLATFORM", m_generatorPlatform, "Name of generator platform.", cmStateEnums::INTERNAL);
  }

  if (cmValue tsName = m_pState->GetInitializedCacheValue("CMAKE_GENERATOR_TOOLSET")) {
    if (m_generatorToolsetSet && m_generatorToolset != *tsName) {
      std::string message = cmStrCat(
        "Error: generator toolset: ", m_generatorToolset, '\n', "Does not match the toolset used previously: ", *tsName,
        '\n',
        "Either remove the CMakeCache.txt file and CMakeFiles "
        "directory or choose a different binary directory.");
      cmSystemTools::Error(message);
      return -2;
    }
  } else {
    AddCacheEntry("CMAKE_GENERATOR_TOOLSET", m_generatorToolset, "Name of generator toolset.", cmStateEnums::INTERNAL);
  }

  if (!m_pState->GetInitializedCacheValue("CMAKE_TEST_LAUNCHER")) {
    cm::optional<std::string> testLauncher = cmSystemTools::GetEnvVar("CMAKE_TEST_LAUNCHER");
    if (testLauncher && !testLauncher->empty()) {
      std::string message = "Test launcher to run tests executable.";
      AddCacheEntry("CMAKE_TEST_LAUNCHER", *testLauncher, message, cmStateEnums::STRING);
    }
  }

  if (!m_pState->GetInitializedCacheValue("CMAKE_CROSSCOMPILING_EMULATOR")) {
    cm::optional<std::string> emulator = cmSystemTools::GetEnvVar("CMAKE_CROSSCOMPILING_EMULATOR");
    if (emulator && !emulator->empty()) {
      std::string message = "Emulator to run executables and tests when cross compiling.";
      AddCacheEntry("CMAKE_CROSSCOMPILING_EMULATOR", *emulator, message, cmStateEnums::STRING);
    }
  }

  // reset any system configuration information, except for when we are
  // InTryCompile. With TryCompile the system info is taken from the parent's
  // info to save time
  if (!GetIsInTryCompile()) {
    m_pGlobalGenerator->ClearEnabledLanguages();
  }

#if !defined(CMAKE_BOOTSTRAP)
  m_pFileAPI = cm::make_unique<cmFileAPI>(this);
  m_pFileAPI->ReadQueries();

  if (!GetIsInTryCompile()) {
    TruncateOutputLog("CMakeConfigureLog.yaml");
    m_configureLog = cm::make_unique<cmConfigureLog>(
      cmStrCat(GetHomeOutputDirectory(), "/CMakeFiles"_s), m_pFileAPI->GetConfigureLogVersions());
  }

  // m_pInstrumentation = cm::make_unique<cmInstrumentation>(m_pState->GetBinaryDirectory());
  // m_pInstrumentation->ClearGeneratedQueries();
#endif

  // actually do the configure
  auto startTime = std::chrono::steady_clock::now();
#if !defined(CMAKE_BOOTSTRAP)
  // if (m_pInstrumentation->HasErrors()) {
  //   return 1;
  // }
  auto doConfigure = [this]() -> int {
    m_pGlobalGenerator->Configure();
    return 0;
  };
  int ret = doConfigure();
  // int ret = m_pInstrumentation->InstrumentCommand(
  //   "configure", m_cmdArgs, [doConfigure]() { return doConfigure(); }, cm::nullopt, cm::nullopt, true);
  if (ret != 0) {
    return ret;
  }
#else
  this->GlobalGenerator->Configure();
#endif
  auto endTime = std::chrono::steady_clock::now();

  // configure result
  if (GetWorkingMode() == CMake::NORMAL_MODE) {
    std::ostringstream msg;
    if (cmSystemTools::GetErrorOccurredFlag()) {
      msg << "Configuring incomplete, errors occurred!";
    } else {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
      msg << "Configuring done (" << std::fixed << std::setprecision(1) << ms.count() / 1000.0L << "s)";
    }
    UpdateProgress(msg.str(), -1);
  }

#if !defined(CMAKE_BOOTSTRAP)
  m_configureLog.reset();
#endif

  // Before saving the cache
  // if the project did not define one of the entries below, add them now
  // so users can edit the values in the cache:

  auto const& mf = m_pGlobalGenerator->GetMakefiles()[0];

  if (mf->IsOn("CTEST_USE_LAUNCHERS") && !m_pState->GetGlobalProperty("RULE_LAUNCH_COMPILE")) {
    IssueMessage(
      MessageType::FATAL_ERROR,
      "CTEST_USE_LAUNCHERS is enabled, but the "
      "RULE_LAUNCH_COMPILE global property is not defined.\n"
      "Did you forget to include(CTest) in the toplevel "
      "CMakeLists.txt ?");
  }
  // Setup launchers for instrumentation
#if !defined(CMAKE_BOOTSTRAP)
//  m_pInstrumentation->LoadQueries();
// if (m_pInstrumentation->HasQuery()) {
//  std::string launcher;
//  if (mf->IsOn("CTEST_USE_LAUNCHERS")) {
//    launcher = cmStrCat(
//      "\"", cmSystemTools::GetCTestCommand(), "\" --launch ", "--current-build-dir <CMAKE_CURRENT_BINARY_DIR> ");
//  } else {
//    launcher = cmStrCat("\"", cmSystemTools::GetCTestCommand(), "\" --instrument ");
//  }
//  std::string common_args =
//    cmStrCat(" --target-name <TARGET_NAME> --build-dir \"", m_pState->GetBinaryDirectory(), "\" ");
//  m_pState->SetGlobalProperty(
//    "RULE_LAUNCH_COMPILE",
//    cmStrCat(
//      launcher, "--command-type compile", common_args, "--config <CONFIG> ",
//      "--output <OBJECT> --source <SOURCE> --language <LANGUAGE> -- "));
//  m_pState->SetGlobalProperty(
//    "RULE_LAUNCH_LINK",
//    cmStrCat(
//      launcher, "--command-type link", common_args,
//      "--output <TARGET> --target-type <TARGET_TYPE> --config <CONFIG> ",
//      "--language <LANGUAGE> --target-labels \"<TARGET_LABELS>\" -- "));
//  m_pState->SetGlobalProperty(
//    "RULE_LAUNCH_CUSTOM",
//    cmStrCat(launcher, "--command-type custom", common_args, "--output \"<OUTPUT>\" --role <ROLE> -- "));
//}
#endif

  m_pState->SaveVerificationScript(GetHomeOutputDirectory(), m_pMessenger.get());
  SaveCache(GetHomeOutputDirectory());
  if (cmSystemTools::GetErrorOccurredFlag()) {
#if !defined(CMAKE_BOOTSTRAP)
    m_pFileAPI->WriteReplies(cmFileAPI::IndexFor::FailedConfigure);
#endif
    return -1;
  }
  return 0;
}

std::unique_ptr<cmGlobalGenerator> CMake::EvaluateDefaultGlobalGenerator()
{
  if (!m_environmentGenerator.empty()) {
    auto gen = CreateGlobalGenerator(m_environmentGenerator);
    if (!gen) {
      cmSystemTools::Error(
        "CMAKE_GENERATOR was set but the specified "
        "generator doesn't exist. Using CMake default.");
    } else {
      return gen;
    }
  }
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(CMAKE_BOOT_MINGW)
  std::string found;
  // Try to find the newest VS installed on the computer and
  // use that as a default if -G is not specified
  std::string const vsregBase = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\";
  static char const* const vsVariants[] = { /* clang-format needs this comment to break after the opening brace */
                                            "VisualStudio\\", "VCExpress\\", "WDExpress\\"
  };
  struct VSVersionedGenerator
  {
    char const* MSVersion;
    char const* GeneratorName;
  };
  static VSVersionedGenerator const vsGenerators[] = {
    { "14.0", "Visual Studio 14 2015" }, //
  };
  static char const* const vsEntries[] = {
    "\\Setup\\VC;ProductDir", //
    ";InstallDir"             //
  };
  if (cmVSSetupAPIHelper(17).IsVSInstalled()) {
    found = "Visual Studio 17 2022";
  } else if (cmVSSetupAPIHelper(16).IsVSInstalled()) {
    found = "Visual Studio 16 2019";
  } else if (cmVSSetupAPIHelper(15).IsVSInstalled()) {
    found = "Visual Studio 15 2017";
  } else {
    for (VSVersionedGenerator const* g = cm::cbegin(vsGenerators); found.empty() && g != cm::cend(vsGenerators); ++g) {
      for (char const* const* v = cm::cbegin(vsVariants); found.empty() && v != cm::cend(vsVariants); ++v) {
        for (char const* const* e = cm::cbegin(vsEntries); found.empty() && e != cm::cend(vsEntries); ++e) {
          std::string const reg = vsregBase + *v + g->MSVersion + *e;
          std::string dir;
          if (
            cmSystemTools::ReadRegistryValue(reg, dir, cmSystemTools::KeyWOW64_32) && cmSystemTools::PathExists(dir)) {
            found = g->GeneratorName;
          }
        }
      }
    }
  }
  auto gen = CreateGlobalGenerator(found);
  if (!gen) {
    gen = cm::make_unique<cmGlobalNMakeMakefileGenerator>(this);
  }
  return std::unique_ptr<cmGlobalGenerator>(std::move(gen));
#elif defined(CMAKE_BOOTSTRAP_NINJA)
  return std::unique_ptr<cmGlobalGenerator>(cm::make_unique<cmGlobalNinjaGenerator>(this));
#else
  return std::unique_ptr<cmGlobalGenerator>(cm::make_unique<cmGlobalUnixMakefileGenerator3>(this));
#endif
}

void CMake::CreateDefaultGlobalGenerator()
{
  auto gen = EvaluateDefaultGlobalGenerator();
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(CMAKE_BOOT_MINGW)
  // This print could be unified for all platforms
  std::cout << "-- Building for: " << gen->GetName() << '\n';
#endif
  SetGlobalGenerator(std::move(gen));
}

void CMake::PreLoadCMakeFiles()
{
  FunctionTrace f(__func__);

  std::vector<std::string> args;
  std::string pre_load = GetHomeDirectory();
  if (!pre_load.empty()) {
    pre_load += "/PreLoad.cmake";
    if (cmSystemTools::FileExists(pre_load)) {
      ReadListFile(args, pre_load);
    }
  }
  pre_load = GetHomeOutputDirectory();
  if (!pre_load.empty()) {
    pre_load += "/PreLoad.cmake";
    if (cmSystemTools::FileExists(pre_load)) {
      ReadListFile(args, pre_load);
    }
  }
}

#ifdef CMake_ENABLE_DEBUGGER

bool CMake::StartDebuggerIfEnabled()
{
  if (!GetDebuggerOn()) {
    return true;
  }

  if (!m_debugAdapter) {
    if (GetDebuggerPipe().empty()) {
      std::cerr << "Error: --debugger-pipe must be set when debugging is enabled.\n";
      return false;
    }

    try {
      m_debugAdapter = std::make_shared<cmDebugger::cmDebuggerAdapter>(
        std::make_shared<cmDebugger::cmDebuggerPipeConnection>(GetDebuggerPipe()), GetDebuggerDapLogFile());
    } catch (std::runtime_error const& error) {
      std::cerr << "Error: Failed to create debugger adapter.\n";
      std::cerr << error.what() << "\n";
      return false;
    }
    m_pMessenger->SetDebuggerAdapter(m_debugAdapter);
  }

  return true;
}

void CMake::StopDebuggerIfNeeded(int exitCode)
{
  if (!GetDebuggerOn()) {
    return;
  }

  // The debug adapter may have failed to start (e.g. invalid pipe path).
  if (m_debugAdapter) {
    m_debugAdapter->ReportExitCode(exitCode);
    m_debugAdapter.reset();
  }
}

#endif

// handle a command line invocation
int CMake::Run(
  std::vector<std::string> const& args,
  bool noconfigure)
{
  FunctionTrace f(__func__);

  // Process the arguments
  SetArgs(args);
  if (cmSystemTools::GetErrorOccurredFlag()) {
    return -1;
  }
  if (GetWorkingMode() == HELP_MODE) {
    return 0;
  }

#ifndef CMAKE_BOOTSTRAP
  // Configure the SARIF log for the current run
  cmSarif::LogFileWriter sarifLogFileWriter(GetMessenger()->GetSarifResultsLog());
  if (!sarifLogFileWriter.ConfigureForCMakeRun(*this)) {
    return -1;
  }
#endif

  // Log the trace format version to the desired output
  if (GetTrace()) {
    PrintTraceFormatVersion();
  }

  // If we are given a stamp list file check if it is really out of date.
  if (!m_checkStampList.empty() && isGenerateStampListUpToDate(m_checkStampList)) {
    return 0;
  }

  // If we are given a stamp file check if it is really out of date.
  if (!m_checkStampFile.empty() && isStampFileUpToDate(m_checkStampFile)) {
    return 0;
  }

  if (GetWorkingMode() == NORMAL_MODE) {
    if (m_freshCache) {
      DeleteCache(this->GetHomeOutputDirectory());
    }
    // load the cache
    if (LoadCache() < 0) {
      cmSystemTools::Error("Error executing cmake::LoadCache(). Aborting.\n");
      return -1;
    }
#ifndef CMAKE_BOOTSTRAP
    // If no SARIF file has been explicitly specified, use the default path
    if (!m_SarifFileOutput) {
      // If no output file is specified, use the default path
      // Enable parent directory creation for the default path
      sarifLogFileWriter.SetPath(
        cm::filesystem::path(GetHomeOutputDirectory()) / std::string(cmSarif::PROJECT_DEFAULT_SARIF_FILE), true);
    }
#endif
  } else {
    if (m_freshCache) {
      cmSystemTools::Error("--fresh allowed only when configuring a project");
      return -1;
    }

    AddCMakePaths();
  }

#ifndef CMAKE_BOOTSTRAP
  ProcessPresetVariables();
  ProcessPresetEnvironment();
#endif
  // Add any cache args
  if (!SetCacheArgs(args)) {
    cmSystemTools::Error("Run 'cmake --help' for all supported options.");
    return -1;
  }
#ifndef CMAKE_BOOTSTRAP
  if (
    GetLogLevel() == Message::LogLevel::LOG_VERBOSE || GetLogLevel() == Message::LogLevel::LOG_DEBUG ||
    GetLogLevel() == Message::LogLevel::LOG_TRACE) {
    PrintPresetVariables();
    PrintPresetEnvironment();
  }
#endif
  PrintPresetVariables();
  PrintPresetEnvironment();

  // In script mode we terminate after running the script.
  if (GetWorkingMode() != NORMAL_MODE) {
    if (cmSystemTools::GetErrorOccurredFlag()) {
      return -1;
    }
    return HasScriptModeExitCode() ? GetScriptModeExitCode() : 0;
  }

#ifndef CMAKE_BOOTSTRAP
  // CMake only responds to the SARIF variable in normal mode
  MarkCliAsUsed(cmSarif::PROJECT_SARIF_FILE_VARIABLE);
#endif

  // If MAKEFLAGS are given in the environment, remove the environment
  // variable.  This will prevent try-compile from succeeding when it
  // should fail (if "-i" is an option).  We cannot simply test
  // whether "-i" is given and remove it because some make programs
  // encode the MAKEFLAGS variable in a strange way.
  if (cmSystemTools::HasEnv("MAKEFLAGS")) {
    cmSystemTools::PutEnv("MAKEFLAGS=");
  }

  PreLoadCMakeFiles();

  if (noconfigure) {
    return 0;
  }

  // now run the global generate
  // Check the state of the build system to see if we need to regenerate.
  if (!CheckBuildSystem()) {
    return 0;
  }

#ifdef CMake_ENABLE_DEBUGGER
  if (!StartDebuggerIfEnabled()) {
    return -1;
  }
#endif

  int ret = Configure();
  if (ret) {
#if defined(CMAKE_HAVE_VS_GENERATORS)
    if (!m_VSSolutionFile.empty() && m_pGlobalGenerator) {
      // CMake is running to regenerate a Visual Studio build tree
      // during a build from the VS IDE.  The build files cannot be
      // regenerated, so we should stop the build.
      cmSystemTools::Message(
        "CMake Configure step failed.  "
        "Build files cannot be regenerated correctly.  "
        "Attempting to stop IDE build.");
      cmGlobalVisualStudioGenerator& gg = cm::static_reference_cast<cmGlobalVisualStudioGenerator>(m_pGlobalGenerator);
      gg.CallVisualStudioMacro(cmGlobalVisualStudioGenerator::MacroStop, m_VSSolutionFile);
    }
#endif
    return ret;
  }
  ret = Generate();
  if (ret) {
    cmSystemTools::Message(
      "CMake Generate step failed.  "
      "Build files cannot be regenerated correctly.");
    return ret;
  }
  std::string message = cmStrCat("Build files have been written to: ", GetHomeOutputDirectory());
  UpdateProgress(message, -1);
  return ret;
}

int CMake::Generate()
{
  FunctionTrace f(__func__);

  if (!m_pGlobalGenerator) {
    //    TODO:   so is this fatal? Should it throw?
    return -1;
  }

  auto startTime = std::chrono::steady_clock::now();
#if !defined(CMAKE_BOOTSTRAP)
  auto profilingRAII = CreateProfilingEntry("project", "generate");

  //    TODO: more f'ing lambdas.
  auto doGenerate = [this]() -> int {
    if (!m_pGlobalGenerator->Compute()) {
      m_pFileAPI->WriteReplies(cmFileAPI::IndexFor::FailedCompute);
      return -1;
    }
    m_pGlobalGenerator->Generate();
    return 0;
  };

  //  m_pInstrumentation->LoadQueries();
  int ret = doGenerate();
  //  int ret = m_pInstrumentation->InstrumentCommand("generate", m_cmdArgs, [doGenerate]() { return doGenerate(); });
  if (ret != 0) {
    return ret;
  }
#else
  if (!this->GlobalGenerator->Compute()) {
    return -1;
  }
  this->GlobalGenerator->Generate();
#endif
  auto endTime = std::chrono::steady_clock::now();
  {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    std::ostringstream msg;
    msg << "Generating done (" << std::fixed << std::setprecision(1) << ms.count() / 1000.0L << "s)";
    UpdateProgress(msg.str(), -1);
  }
#if !defined(CMAKE_BOOTSTRAP)
//  m_pInstrumentation->CollectTimingData(cmInstrumentationQuery::Hook::PostGenerate);
#endif
  if (!m_graphVizFile.empty()) {
    std::cout << "Generate graphviz: " << m_graphVizFile << '\n';
    GenerateGraphViz(m_graphVizFile);
  }
  if (m_warnUnusedCli) {
    RunCheckForUnusedVariables();
  }
  if (cmSystemTools::GetErrorOccurredFlag()) {
#if !defined(CMAKE_BOOTSTRAP)
    m_pFileAPI->WriteReplies(cmFileAPI::IndexFor::FailedGenerate);
#endif
    return -1;
  }
  // Save the cache again after a successful Generate so that any internal
  // variables created during Generate are saved. (Specifically target GUIDs
  // for the Visual Studio and Xcode generators.)
  SaveCache(GetHomeOutputDirectory());

#if !defined(CMAKE_BOOTSTRAP)
  m_pGlobalGenerator->WriteInstallJson();
  m_pFileAPI->WriteReplies(cmFileAPI::IndexFor::Success);
#endif

  return 0;
}

void CMake::AddCacheEntry(
  std::string const& key,
  cmValue value,
  cmValue helpString,
  int type)
{
  m_pState->AddCacheEntry(key, value, helpString, static_cast<cmStateEnums::CacheEntryType>(type));
  UnwatchUnusedCli(key);

  if (key == "CMAKE_WARN_DEPRECATED"_s) {
    m_pMessenger->SetSuppressDeprecatedWarnings(value && value.IsOff());
  } else if (key == "CMAKE_ERROR_DEPRECATED"_s) {
    m_pMessenger->SetDeprecatedWarningsAsErrors(value.IsOn());
  } else if (key == "CMAKE_SUPPRESS_DEVELOPER_WARNINGS"_s) {
    m_pMessenger->SetSuppressDevWarnings(value.IsOn());
  } else if (key == "CMAKE_SUPPRESS_DEVELOPER_ERRORS"_s) {
    m_pMessenger->SetDevWarningsAsErrors(value && value.IsOff());
  }
}

bool CMake::DoWriteGlobVerifyTarget() const
{
  return m_pState->DoWriteGlobVerifyTarget();
}

std::string const& CMake::GetGlobVerifyScript() const
{
  return m_pState->GetGlobVerifyScript();
}

std::string const& CMake::GetGlobVerifyStamp() const
{
  return m_pState->GetGlobVerifyStamp();
}

void CMake::AddGlobCacheEntry(
  cmGlobCacheEntry const& entry,
  std::string const& variable,
  cmListFileBacktrace const& backtrace)
{
  m_pState->AddGlobCacheEntry(entry, variable, backtrace, m_pMessenger.get());
}

std::vector<cmGlobCacheEntry> CMake::GetGlobCacheEntries() const
{
  return m_pState->GetGlobCacheEntries();
}

std::vector<std::string> CMake::GetAllExtensions() const
{
  std::vector<std::string> allExt = m_CLikeSourceFileExtensions.ordered;
  allExt.insert(allExt.end(), m_headerFileExtensions.ordered.begin(), m_headerFileExtensions.ordered.end());
  // cuda extensions are also in SourceFileExtensions so we ignore it here
  allExt.insert(allExt.end(), m_FortranFileExtensions.ordered.begin(), m_FortranFileExtensions.ordered.end());
  allExt.insert(allExt.end(), m_HipFileExtensions.ordered.begin(), m_HipFileExtensions.ordered.end());
  allExt.insert(allExt.end(), m_ISPCFileExtensions.ordered.begin(), m_ISPCFileExtensions.ordered.end());
  return allExt;
}

std::string CMake::StripExtension(std::string const& file) const
{
  auto dotpos = file.rfind('.');
  if (dotpos != std::string::npos) {
#if defined(_WIN32) || defined(__APPLE__)
    auto ext = cmSystemTools::LowerCase(file.substr(dotpos + 1));
#else
    auto ext = cm::string_view(file).substr(dotpos + 1);
#endif
    if (IsAKnownExtension(ext)) {
      return file.substr(0, dotpos);
    }
  }
  return file;
}

cmValue CMake::GetCacheDefinition(std::string const& name) const
{
  return m_pState->GetInitializedCacheValue(name);
}

void CMake::AddScriptingCommands() const
{
  GetScriptingCommands(GetState());
}

void CMake::AddProjectCommands() const
{
  GetProjectCommands(GetState());
}

void CMake::AddDefaultGenerators()
{
#if defined(_WIN32) && !defined(__CYGWIN__)
#  if !defined(CMAKE_BOOT_MINGW)
  m_generators.push_back(cmGlobalVisualStudioVersionedGenerator::NewFactory17());
  m_generators.push_back(cmGlobalVisualStudioVersionedGenerator::NewFactory16());
  m_generators.push_back(cmGlobalVisualStudioVersionedGenerator::NewFactory15());
  m_generators.push_back(cmGlobalVisualStudio14Generator::NewFactory());
  m_generators.push_back(cmGlobalBorlandMakefileGenerator::NewFactory());
  m_generators.push_back(cmGlobalNMakeMakefileGenerator::NewFactory());
  m_generators.push_back(cmGlobalJOMMakefileGenerator::NewFactory());
#  endif
  m_generators.push_back(cmGlobalMSYSMakefileGenerator::NewFactory());
  m_generators.push_back(cmGlobalMinGWMakefileGenerator::NewFactory());
#endif
#if !defined(CMAKE_BOOTSTRAP)
#  if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
  m_generators.push_back(cmGlobalGhsMultiGenerator::NewFactory());
#  endif
  m_generators.push_back(cmGlobalUnixMakefileGenerator3::NewFactory());
  m_generators.push_back(cmGlobalNinjaGenerator::NewFactory());
  m_generators.push_back(cmGlobalNinjaMultiGenerator::NewFactory());
#elif defined(CMAKE_BOOTSTRAP_NINJA)
  this->Generators.push_back(cmGlobalNinjaGenerator::NewFactory());
#elif defined(CMAKE_BOOTSTRAP_MAKEFILES)
  this->Generators.push_back(cmGlobalUnixMakefileGenerator3::NewFactory());
#endif
#if defined(CMAKE_USE_WMAKE)
  m_generators.push_back(cmGlobalWatcomWMakeGenerator::NewFactory());
#endif
#ifdef CMAKE_USE_XCODE
  this->Generators.push_back(cmGlobalXCodeGenerator::NewFactory());
#endif
}

bool CMake::ParseCacheEntry(
  std::string const& entry,
  std::string& var,
  std::string& value,
  cmStateEnums::CacheEntryType& type)
{
  return cmState::ParseCacheEntry(entry, var, value, type);
}

int CMake::LoadCache()
{
  // could we not read the cache
  //  TODO: cache filename is hardcoded here and in the lower level function.
  if (!LoadCache(GetHomeOutputDirectory())) {
    // if it does exist, but isn't readable then warn the user
    std::string cacheFile = cmStrCat(GetHomeOutputDirectory(), "/CMakeCache.txt");
    if (cmSystemTools::FileExists(cacheFile)) {
      cmSystemTools::Error(
        "There is a CMakeCache.txt file for the current binary tree but "
        "cmake does not have permission to read it. Please check the "
        "permissions of the directory you are trying to run CMake on.");
      return -1;
    }
  }

  // setup CMAKE_ROOT and CMAKE_COMMAND
  if (!AddCMakePaths()) {
    return -3;
  }
  return 0;
}

bool CMake::LoadCache(std::string const& path)
{
  std::set<std::string> emptySet;
  return LoadCache(path, true, emptySet, emptySet);
}

bool CMake::LoadCache(
  std::string const& path,
  bool internal,
  std::set<std::string>& excludes,
  std::set<std::string>& includes)
{
  bool result = m_pState->LoadCache(path, internal, excludes, includes);
  static auto const entries = { "CMAKE_CACHE_MAJOR_VERSION", "CMAKE_CACHE_MINOR_VERSION" };
  for (auto const& entry : entries) {
    UnwatchUnusedCli(entry);
  }
  return result;
}

bool CMake::SaveCache(std::string const& path)
{
  bool result = m_pState->SaveCache(path, GetMessenger());
  static auto const entries = { "CMAKE_CACHE_MAJOR_VERSION", "CMAKE_CACHE_MINOR_VERSION", "CMAKE_CACHE_PATCH_VERSION",
                                "CMAKE_CACHEFILE_DIR" };
  for (auto const& entry : entries) {
    UnwatchUnusedCli(entry);
  }
  return result;
}

bool CMake::DeleteCache(std::string const& path)
{
  return m_pState->DeleteCache(path);
}

void CMake::SetProgressCallback(ProgressCallbackType f)
{
  m_progressCallback = std::move(f);
}

void CMake::UpdateProgress(
  std::string const& msg,
  float prog)
{
  if (m_progressCallback && !GetIsInTryCompile()) {
    m_progressCallback(msg, prog);
  }
}

bool CMake::GetIsInTryCompile() const
{
  return m_pState->GetProjectKind() == cmState::ProjectKind::TryCompile;
}

// void CMake::AppendGlobalGeneratorsDocumentation(std::vector<cmDocumentationEntry>& v)
//{
//   auto const defaultGenerator = EvaluateDefaultGlobalGenerator();
//   auto const defaultName = defaultGenerator->GetName();
//   auto foundDefaultOne = false;
//
//   for (auto const& g : m_generators) {
//     v.emplace_back(g->GetDocumentation());
//     if (!foundDefaultOne && cmHasPrefix(v.back().m_name, defaultName)) {
//       v.back().m_customNamePrefix = '*';
//       foundDefaultOne = true;
//     }
//   }
// }

// void CMake::AppendExtraGeneratorsDocumentation(std::vector<cmDocumentationEntry>& v)
//{
//   for (cmExternalMakefileProjectGeneratorFactory* eg : m_extraGenerators) {
//     std::string const doc = eg->GetDocumentation();
//     std::string const name = eg->GetName();
//
//     // Aliases:
//     for (std::string const& a : eg->Aliases) {
//       v.emplace_back(cmDocumentationEntry{ a, doc });
//     }
//
//     // Full names:
//     for (std::string const& g : eg->GetSupportedGlobalGenerators()) {
//       v.emplace_back(cmDocumentationEntry{ cmExternalMakefileProjectGenerator::CreateFullGeneratorName(g, name), doc
//       });
//     }
//   }
// }

// std::vector<cmDocumentationEntry> CMake::GetGeneratorsDocumentation()
//{
//   std::vector<cmDocumentationEntry> v;
//   AppendGlobalGeneratorsDocumentation(v);
//   AppendExtraGeneratorsDocumentation(v);
//   return v;
// }

// void CMake::PrintGeneratorList()
//{
// #ifndef CMAKE_BOOTSTRAP
//   cmDocumentation doc;
//   auto generators = GetGeneratorsDocumentation();
//   doc.AppendSection("Generators", generators);
//   std::cerr << '\n';
//   doc.PrintDocumentation(cmDocumentation::ListGenerators, std::cerr);
// #endif
// }

int CMake::CheckBuildSystem()
{
  FunctionTrace f(__func__);

  //    TODO: what the hell? We are running cmake inside of cmake?

  // We do not need to rerun CMake.  Check dependency integrity.
  bool const verbose = isCMakeVerbose();

  // This method will check the integrity of the build system if the
  // option was given on the command line.  It reads the given file to
  // determine whether CMake should rerun.

  // If no file is provided for the check, we have to rerun.
  if (m_checkBuildSystemArgument.empty()) {
    if (verbose) {
      cmSystemTools::Stdout("Re-run cmake no build system arguments\n");
    }
    return 1;
  }

  // If the file provided does not exist, we have to rerun.
  if (!cmSystemTools::FileExists(m_checkBuildSystemArgument)) {
    if (verbose) {
      std::ostringstream msg;
      msg << "Re-run cmake missing file: " << m_checkBuildSystemArgument << '\n';
      cmSystemTools::Stdout(msg.str());
    }
    return 1;
  }

  // Read the rerun check file and use it to decide whether to do the
  // global generate.
  // Actually, all we need is the `set` command.
  CMake cm(RoleScript, cmState::Unknown);
  cm.SetHomeDirectory("");
  cm.SetHomeOutputDirectory("");
  cm.GetCurrentSnapshot().SetDefaultDefinitions();
  cmGlobalGenerator gg(&cm);
  cmMakefile mf(&gg, cm.GetCurrentSnapshot());
  if (!mf.ReadListFile(m_checkBuildSystemArgument) || cmSystemTools::GetErrorOccurredFlag()) {
    if (verbose) {
      std::ostringstream msg;
      msg << "Re-run cmake error reading : " << m_checkBuildSystemArgument << '\n';
      cmSystemTools::Stdout(msg.str());
    }
    // There was an error reading the file.  Just rerun.
    return 1;
  }

  if (m_clearBuildSystem) {
    // Get the generator used for this build system.
    std::string genName = mf.GetSafeDefinition("CMAKE_DEPENDS_GENERATOR");
    if (!cmNonempty(genName)) {
      genName = "Unix Makefiles";
    }

    // Create the generator and use it to clear the dependencies.
    std::unique_ptr<cmGlobalGenerator> ggd = CreateGlobalGenerator(genName);
    if (ggd) {
      cm.GetCurrentSnapshot().SetDefaultDefinitions();
      cmMakefile mfd(ggd.get(), cm.GetCurrentSnapshot());
      auto lgd = ggd->CreateLocalGenerator(&mfd);
      lgd->ClearDependencies(&mfd, verbose);
    }
  }

  // If any byproduct of makefile generation is missing we must re-run.
  cmList products{ mf.GetDefinition("CMAKE_MAKEFILE_PRODUCTS") };
  for (auto const& p : products) {
    if (!cmSystemTools::PathExists(p)) {
      if (verbose) {
        cmSystemTools::Stdout(cmStrCat("Re-run cmake, missing byproduct: ", p, '\n'));
      }
      return 1;
    }
  }

  // Get the set of dependencies and outputs.
  cmList depends{ mf.GetDefinition("CMAKE_MAKEFILE_DEPENDS") };
  cmList outputs;
  if (!depends.empty()) {
    outputs.assign(mf.GetDefinition("CMAKE_MAKEFILE_OUTPUTS"));
  }
  if (depends.empty() || outputs.empty()) {
    // Not enough information was provided to do the test.  Just rerun.
    if (verbose) {
      cmSystemTools::Stdout(
        "Re-run cmake no CMAKE_MAKEFILE_DEPENDS "
        "or CMAKE_MAKEFILE_OUTPUTS :\n");
    }
    return 1;
  }

  // Find the newest dependency.
  auto dep = depends.begin();
  std::string dep_newest = *dep++;
  for (; dep != depends.end(); ++dep) {
    int result = 0;
    if (m_fileTimeCache->Compare(dep_newest, *dep, &result)) {
      if (result < 0) {
        dep_newest = *dep;
      }
    } else {
      if (verbose) {
        cmSystemTools::Stdout("Re-run cmake: build system dependency is missing\n");
      }
      return 1;
    }
  }

  // Find the oldest output.
  auto out = outputs.begin();
  std::string out_oldest = *out++;
  for (; out != outputs.end(); ++out) {
    int result = 0;
    if (m_fileTimeCache->Compare(out_oldest, *out, &result)) {
      if (result > 0) {
        out_oldest = *out;
      }
    } else {
      if (verbose) {
        cmSystemTools::Stdout("Re-run cmake: build system output is missing\n");
      }
      return 1;
    }
  }

  // If any output is older than any dependency then rerun.
  {
    int result = 0;
    if (!m_fileTimeCache->Compare(out_oldest, dep_newest, &result) || result < 0) {
      if (verbose) {
        std::ostringstream msg;
        msg << "Re-run cmake file: " << out_oldest << " older than: " << dep_newest << '\n';
        cmSystemTools::Stdout(msg.str());
      }
      return 1;
    }
  }

  // No need to rerun.
  return 0;
}

void CMake::TruncateOutputLog(char const* fname)
{
  std::string fullPath = cmStrCat(GetHomeOutputDirectory(), '/', fname);
  struct stat st;
  if (::stat(fullPath.c_str(), &st)) {
    return;
  }
  if (!m_pState->GetInitializedCacheValue("CMAKE_CACHEFILE_DIR")) {
    cmSystemTools::RemoveFile(fullPath);
    return;
  }
  off_t fsize = st.st_size;
  off_t const maxFileSize = 50 * 1024;
  if (fsize < maxFileSize) {
    // TODO: truncate file
    return;
  }
}

void CMake::MarkCliAsUsed(std::string const& variable)
{
  m_usedCliVariables[variable] = true;
}

void CMake::GenerateGraphViz(std::string const& fileName) const
{
#ifndef CMAKE_BOOTSTRAP
  cmGraphVizWriter gvWriter(fileName, GetGlobalGenerator());

  std::string settingsFile = cmStrCat(GetHomeOutputDirectory(), "/CMakeGraphVizOptions.cmake");
  std::string fallbackSettingsFile = cmStrCat(GetHomeDirectory(), "/CMakeGraphVizOptions.cmake");

  gvWriter.ReadSettings(settingsFile, fallbackSettingsFile);

  gvWriter.Write();

#endif
}

void CMake::SetProperty(
  std::string const& prop,
  cmValue value)
{
  m_pState->SetGlobalProperty(prop, value);
}

void CMake::AppendProperty(
  std::string const& prop,
  std::string const& value,
  bool asString)
{
  m_pState->AppendGlobalProperty(prop, value, asString);
}

cmValue CMake::GetProperty(std::string const& prop)
{
  return m_pState->GetGlobalProperty(prop);
}

bool CMake::GetPropertyAsBool(std::string const& prop)
{
  return m_pState->GetGlobalPropertyAsBool(prop);
}

cmInstalledFile* CMake::GetOrCreateInstalledFile(
  cmMakefile* mf,
  std::string const& name)
{
  auto i = m_installedFiles.find(name);

  if (i != m_installedFiles.end()) {
    cmInstalledFile& file = i->second;
    return &file;
  }
  cmInstalledFile& file = m_installedFiles[name];
  file.SetName(mf, name);
  return &file;
}

cmInstalledFile const* CMake::GetInstalledFile(std::string const& name) const
{
  auto i = m_installedFiles.find(name);

  if (i != m_installedFiles.end()) {
    cmInstalledFile const& file = i->second;
    return &file;
  }
  return nullptr;
}

int CMake::GetSystemInformation(std::vector<std::string>& args)
{
  // so create the directory
  std::string resultFile;
  std::string cwd = cmSystemTools::GetLogicalWorkingDirectory();
  std::string destPath = cwd + "/__cmake_systeminformation";
  cmSystemTools::RemoveADirectory(destPath);
  cmSystemTools::MakeDirectory(destPath);
  // if (!cmSystemTools::MakeDirectory(destPath)) {
  //  std::cerr << "Error: --system-information must be run from a "
  //               "writable directory!\n";
  //  return 1;
  //}

  // process the arguments
  bool writeToStdout = true;
  for (unsigned int i = 1; i < args.size(); ++i) {
    std::string const& arg = args[i];
    if (cmHasLiteralPrefix(arg, "-G")) {
      std::string value = arg.substr(2);
      if (value.empty()) {
        ++i;
        if (i >= args.size()) {
          cmSystemTools::Error("No generator specified for -G");
          //          PrintGeneratorList();
          return -1;
        }
        value = args[i];
      }
      auto gen = CreateGlobalGenerator(value);
      if (!gen) {
        cmSystemTools::Error("Could not create named generator " + value);
        //        PrintGeneratorList();
      } else {
        SetGlobalGenerator(std::move(gen));
      }
    }
    // no option assume it is the output file
    else {
      if (!cmSystemTools::FileIsFullPath(arg)) {
        resultFile = cmStrCat(cwd, '/');
      }
      resultFile += arg;
      writeToStdout = false;
    }
  }

  // we have to find the module directory, so we can copy the files
  AddCMakePaths();
  std::string modulesPath = cmStrCat(cmSystemTools::GetCMakeRoot(), "/Modules");
  std::string inFile = cmStrCat(modulesPath, "/SystemInformation.cmake");
  std::string outFile = cmStrCat(destPath, "/CMakeLists.txt");

  // Copy file
  if (!cmsys::SystemTools::CopyFileAlways(inFile, outFile)) {
    std::cerr << "Error copying file \"" << inFile << "\" to \"" << outFile << "\".\n";
    return 1;
  }

  // do we write to a file or to stdout?
  if (resultFile.empty()) {
    resultFile = cmStrCat(cwd, "/__cmake_systeminformation/results.txt");
  }

  {
    // now run cmake on the CMakeLists file
    cmWorkingDirectory workdir(destPath);
    if (workdir.Failed()) {
      // We created the directory and we were able to copy the CMakeLists.txt
      // file to it, so we wouldn't expect to get here unless the default
      // permissions are questionable or some other process has deleted the
      // directory
      std::cerr << workdir.GetError() << '\n';
      return 1;
    }
    std::vector<std::string> args2;
    args2.push_back(args[0]);
    args2.push_back(destPath);
    args2.push_back("-DRESULT_FILE=" + resultFile);
    int res = Run(args2, false);

    if (res != 0) {
      std::cerr << "Error: --system-information failed on internal CMake!\n";
      return res;
    }
  }

  // echo results to stdout if needed
  if (writeToStdout) {
    FILE* fin = cmsys::SystemTools::Fopen(resultFile, "r");
    if (fin) {
      int const bufferSize = 4096;
      char buffer[bufferSize];
      size_t n;
      while ((n = fread(buffer, 1, bufferSize, fin)) > 0) {
        for (char* c = buffer; c < buffer + n; ++c) {
          putc(*c, stdout);
        }
        fflush(stdout);
      }
      fclose(fin);
    }
  }

  // clean up the directory
  cmSystemTools::RemoveADirectory(destPath);
  return 0;
}

void CMake::IssueMessage(
  MessageType t,
  std::string const& text,
  cmListFileBacktrace const& backtrace) const
{
  m_pMessenger->IssueMessage(t, text, backtrace);
}

std::vector<std::string> CMake::GetDebugConfigs()
{
  cmList configs;
  if (cmValue config_list = m_pState->GetGlobalProperty("DEBUG_CONFIGURATIONS")) {
    // Expand the specified list and convert to upper-case.
    configs.assign(*config_list);
    configs.transform(cmList::TransformAction::TOUPPER);
  }
  // If no configurations were specified, use a default list.
  if (configs.empty()) {
    configs.emplace_back("DEBUG");
  }
  return std::move(configs.data());
}

//  TODO:   More terrible code.  All setup with the real code at the end.
int CMake::Build(
  int jobs,
  std::string dir,
  std::vector<std::string> targets,
  std::string config,
  std::vector<std::string> nativeOptions,
  cmBuildOptions& buildOptions,
  bool verbose,
  std::string const& presetName,
  bool listPresets,
  std::vector<std::string> const& args)
{
  SetHomeDirectory("");
  SetHomeOutputDirectory("");

#if !defined(CMAKE_BOOTSTRAP)
  if (!presetName.empty() || listPresets) {
    SetHomeDirectory(cmSystemTools::GetLogicalWorkingDirectory());
    SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());

    cmCMakePresetsGraph settingsFile;
    auto result = settingsFile.ReadProjectPresets(GetHomeDirectory());
    if (result != true) {
      cmSystemTools::Error(
        cmStrCat("Could not read presets from ", GetHomeDirectory(), ":\n", settingsFile.parseState.GetErrorMessage()));
      return 1;
    }

    if (listPresets) {
      settingsFile.PrintBuildPresetList();
      return 0;
    }

    auto presetPair = settingsFile.BuildPresets.find(presetName);
    if (presetPair == settingsFile.BuildPresets.end()) {
      cmSystemTools::Error(cmStrCat("No such build preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
      settingsFile.PrintBuildPresetList();
      return 1;
    }

    if (presetPair->second.Unexpanded.Hidden) {
      cmSystemTools::Error(cmStrCat("Cannot use hidden build preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
      settingsFile.PrintBuildPresetList();
      return 1;
    }

    auto const& expandedPreset = presetPair->second.Expanded;
    if (!expandedPreset) {
      cmSystemTools::Error(cmStrCat("Could not evaluate build preset \"", presetName, "\": Invalid macro expansion"));
      settingsFile.PrintBuildPresetList();
      return 1;
    }

    if (!expandedPreset->ConditionResult) {
      cmSystemTools::Error(
        cmStrCat("Cannot use disabled build preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
      settingsFile.PrintBuildPresetList();
      return 1;
    }

    auto configurePresetPair = settingsFile.ConfigurePresets.find(expandedPreset->ConfigurePreset);
    if (configurePresetPair == settingsFile.ConfigurePresets.end()) {
      cmSystemTools::Error(
        cmStrCat("No such configure preset in ", GetHomeDirectory(), ": \"", expandedPreset->ConfigurePreset, '"'));
      PrintPresetList(settingsFile);
      return 1;
    }

    if (configurePresetPair->second.Unexpanded.Hidden) {
      cmSystemTools::Error(cmStrCat(
        "Cannot use hidden configure preset in ", GetHomeDirectory(), ": \"", expandedPreset->ConfigurePreset, '"'));
      PrintPresetList(settingsFile);
      return 1;
    }

    auto const& expandedConfigurePreset = configurePresetPair->second.Expanded;
    if (!expandedConfigurePreset) {
      cmSystemTools::Error(cmStrCat(
        "Could not evaluate configure preset \"", expandedPreset->ConfigurePreset, "\": Invalid macro expansion"));
      return 1;
    }

    if (!expandedConfigurePreset->BinaryDir.empty()) {
      dir = expandedConfigurePreset->BinaryDir;
    }

    m_unprocessedPresetEnvironment = expandedPreset->Environment;
    ProcessPresetEnvironment();

    if (
      (jobs == CMake::DEFAULT_BUILD_PARALLEL_LEVEL || jobs == CMake::NO_BUILD_PARALLEL_LEVEL) && expandedPreset->Jobs) {
      jobs = *expandedPreset->Jobs;
    }

    if (targets.empty()) {
      targets.insert(targets.begin(), expandedPreset->m_targets.begin(), expandedPreset->m_targets.end());
    }

    if (config.empty()) {
      config = expandedPreset->Configuration;
    }

    if (!buildOptions.Clean && expandedPreset->CleanFirst) {
      buildOptions.Clean = *expandedPreset->CleanFirst;
    }

    if (buildOptions.ResolveMode == PackageResolveMode::Default && expandedPreset->ResolvePackageReferences) {
      buildOptions.ResolveMode = *expandedPreset->ResolvePackageReferences;
    }

    if (!verbose && expandedPreset->Verbose) {
      verbose = *expandedPreset->Verbose;
    }

    if (nativeOptions.empty()) {
      nativeOptions.insert(
        nativeOptions.begin(), expandedPreset->NativeToolOptions.begin(), expandedPreset->NativeToolOptions.end());
    }
  }
#endif

  if (!cmSystemTools::FileIsDirectory(dir)) {
    std::cerr << "Error: " << dir << " is not a directory\n";
    return 1;
  }

  std::string cachePath = FindCacheFile(dir);
  if (!LoadCache(cachePath)) {
    std::cerr << "Error: not a CMake build directory (missing CMakeCache.txt)\n";
    return 1;
  }
  cmValue cachedGenerator = m_pState->GetCacheEntryValue("CMAKE_GENERATOR");
  if (!cachedGenerator) {
    std::cerr << "Error: could not find CMAKE_GENERATOR in Cache\n";
    return 1;
  }
  auto gen = CreateGlobalGenerator(*cachedGenerator);
  if (!gen) {
    std::cerr << "Error: could not create CMAKE_GENERATOR \"" << *cachedGenerator << "\"\n";
    return 1;
  }
  SetGlobalGenerator(std::move(gen));
  cmValue cachedGeneratorInstance = m_pState->GetCacheEntryValue("CMAKE_GENERATOR_INSTANCE");
  if (cachedGeneratorInstance) {
    cmMakefile mf(GetGlobalGenerator(), GetCurrentSnapshot());
    if (!m_pGlobalGenerator->SetGeneratorInstance(*cachedGeneratorInstance, &mf)) {
      return 1;
    }
  }
  cmValue cachedGeneratorPlatform = m_pState->GetCacheEntryValue("CMAKE_GENERATOR_PLATFORM");
  if (cachedGeneratorPlatform) {
    cmMakefile mf(GetGlobalGenerator(), GetCurrentSnapshot());
    if (!m_pGlobalGenerator->SetGeneratorPlatform(*cachedGeneratorPlatform, &mf)) {
      return 1;
    }
  }
  cmValue cachedGeneratorToolset = m_pState->GetCacheEntryValue("CMAKE_GENERATOR_TOOLSET");
  if (cachedGeneratorToolset) {
    cmMakefile mf(GetGlobalGenerator(), GetCurrentSnapshot());
    if (!m_pGlobalGenerator->SetGeneratorToolset(*cachedGeneratorToolset, true, &mf)) {
      return 1;
    }
  }
  std::string projName;
  cmValue cachedProjectName = m_pState->GetCacheEntryValue("CMAKE_PROJECT_NAME");
  if (!cachedProjectName) {
    std::cerr << "Error: could not find CMAKE_PROJECT_NAME in Cache\n";
    return 1;
  }
  projName = *cachedProjectName;

  if (m_pState->GetCacheEntryValue("CMAKE_VERBOSE_MAKEFILE").IsOn()) {
    verbose = true;
  }

#ifdef CMAKE_HAVE_VS_GENERATORS
  // For VS generators, explicitly check if regeneration is necessary before
  // actually starting the build. If not done separately from the build
  // itself, there is the risk of building an out-of-date solution file due
  // to limitations of the underlying build system.
  std::string const stampList =
    cachePath + "/" + "CMakeFiles/" + cmGlobalVisualStudio14Generator::GetGenerateStampList();

  // Note that the stampList file only exists for VS generators.
  if (cmSystemTools::FileExists(stampList)) {

    AddScriptingCommands();

    if (!isGenerateStampListUpToDate(stampList)) {
      // Correctly initialize the home (=source) and home output (=binary)
      // directories, which is required for running the generation step.
      std::string homeOrig = GetHomeDirectory();
      std::string homeOutputOrig = GetHomeOutputDirectory();
      SetDirectoriesFromFile(cachePath);

      AddProjectCommands();

      int ret = Configure();
      if (ret) {
        cmSystemTools::Message(
          "CMake Configure step failed.  "
          "Build files cannot be regenerated correctly.");
        return ret;
      }
      ret = Generate();
      if (ret) {
        cmSystemTools::Message(
          "CMake Generate step failed.  "
          "Build files cannot be regenerated correctly.");
        return ret;
      }
      std::string message = cmStrCat("Build files have been written to: ", GetHomeOutputDirectory());
      UpdateProgress(message, -1);

      // Restore the previously set directories to their original value.
      SetHomeDirectory(homeOrig);
      SetHomeOutputDirectory(homeOutputOrig);
    }
  }
#endif

  if (!m_pGlobalGenerator->ReadCacheEntriesForBuild(*m_pState)) {
    return 1;
  }

#if !defined(CMAKE_BOOTSTRAP)
  // cmInstrumentation instrumentation(dir);
  // if (instrumentation.HasErrors()) {
  //   return 1;
  // }
  // instrumentation.CollectTimingData(cmInstrumentationQuery::Hook::PreCMakeBuild);
#endif

  m_pGlobalGenerator->PrintBuildCommandAdvice(std::cerr, jobs);
  std::stringstream ostr;
  // `cmGlobalGenerator::Build` logs metadata about what directory and commands
  // are being executed to the `output` parameter. If CMake is verbose, print
  // this out.
  std::ostream& verbose_ostr = verbose ? std::cout : ostr;
  //    TODO: This is just terrible.  The real work is in a lambda and the instrumentation is mainline.
  auto doBuild = [this, jobs, dir, projName, targets, &verbose_ostr, config, buildOptions, verbose,
                  nativeOptions]() -> int {
    return m_pGlobalGenerator->Build(
      jobs, "", dir, projName, targets, verbose_ostr, "", config, buildOptions, verbose, cmDuration::zero(),
      cmSystemTools::OUTPUT_PASSTHROUGH, nativeOptions);
  };

#if !defined(CMAKE_BOOTSTRAP)
  int buildresult = doBuild();
  // int buildresult = instrumentation.InstrumentCommand("cmakeBuild", args, doBuild);
  // instrumentation.CollectTimingData(cmInstrumentationQuery::Hook::PostCMakeBuild);
#else
  int buildresult = doBuild();
#endif

  return buildresult;
}

bool CMake::Open(
  std::string const& dir,
  DryRun dryRun)
{
  SetHomeDirectory("");
  SetHomeOutputDirectory("");
  if (!cmSystemTools::FileIsDirectory(dir)) {
    if (dryRun == DryRun::No) {
      std::cerr << "Error: " << dir << " is not a directory\n";
    }
    return false;
  }

  std::string cachePath = FindCacheFile(dir);
  if (!LoadCache(cachePath)) {
    std::cerr << "Error: not a CMake build directory (missing CMakeCache.txt)\n";
    return false;
  }
  cmValue genName = m_pState->GetCacheEntryValue("CMAKE_GENERATOR");
  if (!genName) {
    std::cerr << "Error: could not find CMAKE_GENERATOR in Cache\n";
    return false;
  }
  cmValue extraGenName = m_pState->GetInitializedCacheValue("CMAKE_EXTRA_GENERATOR");
  std::string fullName =
    cmExternalMakefileProjectGenerator::CreateFullGeneratorName(*genName, extraGenName ? *extraGenName : "");

  std::unique_ptr<cmGlobalGenerator> gen = CreateGlobalGenerator(fullName);
  if (!gen) {
    std::cerr << "Error: could not create CMAKE_GENERATOR \"" << fullName << "\"\n";
    return false;
  }

  cmValue cachedProjectName = m_pState->GetCacheEntryValue("CMAKE_PROJECT_NAME");
  if (!cachedProjectName) {
    std::cerr << "Error: could not find CMAKE_PROJECT_NAME in Cache\n";
    return false;
  }

  return gen->Open(dir, *cachedProjectName, dryRun == DryRun::Yes);
}

#if !defined(CMAKE_BOOTSTRAP)
template <typename T>
T const* CMake::FindPresetForWorkflow(
  cm::static_string_view type,
  std::map<
    std::string,
    cmCMakePresetsGraph::PresetPair<T>> const& presets,
  cmCMakePresetsGraph::WorkflowPreset::WorkflowStep const& step)
{
  auto it = presets.find(step.PresetName);
  if (it == presets.end()) {
    cmSystemTools::Error(cmStrCat("No such ", type, " preset in ", GetHomeDirectory(), ": \"", step.PresetName, '"'));
    return nullptr;
  }

  if (it->second.Unexpanded.Hidden) {
    cmSystemTools::Error(
      cmStrCat("Cannot use hidden ", type, " preset in ", GetHomeDirectory(), ": \"", step.PresetName, '"'));
    return nullptr;
  }

  if (!it->second.Expanded) {
    cmSystemTools::Error(
      cmStrCat("Could not evaluate ", type, " preset \"", step.PresetName, "\": Invalid macro expansion"));
    return nullptr;
  }

  if (!it->second.Expanded->ConditionResult) {
    cmSystemTools::Error(
      cmStrCat("Cannot use disabled ", type, " preset in ", GetHomeDirectory(), ": \"", step.PresetName, '"'));
    return nullptr;
  }

  return &*it->second.Expanded;
}

std::function<int()> CMake::BuildWorkflowStep(std::vector<std::string> const& args)
{
  cmUVProcessChainBuilder builder;
  builder.AddCommand(args)
    .SetExternalStream(cmUVProcessChainBuilder::Stream_OUTPUT, stdout)
    .SetExternalStream(cmUVProcessChainBuilder::Stream_ERROR, stderr);
  return [builder]() -> int {
    auto chain = builder.Start();
    chain.Wait();
    return static_cast<int>(chain.GetStatus(0).ExitStatus);
  };
}
#endif

int CMake::Workflow(
  std::string const& presetName,
  WorkflowListPresets listPresets,
  WorkflowFresh fresh)
{
#ifndef CMAKE_BOOTSTRAP
  SetHomeDirectory(cmSystemTools::GetLogicalWorkingDirectory());
  SetHomeOutputDirectory(cmSystemTools::GetLogicalWorkingDirectory());

  cmCMakePresetsGraph settingsFile;
  auto result = settingsFile.ReadProjectPresets(GetHomeDirectory());
  if (result != true) {
    cmSystemTools::Error(
      cmStrCat("Could not read presets from ", GetHomeDirectory(), ":\n", settingsFile.parseState.GetErrorMessage()));
    return 1;
  }

  if (listPresets == WorkflowListPresets::Yes) {
    settingsFile.PrintWorkflowPresetList();
    return 0;
  }

  auto presetPair = settingsFile.WorkflowPresets.find(presetName);
  if (presetPair == settingsFile.WorkflowPresets.end()) {
    cmSystemTools::Error(cmStrCat("No such workflow preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
    settingsFile.PrintWorkflowPresetList();
    return 1;
  }

  if (presetPair->second.Unexpanded.Hidden) {
    cmSystemTools::Error(
      cmStrCat("Cannot use hidden workflow preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
    settingsFile.PrintWorkflowPresetList();
    return 1;
  }

  auto const& expandedPreset = presetPair->second.Expanded;
  if (!expandedPreset) {
    cmSystemTools::Error(cmStrCat("Could not evaluate workflow preset \"", presetName, "\": Invalid macro expansion"));
    settingsFile.PrintWorkflowPresetList();
    return 1;
  }

  if (!expandedPreset->ConditionResult) {
    cmSystemTools::Error(
      cmStrCat("Cannot use disabled workflow preset in ", GetHomeDirectory(), ": \"", presetName, '"'));
    settingsFile.PrintWorkflowPresetList();
    return 1;
  }

  struct CalculatedStep
  {
    int StepNumber;
    cm::static_string_view Type;
    std::string m_name;
    std::function<int()> m_action;

    CalculatedStep(
      int stepNumber,
      cm::static_string_view type,
      std::string name,
      std::function<int()> action)
      : StepNumber(stepNumber)
      , Type(type)
      , m_name(std::move(name))
      , m_action(std::move(action))
    {
    }
  };

  std::vector<CalculatedStep> steps;
  steps.reserve(expandedPreset->Steps.size());
  int stepNumber = 1;
  for (auto const& step : expandedPreset->Steps) {
    switch (step.PresetType) {
      case cmCMakePresetsGraph::WorkflowPreset::WorkflowStep::Type::Configure: {
        auto const* configurePreset = FindPresetForWorkflow("configure"_s, settingsFile.ConfigurePresets, step);
        if (!configurePreset) {
          return 1;
        }
        std::vector<std::string> args{ cmSystemTools::GetCMakeCommand(), "--preset", step.PresetName };
        if (fresh == WorkflowFresh::Yes) {
          args.emplace_back("--fresh");
        }
        steps.emplace_back(stepNumber, "configure"_s, step.PresetName, BuildWorkflowStep(args));
      } break;
      case cmCMakePresetsGraph::WorkflowPreset::WorkflowStep::Type::Build: {
        auto const* buildPreset = FindPresetForWorkflow("build"_s, settingsFile.BuildPresets, step);
        if (!buildPreset) {
          return 1;
        }
        steps.emplace_back(
          stepNumber, "build"_s, step.PresetName,
          BuildWorkflowStep({ cmSystemTools::GetCMakeCommand(), "--build", "--preset", step.PresetName }));
      } break;
      case cmCMakePresetsGraph::WorkflowPreset::WorkflowStep::Type::Test: {
        auto const* testPreset = FindPresetForWorkflow("test"_s, settingsFile.TestPresets, step);
        if (!testPreset) {
          return 1;
        }
        steps.emplace_back(
          stepNumber, "test"_s, step.PresetName,
          BuildWorkflowStep({ cmSystemTools::GetCTestCommand(), "--preset", step.PresetName }));
      } break;
      case cmCMakePresetsGraph::WorkflowPreset::WorkflowStep::Type::Package: {
        auto const* packagePreset = FindPresetForWorkflow("package"_s, settingsFile.PackagePresets, step);
        if (!packagePreset) {
          return 1;
        }
        steps.emplace_back(
          stepNumber, "package"_s, step.PresetName,
          BuildWorkflowStep({ cmSystemTools::GetCPackCommand(), "--preset", step.PresetName }));
      } break;
    }
    stepNumber++;
  }

  int stepResult;
  bool first = true;
  for (auto const& step : steps) {
    if (!first) {
      std::cout << "\n";
    }
    std::cout << "Executing workflow step " << step.StepNumber << " of " << steps.size() << ": " << step.Type
              << " preset \"" << step.m_name << "\"\n\n"
              << std::flush;
    if ((stepResult = step.m_action()) != 0) {
      return stepResult;
    }
    first = false;
  }
#endif

  return 0;
}

void CMake::WatchUnusedCli(std::string const& var)
{
#ifndef CMAKE_BOOTSTRAP
  m_pVariableWatch->AddWatch(var, cmWarnUnusedCliWarning, this);
  if (!cm::contains(m_usedCliVariables, var)) {
    m_usedCliVariables[var] = false;
  }
#endif
}

void CMake::UnwatchUnusedCli(std::string const& var)
{
#ifndef CMAKE_BOOTSTRAP
  m_pVariableWatch->RemoveWatch(var, cmWarnUnusedCliWarning);
  m_usedCliVariables.erase(var);
#endif
}

void CMake::RunCheckForUnusedVariables()
{
#ifndef CMAKE_BOOTSTRAP
  bool haveUnused = false;
  std::ostringstream msg;
  msg << "Manually-specified variables were not used by the project:";
  for (auto const& it : m_usedCliVariables) {
    if (!it.second) {
      haveUnused = true;
      msg << "\n  " << it.first;
    }
  }
  if (haveUnused) {
    IssueMessage(MessageType::WARNING, msg.str());
  }
#endif
}

bool CMake::GetSuppressDevWarnings() const
{
  return m_pMessenger->GetSuppressDevWarnings();
}

void CMake::SetSuppressDevWarnings(bool b)
{
  std::string value;

  // equivalent to -Wno-dev
  if (b) {
    value = "TRUE";
  }
  // equivalent to -Wdev
  else {
    value = "FALSE";
  }

  AddCacheEntry(
    "CMAKE_SUPPRESS_DEVELOPER_WARNINGS", value,
    "Suppress Warnings that are meant for"
    " the author of the CMakeLists.txt files.",
    cmStateEnums::INTERNAL);
}

bool CMake::GetSuppressDeprecatedWarnings() const
{
  return m_pMessenger->GetSuppressDeprecatedWarnings();
}

void CMake::SetSuppressDeprecatedWarnings(bool b)
{
  std::string value;

  // equivalent to -Wno-deprecated
  if (b) {
    value = "FALSE";
  }
  // equivalent to -Wdeprecated
  else {
    value = "TRUE";
  }

  AddCacheEntry(
    "CMAKE_WARN_DEPRECATED", value,
    "Whether to issue warnings for deprecated "
    "functionality.",
    cmStateEnums::INTERNAL);
}

bool CMake::GetDevWarningsAsErrors() const
{
  return m_pMessenger->GetDevWarningsAsErrors();
}

void CMake::SetDevWarningsAsErrors(bool b)
{
  std::string value;

  // equivalent to -Werror=dev
  if (b) {
    value = "FALSE";
  }
  // equivalent to -Wno-error=dev
  else {
    value = "TRUE";
  }

  AddCacheEntry(
    "CMAKE_SUPPRESS_DEVELOPER_ERRORS", value,
    "Suppress errors that are meant for"
    " the author of the CMakeLists.txt files.",
    cmStateEnums::INTERNAL);
}

bool CMake::GetDeprecatedWarningsAsErrors() const
{
  return m_pMessenger->GetDeprecatedWarningsAsErrors();
}

void CMake::SetDeprecatedWarningsAsErrors(bool b)
{
  std::string value;

  // equivalent to -Werror=deprecated
  if (b) {
    value = "TRUE";
  }
  // equivalent to -Wno-error=deprecated
  else {
    value = "FALSE";
  }

  AddCacheEntry(
    "CMAKE_ERROR_DEPRECATED", value,
    "Whether to issue deprecation errors for macros"
    " and functions.",
    cmStateEnums::INTERNAL);
}

void CMake::SetDebugFindOutputPkgs(std::string const& args)
{
  m_debugFindPkgs.emplace(args);
}

void CMake::SetDebugFindOutputVars(std::string const& args)
{
  m_debugFindVars.emplace(args);
}

bool CMake::GetDebugFindOutput(std::string const& var) const
{
  return m_debugFindVars.count(var);
}

bool CMake::GetDebugFindPkgOutput(std::string const& pkg) const
{
  return m_debugFindPkgs.count(pkg);
}

void CMake::SetCMakeListName(std::string const& name)
{
  m_cmakeListName = name;
}

std::string CMake::GetCMakeListFile(std::string const& dir) const
{
  std::string listFile = cmStrCat(dir, '/', m_cmakeListName);
  if (m_cmakeListName.empty() || !cmSystemTools::FileExists(listFile, true)) {
    return cmStrCat(dir, "/CMakeLists.txt");
  }
  return listFile;
}

#if !defined(CMAKE_BOOTSTRAP)
cmMakefileProfilingData& CMake::GetProfilingOutput()
{
  return *(m_profilingOutput);
}

bool CMake::IsProfilingEnabled() const
{
  return static_cast<bool>(m_profilingOutput);
}
#endif
