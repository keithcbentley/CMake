/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */

#include "cmConfigure.h" // IWYU pragma: keep

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cm/memory>
#include <cm/optional>
#include <cmext/algorithm>

#include <cm3p/uv.h>

#include "cmBuildOptions.h"
#include "cmCommandLineArgument.h"
#include "cmConsoleBuf.h"
#include "cmDocumentationEntry.h"
#include "cmGlobalGenerator.h"
#include "cmInstallScriptHandler.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmMessageMetadata.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"
#include "cmake.h"
#include "cmakeException.h"
#include "cmakeMessage.h"
#include "cmcmd.h"

#ifndef CMAKE_BOOTSTRAP
#  include "cmDocumentation.h"
#  include "cmDynamicLoader.h"
#endif

#include "cmsys/Encoding.hxx"
#include "cmsys/RegularExpression.hxx"
#include "cmsys/Terminal.h"

namespace {

//  TODO: why is this consoleBuf thing passed around?
//  TODO: why is the consoleBuf a unique pointer? why not just entire program lifetime?
//  TODO: why is the arg list being recreated?
int do_command(
  int ac,
  char const* const* av,
  std::unique_ptr<cmConsoleBuf> consoleBuf)
{
  std::vector<std::string> args;
  args.reserve(ac - 1);
  args.emplace_back(av[0]);
  cm::append(args, av + 2, av + ac);
  return cmcmd::ExecuteCMakeCommand(args, std::move(consoleBuf));
}

cmMakefile* cmakemainGetMakefile(CMake* cmake)
{
  //  TODO: why is a null check needed?
  //  TODO: why isn't this a method on CMake?
  //  TODO: why is there a check for debug output?
  //  TODO: is this a fatal error if we can't get the makefile?
  if (cmake && cmake->GetDebugOutput()) {
    cmGlobalGenerator* globalGenerator = cmake->GetGlobalGenerator();
    if (globalGenerator) {
      return globalGenerator->GetCurrentMakefile();
    }
  }
  return nullptr;
}

std::string cmakemainGetStack(CMake* cmake)
{
  std::string msg;
  cmMakefile* mf = cmakemainGetMakefile(cmake);
  if (mf) {
    msg = mf->FormatListFileStack();
    if (!msg.empty()) {
      msg = "\n   Called from: " + msg;
    }
  }

  return msg;
}

void cmakemainMessageCallback(
  std::string const& message,
  cmMessageMetadata const& md,
  CMake* cmake)
{
#if defined(_WIN32)
  // FIXME: On Windows we replace cerr's streambuf with a custom
  // implementation that converts our internal UTF-8 encoding to the
  // console's encoding.  It also does *not* replace LF with CRLF.
  // Since stderr does not convert encoding and does convert LF, we
  // cannot use it to print messages.  Another implementation will
  // be needed to print colored messages on Windows.
  static_cast<void>(md);
  std::cerr << message << cmakemainGetStack(cmake) << std::endl;
#else
  cmsysTerminal_cfprintf(md.desiredColor, stderr, "%s", m.c_str());
  fflush(stderr); // stderr is buffered in some cases.
  std::cerr << cmakemainGetStack(cm) << std::endl;
#endif
}

void cmakemainProgressCallback(
  std::string const& message,
  float progress,
  CMake* cmake)
{
  cmMakefile* makefile = cmakemainGetMakefile(cmake);
  std::string dir;
  if (makefile && cmHasLiteralPrefix(message, "Configuring") && (progress < 0)) {
    dir = cmStrCat(' ', makefile->GetCurrentSourceDirectory());
  } else if (makefile && cmHasLiteralPrefix(message, "Generating")) {
    dir = cmStrCat(' ', makefile->GetCurrentBinaryDirectory());
  }

  if ((progress < 0) || (!dir.empty())) {
    std::cout << "-- " << message << dir << cmakemainGetStack(cmake) << std::endl;
  }
}

//  TODO: what the hell is this? Why?
std::function<bool(std::string const& value)> getShowCachedCallback(
  bool& show_flag,
  bool* help_flag = nullptr,
  std::string* filter = nullptr)
{
  return [=, &show_flag](std::string const& value) -> bool {
    show_flag = true;
    if (help_flag) {
      *help_flag = true;
    }
    if (filter) {
      *filter = value;
    }
    return true;
  };
}

//  TODO: get rid of this whole return code stuff.  Use exceptions.
int do_cmake(
  int ac,
  char const* const* av)
{
  FunctionTrace f(__func__);

  //  TODO: Why would we get this far if we don't have a working directory?
  if (cmSystemTools::GetLogicalWorkingDirectory().empty()) {
    std::cerr << "Current working directory cannot be established." << std::endl;
    return 1;
  }

  bool sysinfo = false;
  bool list_cached = false;
  bool list_all_cached = false;
  bool list_help = false;
  // (Regex) Filter on the cached variable(s) to print.
  std::string filter_var_name;
  bool view_only = false;
  CMake::WorkingMode workingMode = CMake::NORMAL_MODE;
  std::vector<std::string> parsedArgs;

  //    TODO: this is ridiculous.  How complicated is it to parse command line arguments.
  using CommandArgument = cmCommandLineArgument<bool(std::string const& value)>;
  std::vector<CommandArgument> arguments = {
    CommandArgument{ "--system-information", CommandArgument::Values::Zero, CommandArgument::setToTrue(sysinfo) },
    CommandArgument{ "-N", CommandArgument::Values::Zero, CommandArgument::setToTrue(view_only) },
    CommandArgument{ "-LAH", CommandArgument::Values::Zero, getShowCachedCallback(list_all_cached, &list_help) },
    CommandArgument{ "-LA", CommandArgument::Values::Zero, getShowCachedCallback(list_all_cached) },
    CommandArgument{ "-LH", CommandArgument::Values::Zero, getShowCachedCallback(list_cached, &list_help) },
    CommandArgument{ "-L", CommandArgument::Values::Zero, getShowCachedCallback(list_cached) },
    CommandArgument{ "-LRAH", CommandArgument::Values::One,
                     getShowCachedCallback(list_all_cached, &list_help, &filter_var_name) },
    CommandArgument{ "-LRA", CommandArgument::Values::One,
                     getShowCachedCallback(list_all_cached, nullptr, &filter_var_name) },
    CommandArgument{ "-LRH", CommandArgument::Values::One,
                     getShowCachedCallback(list_cached, &list_help, &filter_var_name) },
    CommandArgument{ "-LR", CommandArgument::Values::One,
                     getShowCachedCallback(list_cached, nullptr, &filter_var_name) },
    CommandArgument{ "-P", "No script specified for argument -P", CommandArgument::Values::One,
                     CommandArgument::RequiresSeparator::No,
                     [&](std::string const& value) -> bool {
                       workingMode = CMake::SCRIPT_MODE;
                       parsedArgs.emplace_back("-P");
                       parsedArgs.push_back(value);
                       return true;
                     } },
    CommandArgument{ "--find-package", CommandArgument::Values::Zero,
                     [&](std::string const&) -> bool {
                       workingMode = CMake::FIND_PACKAGE_MODE;
                       parsedArgs.emplace_back("--find-package");
                       return true;
                     } },
    CommandArgument{ "--list-presets", CommandArgument::Values::ZeroOrOne,
                     [&](std::string const& value) -> bool {
                       workingMode = CMake::HELP_MODE;
                       parsedArgs.emplace_back("--list-presets");
                       parsedArgs.emplace_back(value);
                       return true;
                     } },
  };

  std::vector<std::string> inputArgs;
  inputArgs.reserve(ac);
  cm::append(inputArgs, av, av + ac);

  for (decltype(inputArgs.size()) i = 0; i < inputArgs.size(); ++i) {
    std::string const& arg = inputArgs[i];
    bool matched = false;

    // Only in script mode do we stop parsing instead
    // of preferring the last mode flag provided
    if (arg == "--" && workingMode == CMake::SCRIPT_MODE) {
      parsedArgs = inputArgs;
      break;
    }
    for (auto const& m : arguments) {
      if (m.matches(arg)) {
        matched = true;
        if (m.parse(arg, i, inputArgs)) {
          break;
        }
        return 1; // failed to parse
      }
    }
    if (!matched) {
      parsedArgs.emplace_back(av[i]);
    }
  }

  if (sysinfo) {
    CMake cm(CMake::RoleProject, cmState::Project);
    cm.SetHomeDirectory("");
    cm.SetHomeOutputDirectory("");
    int ret = cm.GetSystemInformation(parsedArgs);
    return ret;
  }
  CMake::Role const role = workingMode == CMake::SCRIPT_MODE ? CMake::RoleScript : CMake::RoleProject;
  cmState::Mode mode = cmState::Unknown;
  switch (workingMode) {
    case CMake::NORMAL_MODE:
    case CMake::HELP_MODE:
      mode = cmState::Project;
      break;
    case CMake::SCRIPT_MODE:
      mode = cmState::Script;
      break;
    case CMake::FIND_PACKAGE_MODE:
      mode = cmState::FindPackage;
      break;
  }
  auto const failurePolicy = workingMode == CMake::NORMAL_MODE ? CMake::CommandFailureAction::EXIT_CODE
                                                               : CMake::CommandFailureAction::FATAL_ERROR;
  CMake cm(role, mode);
  cm.SetHomeDirectory("");
  cm.SetHomeOutputDirectory("");
  cmSystemTools::SetMessageCallback(
    [&cm](std::string const& msg, cmMessageMetadata const& md) { cmakemainMessageCallback(msg, md, &cm); });
  cm.SetProgressCallback([&cm](std::string const& msg, float prog) { cmakemainProgressCallback(msg, prog, &cm); });
  cm.SetWorkingMode(workingMode, failurePolicy);

  int res = cm.Run(parsedArgs, view_only);
  if (list_cached || list_all_cached) {
    std::cout << "-- Cache values" << std::endl;
    std::vector<std::string> keys = cm.GetState()->GetCacheEntryKeys();
    cmsys::RegularExpression regex_var_name;
    if (!filter_var_name.empty()) {
      regex_var_name.compile(filter_var_name);
    }
    for (std::string const& k : keys) {
      if (regex_var_name.is_valid() && !regex_var_name.find(k)) {
        continue;
      }

      cmStateEnums::CacheEntryType t = cm.GetState()->GetCacheEntryType(k);
      if (t != cmStateEnums::INTERNAL && t != cmStateEnums::STATIC && t != cmStateEnums::UNINITIALIZED) {
        cmValue advancedProp = cm.GetState()->GetCacheEntryProperty(k, "ADVANCED");
        if (list_all_cached || !advancedProp) {
          if (list_help) {
            cmValue help = cm.GetState()->GetCacheEntryProperty(k, "HELPSTRING");
            std::cout << "// " << (help ? *help : "") << std::endl;
          }
          std::cout << k << ":" << cmState::CacheEntryTypeToString(t) << "=" << cm.GetState()->GetSafeCacheEntryValue(k)
                    << std::endl;
          if (list_help) {
            std::cout << std::endl;
          }
        }
      }
    }
  }

  // Always return a non-negative value (except exit code from SCRIPT_MODE).
  // Windows tools do not always interpret negative return values as errors.
  if (res != 0) {
    auto scriptModeExitCode = cm.HasScriptModeExitCode() ? cm.GetScriptModeExitCode() : 0;
    res = scriptModeExitCode ? scriptModeExitCode : 1;
#ifdef CMake_ENABLE_DEBUGGER
    cm.StopDebuggerIfNeeded(res);
#endif
    return res;
  }
#ifdef CMake_ENABLE_DEBUGGER
  cm.StopDebuggerIfNeeded(0);
#endif
  return 0;
}

#ifndef CMAKE_BOOTSTRAP
int extract_job_number(
  std::string const& command,
  std::string const& jobString)
{
  int jobs = -1;
  unsigned long numJobs = 0;
  if (jobString.empty()) {
    jobs = CMake::DEFAULT_BUILD_PARALLEL_LEVEL;
  } else if (cmStrToULong(jobString, &numJobs)) {
    if (numJobs == 0) {
      std::cerr << "The <jobs> value requires a positive integer argument.\n\n";
    } else if (numJobs > INT_MAX) {
      std::cerr << "The <jobs> value is too large.\n\n";
    } else {
      jobs = static_cast<int>(numJobs);
    }
  } else {
    std::cerr << "'" << command << "' invalid number '" << jobString << "' given.\n\n";
  }
  return jobs;
}

std::function<bool(std::string const&)> extract_job_number_lambda_builder(
  std::string& dir,
  int& jobs,
  std::string const& flag)
{
  return [&dir, &jobs, flag](std::string const& value) -> bool {
    jobs = extract_job_number(flag, value);
    if (jobs < 0) {
      dir.clear();
    }
    return true;
  };
};
#endif

//  TODO:   Ridiculous.  Lines and lines of set up, and then call the real build in another function.
int do_build(
  int ac,
  char const* const* av)
{
#ifdef CMAKE_BOOTSTRAP
  std::cerr << "This cmake does not support --build\n";
  return -1;
#else
  int jobs = CMake::NO_BUILD_PARALLEL_LEVEL;
  std::vector<std::string> targets;
  std::string config;
  std::string dir;
  std::vector<std::string> nativeOptions;
  bool nativeOptionsPassed = false;
  bool cleanFirst = false;
  bool foundClean = false;
  bool foundNonClean = false;
  PackageResolveMode resolveMode = PackageResolveMode::Default;
  bool verbose = cmSystemTools::HasEnv("VERBOSE");
  std::string presetName;
  bool listPresets = false;

  auto jLambda = extract_job_number_lambda_builder(dir, jobs, "-j");
  auto parallelLambda = extract_job_number_lambda_builder(dir, jobs, "--parallel");

  auto targetLambda = [&](std::string const& value) -> bool {
    if (!value.empty()) {
      cmList values{ value };
      for (auto const& v : values) {
        targets.emplace_back(v);
        if (v == "clean") {
          foundClean = true;
        } else {
          foundNonClean = true;
        }
      }
      return true;
    }
    return false;
  };
  auto resolvePackagesLambda = [&](std::string const& value) -> bool {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);

    if (v == "on") {
      resolveMode = PackageResolveMode::Force;
    } else if (v == "only") {
      resolveMode = PackageResolveMode::OnlyResolve;
    } else if (v == "off") {
      resolveMode = PackageResolveMode::Disable;
    } else {
      return false;
    }

    return true;
  };
  auto verboseLambda = [&](std::string const&) -> bool {
    verbose = true;
    return true;
  };

  using CommandArgument = cmCommandLineArgument<bool(std::string const& value)>;

  std::vector<CommandArgument> arguments = {
    CommandArgument{ "--preset", CommandArgument::Values::One, CommandArgument::setToValue(presetName) },
    CommandArgument{ "--list-presets", CommandArgument::Values::Zero, CommandArgument::setToTrue(listPresets) },
    CommandArgument{ "-j", CommandArgument::Values::ZeroOrOne, CommandArgument::RequiresSeparator::No, jLambda },
    CommandArgument{ "--parallel", CommandArgument::Values::ZeroOrOne, CommandArgument::RequiresSeparator::No,
                     parallelLambda },
    CommandArgument{ "-t", CommandArgument::Values::OneOrMore, targetLambda },
    CommandArgument{ "--target", CommandArgument::Values::OneOrMore, targetLambda },
    CommandArgument{ "--config", CommandArgument::Values::One, CommandArgument::setToValue(config) },
    CommandArgument{ "--clean-first", CommandArgument::Values::Zero, CommandArgument::setToTrue(cleanFirst) },
    CommandArgument{ "--resolve-package-references", CommandArgument::Values::One, resolvePackagesLambda },
    CommandArgument{ "-v", CommandArgument::Values::Zero, verboseLambda },
    CommandArgument{ "--verbose", CommandArgument::Values::Zero, verboseLambda },
    /* legacy option no-op*/
    CommandArgument{ "--use-stderr", CommandArgument::Values::Zero, [](std::string const&) -> bool { return true; } },
    CommandArgument{ "--", CommandArgument::Values::Zero, CommandArgument::setToTrue(nativeOptionsPassed) },
  };

  if (ac >= 3) {
    std::vector<std::string> inputArgs;

    inputArgs.reserve(ac - 2);
    cm::append(inputArgs, av + 2, av + ac);

    decltype(inputArgs.size()) i = 0;
    for (; i < inputArgs.size() && !nativeOptionsPassed; ++i) {

      std::string const& arg = inputArgs[i];
      bool matched = false;
      bool parsed = false;
      for (auto const& m : arguments) {
        matched = m.matches(arg);
        if (matched) {
          parsed = m.parse(arg, i, inputArgs);
          break;
        }
      }
      if (!matched && i == 0) {
        dir = cmSystemTools::ToNormalizedPathOnDisk(arg);
        matched = true;
        parsed = true;
      }
      if (!(matched && parsed)) {
        dir.clear();
        if (!matched) {
          std::cerr << "Unknown argument " << arg << std::endl;
        }
        break;
      }
    }

    if (nativeOptionsPassed) {
      cm::append(nativeOptions, inputArgs.begin() + i, inputArgs.end());
    }
  }

  if (foundClean && foundNonClean) {
    std::cerr << "Error: Building 'clean' and other targets together "
                 "is not supported."
              << std::endl;
    dir.clear();
  }

  if (jobs == CMake::NO_BUILD_PARALLEL_LEVEL) {
    std::string parallel;
    if (cmSystemTools::GetEnv("CMAKE_BUILD_PARALLEL_LEVEL", parallel)) {
      if (parallel.empty()) {
        jobs = CMake::DEFAULT_BUILD_PARALLEL_LEVEL;
      } else {
        unsigned long numJobs = 0;
        if (cmStrToULong(parallel, &numJobs)) {
          if (numJobs == 0) {
            std::cerr << "The CMAKE_BUILD_PARALLEL_LEVEL environment variable "
                         "requires a positive integer argument.\n\n";
            dir.clear();
          } else if (numJobs > INT_MAX) {
            std::cerr << "The CMAKE_BUILD_PARALLEL_LEVEL environment variable "
                         "is too large.\n\n";
            dir.clear();
          } else {
            jobs = static_cast<int>(numJobs);
          }
        } else {
          std::cerr << "'CMAKE_BUILD_PARALLEL_LEVEL' environment variable\n"
                    << "invalid number '" << parallel << "' given.\n\n";
          dir.clear();
        }
      }
    }
  }

  if (dir.empty() && presetName.empty() && !listPresets) {
    /* clang-format off */
    std::cerr <<
      "Usage: cmake --build <dir>            "
      " [options] [-- [native-options]]\n"
      "       cmake --build --preset <preset>"
      " [options] [-- [native-options]]\n"
      "Options:\n"
      "  <dir>          = Project binary directory to be built.\n"
      "  --preset <preset>, --preset=<preset>\n"
      "                 = Specify a build preset.\n"
      "  --list-presets[=<type>]\n"
      "                 = List available build presets.\n"
      "  --parallel [<jobs>], -j [<jobs>]\n"
      "                 = Build in parallel using the given number of jobs. \n"
      "                   If <jobs> is omitted the native build tool's \n"
      "                   default number is used.\n"
      "                   The CMAKE_BUILD_PARALLEL_LEVEL environment "
      "variable\n"
      "                   specifies a default parallel level when this "
      "option\n"
      "                   is not given.\n"
      "  -t <tgt>..., --target <tgt>...\n"
      "                 = Build <tgt> instead of default targets.\n"
      "  --config <cfg> = For multi-configuration tools, choose <cfg>.\n"
      "  --clean-first  = Build target 'clean' first, then build.\n"
      "                   (To clean only, use --target 'clean'.)\n"
      "  --resolve-package-references={on|only|off}\n"
      "                 = Restore/resolve package references during build.\n"
      "  -v, --verbose  = Enable verbose output - if supported - including\n"
      "                   the build commands to be executed. \n"
      "  --             = Pass remaining options to the native tool.\n"
      ;
    /* clang-format on */
    return 1;
  }

  CMake cm(CMake::RoleInternal, cmState::Project);

  //    TODO: this is ridiculous: a lambda that captures the object that it then calls a method on.
  cmSystemTools::SetMessageCallback(
    [&cm](std::string const& msg, cmMessageMetadata const& md) { cmakemainMessageCallback(msg, md, &cm); });
  cm.SetProgressCallback([&cm](std::string const& msg, float prog) { cmakemainProgressCallback(msg, prog, &cm); });

  cmBuildOptions buildOptions(cleanFirst, false, resolveMode);
  std::vector<std::string> cmd;
  cm::append(cmd, av, av + ac);
  return cm.Build(
    jobs, dir, std::move(targets), std::move(config), std::move(nativeOptions), buildOptions, verbose, presetName,
    listPresets, cmd);
#endif
}

bool parse_default_directory_permissions(
  std::string const& permissions,
  std::string& parsedPermissionsVar)
{
  std::vector<std::string> parsedPermissions;
  enum Doing
  {
    DoingNone,
    DoingOwner,
    DoingGroup,
    DoingWorld,
    DoingOwnerAssignment,
    DoingGroupAssignment,
    DoingWorldAssignment,
  };
  Doing doing = DoingNone;

  auto uniquePushBack = [&parsedPermissions](std::string const& e) {
    if (std::find(parsedPermissions.begin(), parsedPermissions.end(), e) == parsedPermissions.end()) {
      parsedPermissions.push_back(e);
    }
  };

  for (auto const& e : permissions) {
    switch (doing) {
      case DoingNone:
        if (e == 'u') {
          doing = DoingOwner;
        } else if (e == 'g') {
          doing = DoingGroup;
        } else if (e == 'o') {
          doing = DoingWorld;
        } else {
          return false;
        }
        break;
      case DoingOwner:
        if (e == '=') {
          doing = DoingOwnerAssignment;
        } else {
          return false;
        }
        break;
      case DoingGroup:
        if (e == '=') {
          doing = DoingGroupAssignment;
        } else {
          return false;
        }
        break;
      case DoingWorld:
        if (e == '=') {
          doing = DoingWorldAssignment;
        } else {
          return false;
        }
        break;
      case DoingOwnerAssignment:
        if (e == 'r') {
          uniquePushBack("OWNER_READ");
        } else if (e == 'w') {
          uniquePushBack("OWNER_WRITE");
        } else if (e == 'x') {
          uniquePushBack("OWNER_EXECUTE");
        } else if (e == ',') {
          doing = DoingNone;
        } else {
          return false;
        }
        break;
      case DoingGroupAssignment:
        if (e == 'r') {
          uniquePushBack("GROUP_READ");
        } else if (e == 'w') {
          uniquePushBack("GROUP_WRITE");
        } else if (e == 'x') {
          uniquePushBack("GROUP_EXECUTE");
        } else if (e == ',') {
          doing = DoingNone;
        } else {
          return false;
        }
        break;
      case DoingWorldAssignment:
        if (e == 'r') {
          uniquePushBack("WORLD_READ");
        } else if (e == 'w') {
          uniquePushBack("WORLD_WRITE");
        } else if (e == 'x') {
          uniquePushBack("WORLD_EXECUTE");
        } else if (e == ',') {
          doing = DoingNone;
        } else {
          return false;
        }
        break;
    }
  }
  if (doing != DoingOwnerAssignment && doing != DoingGroupAssignment && doing != DoingWorldAssignment) {
    return false;
  }

  std::ostringstream oss;
  for (auto i = 0u; i < parsedPermissions.size(); i++) {
    if (i != 0) {
      oss << ';';
    }
    oss << parsedPermissions[i];
  }

  parsedPermissionsVar = oss.str();
  return true;
}

int do_install(
  int ac,
  char const* const* av)
{
#ifdef CMAKE_BOOTSTRAP
  std::cerr << "This cmake does not support --install\n";
  return -1;
#else
  assert(1 < ac);

  std::string config;
  std::string component;
  std::string defaultDirectoryPermissions;
  std::string prefix;
  std::string dir;
  int jobs = 0;
  bool strip = false;
  bool verbose = cmSystemTools::HasEnv("VERBOSE");

  auto jLambda = extract_job_number_lambda_builder(dir, jobs, "-j");
  auto parallelLambda = extract_job_number_lambda_builder(dir, jobs, "--parallel");

  auto verboseLambda = [&](std::string const&) -> bool {
    verbose = true;
    return true;
  };

  using CommandArgument = cmCommandLineArgument<bool(std::string const& value)>;

  std::vector<CommandArgument> arguments = {
    CommandArgument{ "--config", CommandArgument::Values::One, CommandArgument::setToValue(config) },
    CommandArgument{ "--component", CommandArgument::Values::One, CommandArgument::setToValue(component) },
    CommandArgument{ "--default-directory-permissions", CommandArgument::Values::One,
                     CommandArgument::setToValue(defaultDirectoryPermissions) },
    CommandArgument{ "-j", CommandArgument::Values::One, jLambda },
    CommandArgument{ "--parallel", CommandArgument::Values::One, parallelLambda },
    CommandArgument{ "--prefix", CommandArgument::Values::One, CommandArgument::setToValue(prefix) },
    CommandArgument{ "--strip", CommandArgument::Values::Zero, CommandArgument::setToTrue(strip) },
    CommandArgument{ "-v", CommandArgument::Values::Zero, verboseLambda },
    CommandArgument{ "--verbose", CommandArgument::Values::Zero, verboseLambda }
  };

  if (ac >= 3) {
    dir = cmSystemTools::ToNormalizedPathOnDisk(av[2]);

    std::vector<std::string> inputArgs;
    inputArgs.reserve(ac - 3);
    cm::append(inputArgs, av + 3, av + ac);
    for (decltype(inputArgs.size()) i = 0; i < inputArgs.size(); ++i) {
      std::string const& arg = inputArgs[i];
      bool matched = false;
      bool parsed = false;
      for (auto const& m : arguments) {
        matched = m.matches(arg);
        if (matched) {
          parsed = m.parse(arg, i, inputArgs);
          break;
        }
      }
      if (!(matched && parsed)) {
        dir.clear();
        if (!matched) {
          std::cerr << "Unknown argument " << arg << std::endl;
        }
        break;
      }
    }
  }

  if (dir.empty()) {
    /* clang-format off */
    std::cerr <<
      "Usage: cmake --install <dir> [options]\n"
      "Options:\n"
      "  <dir>              = Project binary directory to install.\n"
      "  --config <cfg>     = For multi-configuration tools, choose <cfg>.\n"
      "  --component <comp> = Component-based install. Only install <comp>.\n"
      "  --default-directory-permissions <permission> \n"
      "     Default install permission. Use default permission <permission>.\n"
      "  -j <jobs> --parallel <jobs>\n"
      "     Build in parallel using the given number of jobs. \n"
      "     The CMAKE_INSTALL_PARALLEL_LEVEL environment variable\n"
      "     specifies a default parallel level when this option is not given.\n"
      "  --prefix <prefix>  = The installation prefix CMAKE_INSTALL_PREFIX.\n"
      "  --strip            = Performing install/strip.\n"
      "  -v --verbose       = Enable verbose output.\n"
      ;
    /* clang-format on */
    return 1;
  }

  std::vector<std::string> args{ av[0] };

  if (!prefix.empty()) {
    args.emplace_back("-DCMAKE_INSTALL_PREFIX=" + prefix);
  }

  if (!component.empty()) {
    args.emplace_back("-DCMAKE_INSTALL_COMPONENT=" + component);
  }

  if (strip) {
    args.emplace_back("-DCMAKE_INSTALL_DO_STRIP=1");
  }

  if (!defaultDirectoryPermissions.empty()) {
    std::string parsedPermissionsVar;
    if (!parse_default_directory_permissions(defaultDirectoryPermissions, parsedPermissionsVar)) {
      std::cerr << "--default-directory-permissions is in incorrect format" << std::endl;
      return 1;
    }
    args.emplace_back("-DCMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS=" + parsedPermissionsVar);
  }

  args.emplace_back("-P");

  // cmInstrumentation instrumentation(dir);
  auto handler = cmInstallScriptHandler(dir, component, config, args);
  int ret = 0;
  if (!jobs && handler.IsParallel()) {
    jobs = 1;
    auto envvar = cmSystemTools::GetEnvVar("CMAKE_INSTALL_PARALLEL_LEVEL");
    if (envvar.has_value()) {
      jobs = extract_job_number("", envvar.value());
      if (jobs < 1) {
        std::cerr << "Value of CMAKE_INSTALL_PARALLEL_LEVEL environment"
                     " variable must be a positive integer.\n";
        return 1;
      }
    }
  }

  auto doInstall = [&handler, &verbose, &jobs]() -> int {
    int ret_ = 0;
    if (handler.IsParallel()) {
      ret_ = handler.Install(jobs);
    } else {
      for (auto const& cmd : handler.GetCommands()) {
        CMake cm(CMake::RoleScript, cmState::Script);
        cmSystemTools::SetMessageCallback(
          [&cm](std::string const& msg, cmMessageMetadata const& md) { cmakemainMessageCallback(msg, md, &cm); });
        cm.SetProgressCallback(
          [&cm](std::string const& msg, float prog) { cmakemainProgressCallback(msg, prog, &cm); });
        cm.SetHomeDirectory("");
        cm.SetHomeOutputDirectory("");
        cm.SetDebugOutputOn(verbose);
        cm.SetWorkingMode(CMake::SCRIPT_MODE, CMake::CommandFailureAction::FATAL_ERROR);
        ret_ = int(bool(cm.Run(cmd)));
      }
    }
    return int(ret_ > 0);
  };

  std::vector<std::string> cmd;
  cm::append(cmd, av, av + ac);
  ret = doInstall();
  // ret = instrumentation.InstrumentCommand("cmakeInstall", cmd, [doInstall]() { return doInstall(); });
  // instrumentation.CollectTimingData(cmInstrumentationQuery::Hook::PostInstall);
  return ret;
#endif
}

int do_workflow(
  int ac,
  char const* const* av)
{
#ifdef CMAKE_BOOTSTRAP
  std::cerr << "This cmake does not support --workflow\n";
  return -1;
#else
  using WorkflowListPresets = CMake::WorkflowListPresets;
  using WorkflowFresh = CMake::WorkflowFresh;
  std::string presetName;
  auto listPresets = WorkflowListPresets::No;
  auto fresh = WorkflowFresh::No;

  using CommandArgument = cmCommandLineArgument<bool(std::string const& value)>;

  std::vector<CommandArgument> arguments = {
    CommandArgument{ "--preset", CommandArgument::Values::One, CommandArgument::setToValue(presetName) },
    CommandArgument{ "--list-presets", CommandArgument::Values::Zero,
                     [&listPresets](std::string const&) -> bool {
                       listPresets = WorkflowListPresets::Yes;
                       return true;
                     } },
    CommandArgument{ "--fresh", CommandArgument::Values::Zero,
                     [&fresh](std::string const&) -> bool {
                       fresh = WorkflowFresh::Yes;
                       return true;
                     } },
  };

  std::vector<std::string> inputArgs;

  inputArgs.reserve(ac - 2);
  cm::append(inputArgs, av + 2, av + ac);

  decltype(inputArgs.size()) i = 0;
  for (; i < inputArgs.size(); ++i) {
    std::string const& arg = inputArgs[i];
    bool matched = false;
    bool parsed = false;
    for (auto const& m : arguments) {
      matched = m.matches(arg);
      if (matched) {
        parsed = m.parse(arg, i, inputArgs);
        break;
      }
    }
    if (!matched && i == 0) {
      inputArgs.insert(inputArgs.begin(), "--preset");
      matched = true;
      parsed = arguments[0].parse("--preset", i, inputArgs);
    }
    if (!(matched && parsed)) {
      if (!matched) {
        presetName.clear();
        listPresets = WorkflowListPresets::No;
        std::cerr << "Unknown argument " << arg << std::endl;
      }
      break;
    }
  }

  if (presetName.empty() && listPresets == WorkflowListPresets::No) {
    /* clang-format off */
    std::cerr <<
      "Usage: cmake --workflow <options>\n"
      "Options:\n"
      "  --preset <preset> = Workflow preset to execute.\n"
      "  --list-presets    = List available workflow presets.\n"
      "  --fresh           = Configure a fresh build tree, removing any "
                            "existing cache file.\n"
      ;
    /* clang-format on */
    return 1;
  }

  CMake cm(CMake::RoleInternal, cmState::Project);
  cmSystemTools::SetMessageCallback(
    [&cm](std::string const& msg, cmMessageMetadata const& md) { cmakemainMessageCallback(msg, md, &cm); });
  cm.SetProgressCallback([&cm](std::string const& msg, float prog) { cmakemainProgressCallback(msg, prog, &cm); });

  return cm.Workflow(presetName, listPresets, fresh);
#endif
}

int do_open(
  int ac,
  char const* const* av)
{
#ifdef CMAKE_BOOTSTRAP
  std::cerr << "This cmake does not support --open\n";
  return -1;
#else
  std::string dir;

  enum Doing
  {
    DoingNone,
    DoingDir,
  };
  Doing doing = DoingDir;
  for (int i = 2; i < ac; ++i) {
    switch (doing) {
      case DoingDir:
        dir = cmSystemTools::ToNormalizedPathOnDisk(av[i]);
        doing = DoingNone;
        break;
      default:
        std::cerr << "Unknown argument " << av[i] << std::endl;
        dir.clear();
        break;
    }
  }
  if (dir.empty()) {
    std::cerr << "Usage: cmake --open <dir>\n";
    return 1;
  }

  CMake cm(CMake::RoleInternal, cmState::Unknown);
  cmSystemTools::SetMessageCallback(
    [&cm](std::string const& msg, cmMessageMetadata const& md) { cmakemainMessageCallback(msg, md, &cm); });
  cm.SetProgressCallback([&cm](std::string const& msg, float prog) { cmakemainProgressCallback(msg, prog, &cm); });
  return cm.Open(dir, CMake::DryRun::No) ? 0 : 1;
#endif
}
} // namespace

int main(
  int ac,
  char const* const* av)
{
  std::cout << "New CMake\n";

  FunctionTrace f(__func__);

  try {
    cmSystemTools::EnsureStdPipes();

    // Replace streambuf so we can output Unicode to console
    auto consoleBuf = cm::make_unique<cmConsoleBuf>();
    consoleBuf->SetUTF8Pipes();

    cmsys::Encoding::CommandLineArguments args = cmsys::Encoding::CommandLineArguments::Main(ac, av);
    ac = args.argc();
    av = args.argv();

    //    TODO: do we really need an asynchronous i/o polling system for a build system?
    cmSystemTools::InitializeLibUV();
    cmSystemTools::FindCMakeResources(av[0]);
    if (ac > 1) {
      if (strcmp(av[1], "--build") == 0) {
        return do_build(ac, av);
      }
      if (strcmp(av[1], "--install") == 0) {
        return do_install(ac, av);
      }
      if (strcmp(av[1], "--open") == 0) {
        return do_open(ac, av);
      }
      if (strcmp(av[1], "--workflow") == 0) {
        return do_workflow(ac, av);
      }
      if (strcmp(av[1], "-E") == 0) {
        return do_command(ac, av, std::move(consoleBuf));
      }
      if (strcmp(av[1], "--print-config-dir") == 0) {
        std::cout << cmSystemTools::ConvertToOutputPath(
                       cmSystemTools::GetCMakeConfigDirectory().value_or(std::string()))
                  << std::endl;
        return 0;
      }
    }
    int exitCode = do_cmake(ac, av);
#ifndef CMAKE_BOOTSTRAP
    cmDynamicLoader::FlushCache();
#endif
    if (uv_loop_t* loop = uv_default_loop()) {
      uv_loop_close(loop);
    }
    return exitCode;
  } catch (CMakeException& e) {
    CMakeMessage::error(e.what());
  } catch (std::exception& e) {
    CMakeMessage::error(e.what());
  }
  return 1;
}
